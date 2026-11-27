/*
 * navpath: walks on the server's navmesh offline (docs/orders.md "Road links from the navmesh").
 *
 * The worldserver's path finder only sees the navmesh tiles of loaded grids: a walk from a grid with a bot in it to a
 * grid without one is a straight line ("not using a path"), so the module cannot prove at run time that two parts of
 * the road map that mod-playerbots never linked are joined on the ground (the start mesa of the Tauren and the plains
 * below it: no link in its travel node graph, and the plains' grid is empty while a recruit stands on the mesa). This
 * tool loads the tiles from the mmaps folder itself and answers with the core's own Detour code, with the filter the
 * core gives bots (ground and water, not steep ground, lava or slime; water costs five times).
 *
 *   navpath <data dir> <map id>
 *
 * Reads lines "x1 y1 z1 x2 y2 z2" (world coordinates) from stdin and writes one line per query:
 *   OK <yards> <points> x y z;x y z;...   the corners of the walk (findStraightPath), the first and last at the ends
 *   FAIL <reason>
 * or "ISLAND x y z": the size and bounds of the part of the navmesh a walk from there reaches.
 * Built by tools\navpath\build.ps1 against the core checkout's deps\recastnavigation.
 */

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "DetourCommon.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // MapDefines.h of the core.
    constexpr uint32_t MmapMagic = 0x4d4d4150;
    constexpr uint32_t MmapVersion = 20;
    struct TileHeader
    {
        uint32_t mmapMagic;
        uint32_t dtVersion;
        uint32_t mmapVersion;
        uint32_t size;
        char usesLiquids;
        char padding[3];
        char recastConfig[36];
    };
    static_assert(sizeof(TileHeader) == 56, "MmapTileHeader size");
    enum : unsigned short
    {
        NavGround = 0x01,
        NavMagma = 0x02,
        NavSlime = 0x04,
        NavWater = 0x08,
        NavGroundSteep = 0x10,
    };

    constexpr int MaxPolys = 8192;
    constexpr int MaxCorners = 2048;

    std::filesystem::path g_mmaps;
    uint32_t g_map = 0;
    dtNavMesh* g_mesh = nullptr;
    dtNavMeshQuery* g_query = nullptr;
    // Every tile file of the map by the tile's own coordinates (dtMeshHeader x, y), read from the files' headers.
    std::map<std::pair<int, int>, std::filesystem::path> g_files;
    std::set<std::pair<int, int>> g_loaded;

    bool IndexTiles()
    {
        char prefix[8];
        std::snprintf(prefix, sizeof(prefix), "%03u", g_map);
        for (auto const& entry : std::filesystem::directory_iterator(g_mmaps))
        {
            std::string const name = entry.path().filename().string();
            if (name.size() != 14 || name.compare(0, 3, prefix) != 0 || entry.path().extension() != ".mmtile")
                continue;
            FILE* file = std::fopen(entry.path().string().c_str(), "rb");
            if (!file)
                continue;
            TileHeader header{};
            dtMeshHeader mesh{};
            bool const ok = std::fread(&header, sizeof(header), 1, file) == 1 && header.mmapMagic == MmapMagic &&
                header.mmapVersion == MmapVersion && std::fread(&mesh, sizeof(mesh), 1, file) == 1;
            std::fclose(file);
            if (ok)
                g_files[{ mesh.x, mesh.y }] = entry.path();
        }
        return !g_files.empty();
    }

    void LoadTile(int tx, int ty)
    {
        if (!g_loaded.insert({ tx, ty }).second)
            return;
        auto const found = g_files.find({ tx, ty });
        if (found == g_files.end())
            return;
        FILE* file = std::fopen(found->second.string().c_str(), "rb");
        if (!file)
            return;
        TileHeader header{};
        if (std::fread(&header, sizeof(header), 1, file) != 1)
        {
            std::fclose(file);
            return;
        }
        unsigned char* data = static_cast<unsigned char*>(dtAlloc(header.size, DT_ALLOC_PERM));
        if (std::fread(data, header.size, 1, file) != 1)
        {
            std::fclose(file);
            dtFree(data);
            return;
        }
        std::fclose(file);
        if (dtStatusFailed(g_mesh->addTile(data, int(header.size), DT_TILE_FREE_DATA, 0, nullptr)))
            dtFree(data);
    }

    // The tiles a walk between two points may touch: every tile within two of the straight line.
    void LoadTilesAlong(float const* a, float const* b)
    {
        float const length = dtVdist2D(a, b);
        int const steps = std::max(1, int(length / 100.0f));
        for (int i = 0; i <= steps; ++i)
        {
            float point[3];
            dtVlerp(point, a, b, float(i) / float(steps));
            int tx = 0, ty = 0;
            g_mesh->calcTileLoc(point, &tx, &ty);
            for (int dx = -2; dx <= 2; ++dx)
                for (int dy = -2; dy <= 2; ++dy)
                    LoadTile(tx + dx, ty + dy);
        }
    }

    std::string Query(float x1, float y1, float z1, float x2, float y2, float z2)
    {
        // Detour's axes: (y, z, x) of the world.
        float const start[3] = { y1, z1, x1 };
        float const end[3] = { y2, z2, x2 };
        LoadTilesAlong(start, end);

        dtQueryFilter filter;
        filter.setIncludeFlags(NavGround | NavWater);
        // (NAVPATH_STEEP=1: steep ground too, as the core gives creatures: where a way runs only over it.)
        static bool const steep = std::getenv("NAVPATH_STEEP") && *std::getenv("NAVPATH_STEEP") == '1';
        filter.setExcludeFlags(NavMagma | NavSlime | (steep ? 0 : NavGroundSteep));
        filter.setAreaCost(NavWater, 5.0f);

        float const extents[3] = { 3.0f, 5.0f, 3.0f };
        dtPolyRef startRef = 0, endRef = 0;
        float startNear[3], endNear[3];
        g_query->findNearestPoly(start, extents, &filter, &startRef, startNear);
        g_query->findNearestPoly(end, extents, &filter, &endRef, endNear);
        if (!startRef)
            return "FAIL no navmesh at the start";
        if (!endRef)
            return "FAIL no navmesh at the end";

        static dtPolyRef polys[MaxPolys];
        int polyCount = 0;
        dtStatus const status = g_query->findPath(startRef, endRef, startNear, endNear, &filter, polys, &polyCount, MaxPolys);
        if (dtStatusFailed(status) || polyCount <= 0)
            return "FAIL no path";
        if (polys[polyCount - 1] != endRef)
        {
            // Where the walk gets to: the point of its last polygon nearest to the end.
            float closest[3];
            g_query->closestPointOnPoly(polys[polyCount - 1], endNear, closest, nullptr);
            std::ostringstream why;
            why.setf(std::ios::fixed);
            why.precision(1);
            why << "FAIL no way (the walk ends short of the end, at " << closest[2] << ' ' << closest[0] << ' ' << closest[1] << ", "
                << dtVdist(closest, endNear) << " yd from it)";
            return why.str();
        }
        if (status & DT_BUFFER_TOO_SMALL)
            return "FAIL too long";

        static float corners[MaxCorners * 3];
        int cornerCount = 0;
        g_query->findStraightPath(startNear, endNear, polys, polyCount, corners, nullptr, nullptr, &cornerCount, MaxCorners);
        if (cornerCount < 2)
            return "FAIL no corners";
        // The corners, and between them a point every StepYards with the navmesh's own height there (a straight stretch
        // between two corners goes over hills and down slopes).
        constexpr float StepYards = 5.0f;
        float length = 0.0f;
        int count = 0;
        std::ostringstream points;
        points.setf(std::ios::fixed);
        points.precision(1);
        auto put = [&](float const* p)
        {
            points << (count++ ? ";" : "") << p[2] << ' ' << p[0] << ' ' << p[1];
        };
        put(&corners[0]);
        float const ground[3] = { 2.0f, 8.0f, 2.0f };
        for (int i = 1; i < cornerCount; ++i)
        {
            float const* a = &corners[(i - 1) * 3];
            float const* b = &corners[i * 3];
            length += dtVdist(a, b);
            int const steps = std::max(1, int(std::ceil(dtVdist2D(a, b) / StepYards)));
            for (int s = 1; s < steps; ++s)
            {
                float p[3];
                dtVlerp(p, a, b, float(s) / float(steps));
                dtPolyRef ref = 0;
                float onMesh[3];
                if (dtStatusSucceed(g_query->findNearestPoly(p, ground, &filter, &ref, onMesh)) && ref)
                    p[1] = onMesh[1];
                put(p);
            }
            put(b);
        }
        std::ostringstream line;
        line.setf(std::ios::fixed);
        line.precision(1);
        line << "OK " << length << ' ' << count << ' ' << points.str();
        return line.str();
    }
}

namespace
{
    // "ISLAND x y z": the part of the navmesh a bot can walk to from there (the bots' filter): how many polygons, and its
    // bounds. A part that holds a boss and is joined to nothing else is a place no walk leads to.
    std::string Island(float x, float y, float z)
    {
        float const point[3] = { y, z, x };
        LoadTilesAlong(point, point);
        dtQueryFilter filter;
        filter.setIncludeFlags(NavGround | NavWater);
        static bool const steep = std::getenv("NAVPATH_STEEP") && *std::getenv("NAVPATH_STEEP") == '1';
        filter.setExcludeFlags(NavMagma | NavSlime | (steep ? 0 : NavGroundSteep));
        float const extents[3] = { 3.0f, 5.0f, 3.0f };
        dtPolyRef start = 0;
        float near[3];
        g_query->findNearestPoly(point, extents, &filter, &start, near);
        if (!start)
            return "FAIL no navmesh there";
        std::set<dtPolyRef> seen{ start };
        std::vector<dtPolyRef> open{ start };
        float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
        float lowest[3] = { 0.0f, 1e9f, 0.0f };
        while (!open.empty() && seen.size() < 200000)
        {
            dtPolyRef const ref = open.back();
            open.pop_back();
            dtMeshTile const* tile = nullptr;
            dtPoly const* poly = nullptr;
            if (dtStatusFailed(g_mesh->getTileAndPolyByRef(ref, &tile, &poly)))
                continue;
            for (int i = 0; i < poly->vertCount; ++i)
            {
                float const* v = &tile->verts[poly->verts[i] * 3];
                for (int k = 0; k < 3; ++k)
                {
                    lo[k] = std::min(lo[k], v[k]);
                    hi[k] = std::max(hi[k], v[k]);
                }
                if (v[1] < lowest[1])
                    dtVcopy(lowest, v);
            }
            for (unsigned int l = poly->firstLink; l != DT_NULL_LINK; l = tile->links[l].next)
            {
                dtPolyRef const next = tile->links[l].ref;
                if (!next || seen.count(next))
                    continue;
                dtMeshTile const* nextTile = nullptr;
                dtPoly const* nextPoly = nullptr;
                if (dtStatusFailed(g_mesh->getTileAndPolyByRef(next, &nextTile, &nextPoly)) || !filter.passFilter(next, nextTile, nextPoly))
                    continue;
                seen.insert(next);
                open.push_back(next);
            }
        }
        std::ostringstream out;
        out.setf(std::ios::fixed);
        out.precision(1);
        out << "ISLAND " << seen.size() << " polygons, x " << lo[2] << " .. " << hi[2] << ", y " << lo[0] << " .. " << hi[0] << ", z "
            << lo[1] << " .. " << hi[1] << "; its lowest point " << lowest[2] << ' ' << lowest[0] << ' ' << lowest[1];
        return out.str();
    }
}

namespace
{
    // "BELOW x y z r": the polygons within r yd (2D) of a point and up to 60 yd below it: where a drop from there lands,
    // with each polygon's flags (1 ground, 2 steep, 4 water, 8 magma, 16 slime).
    std::string Below(float x, float y, float z, float r)
    {
        float const point[3] = { y, z, x };
        LoadTilesAlong(point, point);
        dtQueryFilter filter;
        filter.setIncludeFlags(0xffff);
        float const center[3] = { y, z - 30.0f, x };
        float const half[3] = { r, 30.0f, r };
        dtPolyRef found[256];
        int count = 0;
        g_query->queryPolygons(center, half, &filter, found, &count, 256);
        std::ostringstream out;
        out.setf(std::ios::fixed);
        out.precision(1);
        out << "BELOW " << count << " polygons:";
        for (int i = 0; i < count; ++i)
        {
            dtMeshTile const* tile = nullptr;
            dtPoly const* poly = nullptr;
            if (dtStatusFailed(g_mesh->getTileAndPolyByRef(found[i], &tile, &poly)))
                continue;
            float near[3];
            g_query->closestPointOnPoly(found[i], point, near, nullptr);
            out << " [" << near[2] << ' ' << near[0] << ' ' << near[1] << " f" << int(poly->flags) << ']';
        }
        return out.str();
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: navpath <data dir> <map id>  (queries \"x1 y1 z1 x2 y2 z2\" on stdin)\n";
        return 2;
    }
    g_mmaps = std::filesystem::path(argv[1]) / "mmaps";
    g_map = uint32_t(std::stoul(argv[2]));

    char name[16];
    std::snprintf(name, sizeof(name), "%03u.mmap", g_map);
    FILE* file = std::fopen((g_mmaps / name).string().c_str(), "rb");
    if (!file)
    {
        std::cerr << "no " << name << " in " << g_mmaps.string() << "\n";
        return 1;
    }
    dtNavMeshParams params{};
    bool const read = std::fread(&params, sizeof(params), 1, file) == 1;
    std::fclose(file);
    g_mesh = dtAllocNavMesh();
    if (!read || dtStatusFailed(g_mesh->init(&params)))
    {
        std::cerr << "cannot read " << name << "\n";
        return 1;
    }
    if (!IndexTiles())
    {
        std::cerr << "no tiles of map " << g_map << "\n";
        return 1;
    }
    g_query = dtAllocNavMeshQuery();
    if (dtStatusFailed(g_query->init(g_mesh, 65535)))
    {
        std::cerr << "cannot set up the query\n";
        return 1;
    }

    std::string line;
    while (std::getline(std::cin, line))
    {
        std::istringstream in(line);
        if (line.rfind("BELOW ", 0) == 0)
        {
            std::string word;
            float p[4];
            if (in >> word >> p[0] >> p[1] >> p[2] >> p[3])
                std::cout << Below(p[0], p[1], p[2], p[3]) << std::endl;
            else
                std::cout << "FAIL bad query" << std::endl;
            continue;
        }
        if (line.rfind("ISLAND ", 0) == 0)
        {
            std::string word;
            float p[3];
            if (in >> word >> p[0] >> p[1] >> p[2])
                std::cout << Island(p[0], p[1], p[2]) << std::endl;
            else
                std::cout << "FAIL bad query" << std::endl;
            continue;
        }
        float v[6];
        if (!(in >> v[0] >> v[1] >> v[2] >> v[3] >> v[4] >> v[5]))
        {
            std::cout << "FAIL bad query" << std::endl;
            continue;
        }
        std::cout << Query(v[0], v[1], v[2], v[3], v[4], v[5]) << std::endl;
    }
    return 0;
}
