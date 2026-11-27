#include "DungeonNav.h"
#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "Log.h"
#include "Map.h"
#include "StringFormat.h"
#include "Timer.h"
#include "MapDefines.h"
#include "Object.h"
#include "World.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

using namespace GuildAdventure;

namespace
{
    constexpr int MaxPolys = 8192;
    constexpr int MaxCorners = 2048;
    constexpr int QueryNodes = 65535;
    // A drop: from the edge of one part of the navmesh down onto another part below it, as players jump down a ledge.
    // At most this far across and between these heights (a fall of 12 yd hurts nobody).
    constexpr float DropAcross = 6.0f;
    constexpr float DropMin = 2.0f;
    constexpr float DropMax = 12.0f;
    // Into deep water a player jumps from higher up (no fall damage there): the plateau of the Wailing Caverns' Lord
    // Serpentis lies 28 yd above the pool below it.
    constexpr float WaterDropMax = 50.0f;
    constexpr float WaterDropAcross = 15.0f;
    // Onto ground from higher up than DropMax a player jumps too where nothing else leads down, and takes some damage:
    // the plateau of Lord Serpentis and Verdan falls 27 to 29 yd onto the floor of the cave below, and no walk leaves it.
    // A deeper drop must land in water (DropClear asks the map's liquid: the navmesh marks no polygon as water there).
    constexpr float GroundDropMax = 30.0f;
    // Across water a player swims: from the edge of a part to a part at about the same height (the shore of the
    // island Lady Sarevess stands on in Blackfathom Deeps, deep water all round it). The water between the two is
    // looked at when the way is asked for (DropClear: water all along the line).
    constexpr float SwimMin = 4.0f;
    constexpr float SwimMax = 60.0f;
    constexpr float SwimRise = 5.0f;
    constexpr float DropSpacing = 8.0f;   // drops of one pair of parts at least this far apart
    constexpr std::size_t DropsPerPair = 24;

    // Detour's axes: (y, z, x) of the world.
    NavPoint ToWorld(float const* p) { return NavPoint{ p[2], p[0], p[1] }; }

    struct Drop
    {
        uint32 from = 0, to = 0;  // parts of the navmesh (islands)
        float top[3]{};           // on the edge of the upper part, half a yard inside it
        float land[3]{};          // on the part below
        dtPolyRef topRef = 0, landRef = 0;
        int8 clear = -1;          // the drop's line of sight: -1 not looked at yet
        float outward[2]{};       // the direction a fall from the edge goes (Detour x and z, a unit vector)
        std::string blockedBy;    // what DropClear found in the way (for `.ga admin navway`)
        bool needsWater = false;  // deeper than a fall onto ground may be: it must land in water
        bool swim = false;        // not a fall: a swim across water to a part at about the same height
    };

    struct MapNav
    {
        dtNavMesh* mesh = nullptr;
        dtNavMeshQuery* query = nullptr;
        bool ok = false;
        std::unordered_map<dtPolyRef, uint32> island; // part of the navmesh each walkable polygon belongs to
        std::vector<Drop> drops;
        // The drops that leave each part (indices into drops), and the pairs of parts no drops joined lately (when, by
        // getMSTime()). Zul'Gurub's navmesh has 53833 drops between 4997 parts: a search that looked at every drop for
        // every drop it reached took a second of the world thread each time, every second for two minutes while the raid
        // had no way to High Priestess Jeklik (2026-11-27).
        std::unordered_map<uint32, std::vector<uint32>> dropsFrom;
        std::unordered_map<uint64, uint32> failedPairs;
        ~MapNav()
        {
            if (query)
                dtFreeNavMeshQuery(query);
            if (mesh)
                dtFreeNavMesh(mesh);
        }
        uint32 IslandOf(dtPolyRef ref) const
        {
            auto const found = island.find(ref);
            return found == island.end() ? 0 : found->second;
        }
    };

    std::mutex lock;
    std::unordered_map<uint32, std::unique_ptr<MapNav>> maps;

    void BotFilter(dtQueryFilter& filter)
    {
        filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
        filter.setExcludeFlags(NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP);
        filter.setAreaCost(NAV_WATER, 5.0f);
    }

    // The parts of the navmesh a walk with the bots' filter joins, and the drops between them.
    void FindIslands(MapNav& nav)
    {
        dtQueryFilter filter;
        BotFilter(filter);
        dtNavMesh const* mesh = nav.mesh;
        uint32 next = 0;
        std::vector<dtPolyRef> open;
        for (int t = 0; t < mesh->getMaxTiles(); ++t)
        {
            dtMeshTile const* tile = mesh->getTile(t);
            if (!tile || !tile->header)
                continue;
            dtPolyRef const base = mesh->getPolyRefBase(tile);
            for (int p = 0; p < tile->header->polyCount; ++p)
            {
                dtPolyRef const ref = base | dtPolyRef(p);
                dtPoly const* poly = &tile->polys[p];
                if (poly->getType() != DT_POLYTYPE_GROUND || !filter.passFilter(ref, tile, poly) || nav.island.count(ref))
                    continue;
                uint32 const id = ++next;
                nav.island[ref] = id;
                open.assign(1, ref);
                while (!open.empty())
                {
                    dtPolyRef const at = open.back();
                    open.pop_back();
                    dtMeshTile const* atTile = nullptr;
                    dtPoly const* atPoly = nullptr;
                    mesh->getTileAndPolyByRefUnsafe(at, &atTile, &atPoly);
                    for (unsigned int l = atPoly->firstLink; l != DT_NULL_LINK; l = atTile->links[l].next)
                    {
                        dtPolyRef const nb = atTile->links[l].ref;
                        if (!nb || nav.island.count(nb))
                            continue;
                        dtMeshTile const* nbTile = nullptr;
                        dtPoly const* nbPoly = nullptr;
                        mesh->getTileAndPolyByRefUnsafe(nb, &nbTile, &nbPoly);
                        if (nbPoly->getType() != DT_POLYTYPE_GROUND || !filter.passFilter(nb, nbTile, nbPoly))
                            continue;
                        nav.island[nb] = id;
                        open.push_back(nb);
                    }
                }
            }
        }
        // Drops: from every border edge of a walkable polygon (no neighbour) down onto a polygon of another part.
        std::unordered_map<uint64, std::vector<std::size_t>> perPair;
        for (int t = 0; t < mesh->getMaxTiles(); ++t)
        {
            dtMeshTile const* tile = mesh->getTile(t);
            if (!tile || !tile->header)
                continue;
            dtPolyRef const base = mesh->getPolyRefBase(tile);
            for (int p = 0; p < tile->header->polyCount; ++p)
            {
                dtPolyRef const ref = base | dtPolyRef(p);
                uint32 const from = nav.IslandOf(ref);
                if (!from)
                    continue;
                dtPoly const* poly = &tile->polys[p];
                float center[3] = { 0.0f, 0.0f, 0.0f };
                for (int v = 0; v < poly->vertCount; ++v)
                    dtVadd(center, center, &tile->verts[poly->verts[v] * 3]);
                dtVscale(center, center, 1.0f / float(poly->vertCount));
                for (int e = 0; e < poly->vertCount; ++e)
                {
                    if (poly->neis[e] != 0)
                        continue;
                    float const* a = &tile->verts[poly->verts[e] * 3];
                    float const* b = &tile->verts[poly->verts[(e + 1) % poly->vertCount] * 3];
                    for (float s : { 0.25f, 0.5f, 0.75f })
                    {
                        float edge[3], top[3], inward[3];
                        dtVlerp(edge, a, b, s);
                        dtVsub(inward, center, edge);
                        inward[1] = 0.0f;
                        float const length = dtVlen(inward);
                        if (length < 0.01f)
                            continue;
                        dtVmad(top, edge, inward, std::min(0.6f, length) / length);
                        // Where a fall from the edge comes down: a little out from the cliff's foot (a player walks off
                        // the edge and lands in front of the face; straight under the edge a line from above runs through
                        // the ledge's own lip).
                        float out[3];
                        dtVmad(out, edge, inward, -2.5f / length);
                        float const below[3] = { edge[0], edge[1] - (DropMin + DropMax) / 2.0f, edge[2] };
                        float const half[3] = { DropAcross, (DropMax - DropMin) / 2.0f, DropAcross };
                        dtPolyRef found[64];
                        int count = 0;
                        nav.query->queryPolygons(below, half, &filter, found, &count, 64);
                        dtPolyRef best = 0;
                        float bestLand[3];
                        float bestAcross = DropAcross;
                        for (int q = 0; q < count; ++q)
                        {
                            uint32 const to = nav.IslandOf(found[q]);
                            if (!to || to == from)
                                continue;
                            float land[3];
                            nav.query->closestPointOnPoly(found[q], out, land, nullptr);
                            float const fall = edge[1] - land[1];
                            float const across = dtVdist2D(edge, land);
                            if (fall < DropMin || fall > DropMax || across > bestAcross)
                                continue;
                            best = found[q];
                            bestAcross = across;
                            dtVcopy(bestLand, land);
                        }
                        // No drop onto ground: one into water below (a polygon of the navmesh that lies on water).
                        if (!best)
                        {
                            float const deep[3] = { edge[0], edge[1] - (DropMax + WaterDropMax) / 2.0f, edge[2] };
                            float const deepHalf[3] = { WaterDropAcross, (WaterDropMax - DropMax) / 2.0f, WaterDropAcross };
                            count = 0;
                            nav.query->queryPolygons(deep, deepHalf, &filter, found, &count, 64);
                            bestAcross = WaterDropAcross;
                            for (int q = 0; q < count; ++q)
                            {
                                uint32 const to = nav.IslandOf(found[q]);
                                if (!to || to == from)
                                    continue;
                                float land[3];
                                nav.query->closestPointOnPoly(found[q], out, land, nullptr);
                                float const fall = edge[1] - land[1];
                                float const across = dtVdist2D(edge, land);
                                if (fall <= DropMax || fall > WaterDropMax || across > bestAcross)
                                    continue;
                                best = found[q];
                                bestAcross = across;
                                dtVcopy(bestLand, land);
                            }
                        }
                        // No drop at all: a swim across water to the shore of another part at about the same height.
                        bool swim = false;
                        if (!best)
                        {
                            float const reach[3] = { edge[0] - inward[0] / length * SwimMax / 2.0f, edge[1],
                                edge[2] - inward[2] / length * SwimMax / 2.0f };
                            float const reachHalf[3] = { SwimMax / 2.0f + 2.0f, SwimRise, SwimMax / 2.0f + 2.0f };
                            count = 0;
                            nav.query->queryPolygons(reach, reachHalf, &filter, found, &count, 64);
                            float bestSwim = SwimMax;
                            for (int q = 0; q < count; ++q)
                            {
                                uint32 const to = nav.IslandOf(found[q]);
                                if (!to || to == from)
                                    continue;
                                float land[3];
                                nav.query->closestPointOnPoly(found[q], edge, land, nullptr);
                                float const across = dtVdist2D(edge, land);
                                if (std::fabs(edge[1] - land[1]) > SwimRise || across < SwimMin || across > bestSwim)
                                    continue;
                                // Out from the edge, not back over the part itself.
                                float dir[3];
                                dtVsub(dir, land, edge);
                                if (dir[0] * -inward[0] + dir[2] * -inward[2] <= 0.0f)
                                    continue;
                                best = found[q];
                                bestSwim = across;
                                dtVcopy(bestLand, land);
                                swim = true;
                            }
                        }
                        if (!best)
                            continue;
                        uint32 const to = nav.IslandOf(best);
                        uint64 const pair = (uint64(from) << 32) | to;
                        std::vector<std::size_t>& kept = perPair[pair];
                        if (kept.size() >= DropsPerPair)
                            continue;
                        bool tooClose = false;
                        for (std::size_t index : kept)
                            tooClose = tooClose || dtVdist(nav.drops[index].top, top) < DropSpacing;
                        if (tooClose)
                            continue;
                        Drop drop;
                        drop.from = from;
                        drop.to = to;
                        drop.needsWater = !swim && top[1] - bestLand[1] > GroundDropMax;
                        drop.swim = swim;
                        drop.outward[0] = -inward[0] / length;
                        drop.outward[1] = -inward[2] / length;
                        dtVcopy(drop.top, top);
                        dtVcopy(drop.land, bestLand);
                        drop.topRef = ref;
                        drop.landRef = best;
                        kept.push_back(nav.drops.size());
                        nav.drops.push_back(drop);
                    }
                }
            }
        }
    }

    // Every tile of the map from the mmaps folder, as MMapMgr reads them (its file layout: an MmapTileHeader, then the
    // Detour tile data). Done once per map: a dungeon's navmesh is a few MB.
    MapNav* Load(uint32 mapId)
    {
        auto found = maps.find(mapId);
        if (found != maps.end())
            return found->second->ok ? found->second.get() : nullptr;
        auto nav = std::make_unique<MapNav>();
        MapNav* result = nullptr;
        std::filesystem::path const folder = std::filesystem::path(sWorld->GetDataPath()) / "mmaps";
        char name[16];
        std::snprintf(name, sizeof(name), "%03u.mmap", mapId);
        dtNavMeshParams params{};
        bool read = false;
        if (FILE* file = std::fopen((folder / name).string().c_str(), "rb"))
        {
            read = std::fread(&params, sizeof(params), 1, file) == 1;
            std::fclose(file);
        }
        nav->mesh = read ? dtAllocNavMesh() : nullptr;
        if (nav->mesh && dtStatusSucceed(nav->mesh->init(&params)))
        {
            char prefix[8];
            std::snprintf(prefix, sizeof(prefix), "%03u", mapId);
            uint32 tiles = 0;
            std::error_code error;
            for (auto const& entry : std::filesystem::directory_iterator(folder, error))
            {
                std::string const file = entry.path().filename().string();
                if (file.size() != 14 || file.compare(0, 3, prefix) != 0 || entry.path().extension() != ".mmtile")
                    continue;
                FILE* in = std::fopen(entry.path().string().c_str(), "rb");
                if (!in)
                    continue;
                MmapTileHeader header;
                if (std::fread(&header, sizeof(header), 1, in) != 1 || header.mmapMagic != MMAP_MAGIC || header.mmapVersion != MMAP_VERSION ||
                    !header.size)
                {
                    std::fclose(in);
                    continue;
                }
                unsigned char* data = static_cast<unsigned char*>(dtAlloc(header.size, DT_ALLOC_PERM));
                bool const got = data && std::fread(data, header.size, 1, in) == 1;
                std::fclose(in);
                if (!got || dtStatusFailed(nav->mesh->addTile(data, int(header.size), DT_TILE_FREE_DATA, 0, nullptr)))
                {
                    if (data)
                        dtFree(data);
                    continue;
                }
                ++tiles;
            }
            nav->query = tiles ? dtAllocNavMeshQuery() : nullptr;
            nav->ok = nav->query && dtStatusSucceed(nav->query->init(nav->mesh, QueryNodes));
            uint32 islands = 0;
            if (nav->ok)
            {
                FindIslands(*nav);
                for (auto const& [ref, id] : nav->island)
                    islands = std::max(islands, id);
                for (std::size_t d = 0; d < nav->drops.size(); ++d)
                    nav->dropsFrom[nav->drops[d].from].push_back(uint32(d));
            }
            LOG_INFO("module.ga", "mod-guild-adventure: the navmesh of map {} for walks inside it: {} tiles, {} parts a walk joins, "
                "{} drops from one part onto another{}", mapId, tiles, islands, nav->drops.size(), nav->ok ? "" : " (cannot be used)");
        }
        if (nav->ok)
            result = nav.get();
        maps[mapId] = std::move(nav);
        return result;
    }

    // The open world's navmesh (the continents): far too big to load whole (Kalimdor's tiles are 470 MB), so its tiles
    // are loaded where a way is asked for, the region around both ends, and kept (up to OpenMaxTiles; then it starts
    // afresh). No parts and drops here: a plain walk on the ground. (Thousand Needles, 2026-11-25: the Document Chests of
    // "Grimtotem Spying" stand on the Grimtotem mesas, 118 yd above the canyon; the navmesh has the way up, 1470 yd
    // round the mesa, the core's path finder with its 148 polygons and its loaded grids has none, the road map has no
    // node up there, and the test guild's leader set the chests aside and rode back to Freewind Post.)
    constexpr std::size_t OpenMaxTiles = 200;
    constexpr float OpenMargin = 800.0f;

    struct OpenNav
    {
        struct TileFile
        {
            std::string path;
            float bmin[3]{}, bmax[3]{};
            bool loaded = false;
        };
        dtNavMesh* mesh = nullptr;
        dtNavMeshQuery* query = nullptr;
        bool ok = false;
        std::vector<TileFile> files;
        std::size_t loaded = 0;
        ~OpenNav()
        {
            if (query)
                dtFreeNavMeshQuery(query);
            if (mesh)
                dtFreeNavMesh(mesh);
        }
    };

    std::unordered_map<uint32, std::unique_ptr<OpenNav>> opens;

    bool IsOpenWorld(uint32 mapId)
    {
        return mapId == 0 || mapId == 1 || mapId == 530 || mapId == 571;
    }

    OpenNav* LoadOpen(uint32 mapId)
    {
        auto found = opens.find(mapId);
        if (found != opens.end() && found->second->loaded <= OpenMaxTiles)
            return found->second->ok ? found->second.get() : nullptr;
        // (First use, or too many tiles: a fresh mesh, the index of its files read again.)
        auto nav = std::make_unique<OpenNav>();
        std::filesystem::path const folder = std::filesystem::path(sWorld->GetDataPath()) / "mmaps";
        char name[16];
        std::snprintf(name, sizeof(name), "%03u.mmap", mapId);
        dtNavMeshParams params{};
        bool read = false;
        if (FILE* file = std::fopen((folder / name).string().c_str(), "rb"))
        {
            read = std::fread(&params, sizeof(params), 1, file) == 1;
            std::fclose(file);
        }
        nav->mesh = read ? dtAllocNavMesh() : nullptr;
        if (nav->mesh && dtStatusSucceed(nav->mesh->init(&params)))
        {
            char prefix[8];
            std::snprintf(prefix, sizeof(prefix), "%03u", mapId);
            std::error_code error;
            for (auto const& entry : std::filesystem::directory_iterator(folder, error))
            {
                std::string const file = entry.path().filename().string();
                if (file.size() != 14 || file.compare(0, 3, prefix) != 0 || entry.path().extension() != ".mmtile")
                    continue;
                FILE* in = std::fopen(entry.path().string().c_str(), "rb");
                if (!in)
                    continue;
                MmapTileHeader header;
                dtMeshHeader mesh;
                bool const got = std::fread(&header, sizeof(header), 1, in) == 1 && header.mmapMagic == MMAP_MAGIC &&
                    header.mmapVersion == MMAP_VERSION && header.size >= sizeof(mesh) && std::fread(&mesh, sizeof(mesh), 1, in) == 1;
                std::fclose(in);
                if (!got)
                    continue;
                OpenNav::TileFile tile;
                tile.path = entry.path().string();
                dtVcopy(tile.bmin, mesh.bmin);
                dtVcopy(tile.bmax, mesh.bmax);
                nav->files.push_back(std::move(tile));
            }
            nav->query = nav->files.empty() ? nullptr : dtAllocNavMeshQuery();
            nav->ok = nav->query && dtStatusSucceed(nav->query->init(nav->mesh, QueryNodes));
            LOG_INFO("module.ga", "mod-guild-adventure: the navmesh of map {} for long walks in the open world: {} tiles, loaded where a "
                "way is asked for{}", mapId, nav->files.size(), nav->ok ? "" : " (cannot be used)");
        }
        OpenNav* result = nav->ok ? nav.get() : nullptr;
        opens[mapId] = std::move(nav);
        return result;
    }

    // The tiles around both ends (world coordinates), OpenMargin yards beyond them.
    void LoadOpenTiles(OpenNav& nav, float ax, float ay, float bx, float by)
    {
        float const minX = std::min(ax, bx) - OpenMargin, maxX = std::max(ax, bx) + OpenMargin;
        float const minY = std::min(ay, by) - OpenMargin, maxY = std::max(ay, by) + OpenMargin;
        for (OpenNav::TileFile& tile : nav.files)
        {
            // (Detour's axes: bmin[0] is the world's y, bmin[2] its x.)
            if (tile.loaded || tile.bmax[2] < minX || tile.bmin[2] > maxX || tile.bmax[0] < minY || tile.bmin[0] > maxY)
                continue;
            tile.loaded = true;
            FILE* in = std::fopen(tile.path.c_str(), "rb");
            if (!in)
                continue;
            MmapTileHeader header;
            unsigned char* data = nullptr;
            bool got = std::fread(&header, sizeof(header), 1, in) == 1 && header.size;
            if (got)
            {
                data = static_cast<unsigned char*>(dtAlloc(header.size, DT_ALLOC_PERM));
                got = data && std::fread(data, header.size, 1, in) == 1;
            }
            std::fclose(in);
            if (!got || dtStatusFailed(nav.mesh->addTile(data, int(header.size), DT_TILE_FREE_DATA, 0, nullptr)))
            {
                if (data)
                    dtFree(data);
                continue;
            }
            ++nav.loaded;
        }
    }

    // One walk inside a part of the navmesh: its corners (world coordinates) appended, its yards added.
    bool Leg(dtNavMeshQuery* query, dtPolyRef fromRef, float const* from, dtPolyRef toRef, float const* to, std::vector<NavPoint>& corners,
        float& walked)
    {
        dtQueryFilter filter;
        BotFilter(filter);
        static dtPolyRef polys[MaxPolys];
        int polyCount = 0;
        dtStatus const status = query->findPath(fromRef, toRef, from, to, &filter, polys, &polyCount, MaxPolys);
        if (dtStatusFailed(status) || polyCount <= 0 || polys[polyCount - 1] != toRef)
            return false;
        static float points[MaxCorners * 3];
        int count = 0;
        query->findStraightPath(from, to, polys, polyCount, points, nullptr, nullptr, &count, MaxCorners);
        if (count < 1)
            return false;
        for (int i = 0; i < count; ++i)
        {
            float const* p = &points[i * 3];
            if (!corners.empty())
            {
                NavPoint const& last = corners.back();
                walked += std::sqrt((p[2] - last.x) * (p[2] - last.x) + (p[0] - last.y) * (p[0] - last.y) + (p[1] - last.z) * (p[1] - last.z));
                // (The first corner of a leg is where the one before ended.)
                if (i == 0 && std::fabs(p[2] - last.x) + std::fabs(p[0] - last.y) + std::fabs(p[1] - last.z) < 0.05f)
                    continue;
            }
            corners.push_back(ToWorld(p));
        }
        return true;
    }

    // Is the drop's fall clear of walls (the line of sight from above its edge to above its landing)? Looked at once.
    bool DropClear(MapNav& nav, Drop& drop, Map const* map)
    {
        if (drop.clear < 0)
        {
            if (!map)
                return true;
            // Out over the edge at its height, then down (as a player walks off a ledge and falls): a line from the edge
            // straight to the landing runs through the ledge's own lip. Landing 2.5, 5 and 8 yd out from the edge, the
            // first that is clear: a cliff whose face slopes out is jumped further out from.
            NavPoint const top = ToWorld(drop.top);
            float const air = top.z + 2.0f;
            drop.clear = 0;
            if (drop.swim)
            {
                // Water all along the line (but its first and last yards: the shores), and nothing in the way at the
                // swimmer's height.
                NavPoint const land = ToWorld(drop.land);
                float const length = std::hypot(land.x - top.x, land.y - top.y);
                // (At the water's surface or a little above it, through the plants and small rocks a swimmer passes:
                // the swims to Lady Sarevess's island in Blackfathom Deeps were all "walls" at 1.5 yd over the shores.)
                // (Also under the surface, where a swimmer may go: across the pools of Blackfathom Deeps every line of
                // sight 0.5 to 3 yd over the shores was stopped by the dungeon's model and lines 1.5 and 3 yd under the
                // lower shore were not, 2026-11-25.)
                bool sight = false;
                float sightAt = 0.0f;
                for (float over : { 0.5f, 1.5f, 3.0f, -1.5f, -3.0f })
                    if (!sight && map->isInLineOfSight(top.x, top.y, top.z + over, land.x, land.y, land.z + over, PHASEMASK_NORMAL,
                            LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::M2))
                    {
                        sight = true;
                        sightAt = over;
                    }
                // (The water's surface lies a yard or two below the shores the navmesh ends at: the shores of Blackfathom
                // Deeps' pools at -56, their water from -57.5 down. A sample is wet when the water is there a little,
                // two or four yards below the lower shore.)
                uint32 dry = 0, samples = 0;
                for (float at = 3.0f; sight && at < length - 3.0f; at += 3.0f)
                {
                    float const f = at / length;
                    float const x = top.x + (land.x - top.x) * f;
                    float const y = top.y + (land.y - top.y) * f;
                    float const z = std::min(top.z, land.z);
                    ++samples;
                    if (!map->IsInWater(PHASEMASK_NORMAL, x, y, z - 0.5f, 2.0f) && !map->IsInWater(PHASEMASK_NORMAL, x, y, z - 2.0f, 2.0f) &&
                        !map->IsInWater(PHASEMASK_NORMAL, x, y, z - 4.0f, 2.0f))
                        ++dry;
                }
                bool const wet = sight && (dry <= 1 || dry * 5 <= samples);
                drop.blockedBy += !sight ? " swim:wall" : Acore::StringFormat(" swim:{} ({} of {} dry, sight at {:+.1f} yd)", wet ? "ok" : "dry",
                    dry, samples, sightAt);
                drop.clear = wet ? 1 : 0;
                return drop.clear == 1;
            }
            for (float out : { 2.5f, 5.0f, 8.0f, 12.0f })
            {
                float probe[3] = { drop.top[0] + drop.outward[0] * out, drop.land[1], drop.top[2] + drop.outward[1] * out };
                float land[3];
                if (dtStatusFailed(nav.query->closestPointOnPoly(drop.landRef, probe, land, nullptr)))
                    continue;
                NavPoint const at = ToWorld(land);
                if (top.z - at.z > WaterDropMax || top.z - at.z < DropMin)
                    continue;
                bool const deep = top.z - at.z > GroundDropMax;
                bool const across = map->isInLineOfSight(top.x, top.y, air, at.x, at.y, air, PHASEMASK_NORMAL, LINEOFSIGHT_ALL_CHECKS,
                    VMAP::ModelIgnoreFlags::Nothing);
                bool const down = across && map->isInLineOfSight(at.x, at.y, air, at.x, at.y, at.z + 1.5f, PHASEMASK_NORMAL,
                    LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
                bool const water = !deep || map->IsInWater(PHASEMASK_NORMAL, at.x, at.y, at.z + 0.5f, 2.0f);
                // Another part of the navmesh over the landing, below the edge: the fall ends on it. (Blackfathom Deeps,
                // 2026-11-25: a drop of 4 yd before Ghamoo-ra landed on a floor that lies under the ledge's own part;
                // the jump put the leader on that ledge again, 4 yd above its landing, and every way began there anew.)
                bool under = false;
                if (across && down && water)
                {
                    float const centre[3] = { land[0], (land[1] + air) * 0.5f, land[2] };
                    float const half[3] = { 0.5f, (air - land[1]) * 0.5f + 0.5f, 0.5f };
                    dtQueryFilter any;
                    constexpr int MaxOver = 16;
                    dtPolyRef over[MaxOver];
                    int overCount = 0;
                    nav.query->queryPolygons(centre, half, &any, over, &overCount, MaxOver);
                    for (int i = 0; i < overCount && !under; ++i)
                    {
                        if (nav.IslandOf(over[i]) == drop.to)
                            continue;
                        float height = 0.0f;
                        if (dtStatusSucceed(nav.query->getPolyHeight(over[i], land, &height)) && height > land[1] + 1.5f && height < air)
                            under = true;
                    }
                }
                drop.blockedBy += Acore::StringFormat(" {:.0f}:{}", out, !across ? "out" : !down ? "down" : !water ? "dry" : under ? "under" : "ok");
                if (across && down && water && !under)
                {
                    dtVcopy(drop.land, land);
                    drop.needsWater = deep;
                    drop.clear = 1;
                    break;
                }
            }
        }
        return drop.clear == 1;
    }

    bool Fail(std::string* why, std::string const& text)
    {
        if (why)
            *why = text;
        return false;
    }

    // The way from a point on one polygon to a point on another: a walk when both lie on one part of the navmesh, else
    // down the drops (and across the swims) that lead from the start's part to the end's, the shortest by the straight
    // lines between them. `way` gets the corners, `walked` the yards.
    bool Route(MapNav* nav, dtPolyRef startRef, float const* startNear, dtPolyRef endRef, float const* endNear, Map const* map,
        std::vector<NavPoint>& way, float& walked, std::string* why)
    {
        uint32 const startIsland = nav->IslandOf(startRef);
        uint32 const endIsland = nav->IslandOf(endRef);
        if (startIsland == endIsland)
        {
            if (!Leg(nav->query, startRef, startNear, endRef, endNear, way, walked) || way.size() < 2)
                return Fail(why, Acore::StringFormat("no walk inside part {}", startIsland));
        }
        else
        {
            uint32 blocked = 0;
            std::string blockedText;
            static std::vector<uint32> const none;
            auto const leaving = [&](uint32 part) -> std::vector<uint32> const&
            {
                auto const found = nav->dropsFrom.find(part);
                return found == nav->dropsFrom.end() ? none : found->second;
            };
            uint32 const fromStart = uint32(leaving(startIsland).size());
            // (A pair of parts no drops joined a moment ago: the drops' walls do not change, a drop is only ever taken out.)
            constexpr uint32 FailedPairMs = 60 * IN_MILLISECONDS;
            uint64 const pair = (uint64(startIsland) << 32) | endIsland;
            if (auto const failed = nav->failedPairs.find(pair);
                failed != nav->failedPairs.end() && getMSTimeDiff(failed->second, getMSTime()) < FailedPairMs)
                return Fail(why, Acore::StringFormat("the start is on part {}, the end on part {}: no drops lead there (looked at less than a "
                    "minute ago; {} drops leave the start's part)", startIsland, endIsland, fromStart));
            // Down from one part to another, as players do: the drops that lead to the end's part, the shortest by the
            // straight lines between them (each leg is then walked on its part).
            std::size_t const n = nav->drops.size();
            std::vector<float> cost(n, 1e30f);
            std::vector<int> before(n, -1);
            using Entry = std::pair<float, std::size_t>;
            std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
            for (uint32 d : leaving(startIsland))
            {
                cost[d] = dtVdist(startNear, nav->drops[d].top) + dtVdist(nav->drops[d].top, nav->drops[d].land);
                open.push({ cost[d], d });
            }
            float bestCost = 1e30f;
            int last = -1;
            while (!open.empty())
            {
                auto const [at, d] = open.top();
                open.pop();
                if (at > cost[d] || at >= bestCost)
                    continue;
                Drop& drop = nav->drops[d];
                if (!DropClear(*nav, drop, map))
                {
                    // (The first few in the reason: where a way would leave the part, and how far it falls.)
                    if (++blocked <= 40)
                    {
                        NavPoint const top = ToWorld(drop.top);
                        NavPoint const land = ToWorld(drop.land);
                        blockedText += Acore::StringFormat("; ({:.0f}, {:.0f}, {:.0f}) down {:.0f} yd to ({:.0f}, {:.0f}, {:.0f}) part {}{}", top.x,
                            top.y, top.z, top.z - land.z, land.x, land.y, land.z, drop.to, drop.needsWater ? ", not into water" : "") +
                            " [" + drop.blockedBy + "]";
                    }
                    continue;
                }
                if (drop.to == endIsland)
                {
                    float const total = at + dtVdist(drop.land, endNear);
                    if (total < bestCost)
                    {
                        bestCost = total;
                        last = int(d);
                    }
                    continue;
                }
                for (uint32 e : leaving(drop.to))
                {
                    float const next = at + dtVdist(drop.land, nav->drops[e].top) + dtVdist(nav->drops[e].top, nav->drops[e].land);
                    if (next < cost[e])
                    {
                        cost[e] = next;
                        before[e] = int(d);
                        open.push({ next, e });
                    }
                }
            }
            if (last < 0)
                nav->failedPairs[pair] = std::max<uint32>(getMSTime(), 1);
            if (last < 0)
                return Fail(why, Acore::StringFormat("the start is on part {}, the end on part {}: no drops lead there ({} drops leave the "
                    "start's part, {} drops looked at were blocked by walls{})", startIsland, endIsland, fromStart, blocked, blockedText));
            std::vector<std::size_t> chain;
            for (int d = last; d >= 0; d = before[d])
                chain.push_back(std::size_t(d));
            std::reverse(chain.begin(), chain.end());
            dtPolyRef atRef = startRef;
            float atPoint[3];
            dtVcopy(atPoint, startNear);
            for (std::size_t d : chain)
            {
                Drop const& drop = nav->drops[d];
                if (!Leg(nav->query, atRef, atPoint, drop.topRef, drop.top, way, walked))
                    return Fail(why, Acore::StringFormat("no walk to the edge of a drop at ({:.0f}, {:.0f}, {:.0f})", drop.top[2], drop.top[0], drop.top[1]));
                NavPoint landing = ToWorld(drop.land);
                landing.drop = true;
                landing.swim = drop.swim;
                walked += dtVdist(drop.top, drop.land);
                way.push_back(landing);
                atRef = drop.landRef;
                dtVcopy(atPoint, drop.land);
            }
            if (!Leg(nav->query, atRef, atPoint, endRef, endNear, way, walked) || way.size() < 2)
                return Fail(why, "no walk from the last drop to the end");
        }
        return true;
    }
}

DungeonNav& DungeonNav::Instance()
{
    static DungeonNav instance;
    return instance;
}

// A way that takes long, or waits long for the lock another thread holds, writes a line (at most one in 10 s): the
// planner's step of a raid's leader in Zul'Gurub took a second, every ten seconds for twenty minutes, with no route
// search and no walk of the path finder counted (Wartank, 2026-11-26/27), and nothing said what in it was slow.
bool DungeonNav::FindWay(uint32 mapId, float fromX, float fromY, float fromZ, float toX, float toY, float toZ,
    std::vector<NavPoint>* corners, float* length, Map const* map, std::string* why, std::source_location where)
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point const asked = Clock::now();
    std::lock_guard<std::mutex> guard(lock);
    Clock::time_point const locked = Clock::now();
    float walked = 0.0f;
    std::size_t cornerCount = 0;
    std::vector<NavPoint> own;
    std::vector<NavPoint>* out = corners ? corners : &own;
    bool const found = FindWayLocked(mapId, fromX, fromY, fromZ, toX, toY, toZ, out, &walked, map, why);
    cornerCount = out->size();
    if (length && found)
        *length = walked;
    Clock::time_point const done = Clock::now();
    double const waitMs = std::chrono::duration<double, std::milli>(locked - asked).count();
    double const workMs = std::chrono::duration<double, std::milli>(done - locked).count();
    constexpr double SlowWayMs = 50.0;
    constexpr uint32 SlowWayLineMs = 10 * IN_MILLISECONDS;
    static uint32 lastSlowLine = 0;
    if (waitMs + workMs >= SlowWayMs && (!lastSlowLine || getMSTimeDiff(lastSlowLine, getMSTime()) >= SlowWayLineMs))
    {
        lastSlowLine = std::max<uint32>(getMSTime(), 1);
        std::string file = where.file_name();
        if (std::size_t const slash = file.find_last_of("/\\"); slash != std::string::npos)
            file = file.substr(slash + 1);
        LOG_INFO("module.ga.activity", "activity: a way on the navmesh of map {} took {:.0f} ms ({:.0f} ms waiting for another "
            "thread's way) from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f}), {}: {}, asked by {}:{}", mapId, waitMs + workMs, waitMs,
            fromX, fromY, fromZ, toX, toY, toZ, found ? Acore::StringFormat("{:.0f} yd over {} corners", walked, cornerCount) : "no way",
            found ? "found" : (why && !why->empty() ? *why : "none"), file, where.line());
    }
    return found;
}

bool DungeonNav::FindWayLocked(uint32 mapId, float fromX, float fromY, float fromZ, float toX, float toY, float toZ,
    std::vector<NavPoint>* corners, float* length, Map const* map, std::string* why)
{
    float const start[3] = { fromY, fromZ, fromX };
    float const end[3] = { toY, toZ, toX };
    // The open world: its tiles around both ends, a plain walk on the ground.
    if (IsOpenWorld(mapId))
    {
        // No walk of more than 4000 yd is asked of the open world's navmesh (the longest the planner walks this way), so a
        // farther end is no way without loading the tiles around both ends: a mage cut off in Silithus asked for its
        // way to Salfa in Winterspring, and the tiles of the two ends cost the world tick 667 ms (2026-11-27, 03:32).
        constexpr float OpenMaxStraight = 4000.0f;
        if (float const straight = std::hypot(toX - fromX, toY - fromY); straight > OpenMaxStraight)
            return Fail(why, Acore::StringFormat("too far for a walk on the navmesh ({:.0f} yd straight)", straight).c_str());
        OpenNav* open = LoadOpen(mapId);
        if (!open)
            return Fail(why, "no navmesh of the map");
        LoadOpenTiles(*open, fromX, fromY, toX, toY);
        dtQueryFilter filter;
        BotFilter(filter);
        float const extents[3] = { 3.0f, 5.0f, 3.0f };
        dtPolyRef startRef = 0, endRef = 0;
        float startNear[3], endNear[3];
        open->query->findNearestPoly(start, extents, &filter, &startRef, startNear);
        open->query->findNearestPoly(end, extents, &filter, &endRef, endNear);
        if (!startRef || !endRef)
            return Fail(why, !endRef ? "no navmesh at the end" : "no navmesh at the start");
        // (On foot the start is the part under the feet, as in a dungeon.)
        if (startNear[1] > fromZ + 1.5f)
        {
            constexpr int MaxNear = 32;
            float const downExtents[3] = { 3.0f, 8.0f, 3.0f };
            dtPolyRef nearRefs[MaxNear];
            int nearCount = 0;
            open->query->queryPolygons(start, downExtents, &filter, nearRefs, &nearCount, MaxNear);
            float best = 1e30f;
            for (int i = 0; i < nearCount; ++i)
            {
                float point[3];
                if (dtStatusFailed(open->query->closestPointOnPoly(nearRefs[i], start, point, nullptr)) || point[1] > fromZ + 1.5f)
                    continue;
                if (float const away = dtVdist(start, point); away < best)
                {
                    best = away;
                    startRef = nearRefs[i];
                    dtVcopy(startNear, point);
                }
            }
        }
        std::vector<NavPoint> way;
        float walked = 0.0f;
        if (!Leg(open->query, startRef, startNear, endRef, endNear, way, walked) || way.size() < 2)
            return Fail(why, "no walk on the navmesh between them");
        if (corners)
            *corners = std::move(way);
        if (length)
            *length = walked;
        return true;
    }
    MapNav* nav = Load(mapId);
    if (!nav)
        return Fail(why, "no navmesh of the map");
    dtQueryFilter filter;
    BotFilter(filter);
    float const extents[3] = { 3.0f, 5.0f, 3.0f };
    dtPolyRef startRef = 0, endRef = 0;
    float startNear[3], endNear[3];
    nav->query->findNearestPoly(start, extents, &filter, &startRef, startNear);
    nav->query->findNearestPoly(end, extents, &filter, &endRef, endNear);
    // (A swimmer at the surface: its feet lie less than half a yard under it. Blackfathom Deeps, 2026-11-25: the leader
    // floated at the shore it had swum to, 2.5 yd above that shore's part and 0.5 yd above a ledge's part; taken for a
    // walker, its start was the ledge, every way began with the swim from the ledge again, and it floated there.)
    bool const swimming = map && (map->IsInWater(PHASEMASK_NORMAL, fromX, fromY, fromZ + 0.5f, 2.0f) ||
        map->IsInWater(PHASEMASK_NORMAL, fromX, fromY, fromZ - 0.5f, 2.0f));
    if ((!startRef && !swimming) || !endRef)
        return Fail(why, !endRef ? "no navmesh at the end" : "no navmesh at the start");
    // On foot the start is the part under the feet, not one a few yards above (Blackfathom Deeps, 2026-11-25: after the
    // leader's jump of 6 yd down beside Lorgus Jett's ledge the start was taken on the ledge 5 yd above it, every way
    // began with that jump again, and the group stood below it for eight minutes).
    if (startRef && !swimming && startNear[1] > fromZ + 1.5f)
    {
        constexpr int MaxNear = 32;
        float const downExtents[3] = { 3.0f, 8.0f, 3.0f };
        dtPolyRef nearRefs[MaxNear];
        int nearCount = 0;
        nav->query->queryPolygons(start, downExtents, &filter, nearRefs, &nearCount, MaxNear);
        float best = 1e30f;
        for (int i = 0; i < nearCount; ++i)
        {
            float point[3];
            if (dtStatusFailed(nav->query->closestPointOnPoly(nearRefs[i], start, point, nullptr)) || point[1] > fromZ + 1.5f)
                continue;
            float const away = dtVdist(start, point);
            if (away < best)
            {
                best = away;
                startRef = nearRefs[i];
                dtVcopy(startNear, point);
            }
        }
    }
    // Over water, the nearest polygon beside the character and one right under it: the one under it. (Blackfathom Deeps,
    // 2026-11-25: the leader at the surface of a pool before Ghamoo-ra, the pool's floor 4 yd under it, took the ledge
    // 4 yd beside it as its start, whose way began with a swim back across the pool.)
    if (startRef && !swimming && std::hypot(startNear[0] - start[0], startNear[2] - start[2]) > 1.0f && map &&
        map->IsInWater(PHASEMASK_NORMAL, fromX, fromY, fromZ - 1.5f, 2.0f))
    {
        constexpr int MaxNear = 32;
        float const underExtents[3] = { 1.0f, 8.0f, 1.0f };
        dtPolyRef nearRefs[MaxNear];
        int nearCount = 0;
        nav->query->queryPolygons(start, underExtents, &filter, nearRefs, &nearCount, MaxNear);
        float best = 1e30f;
        for (int i = 0; i < nearCount; ++i)
        {
            float point[3];
            if (dtStatusFailed(nav->query->closestPointOnPoly(nearRefs[i], start, point, nullptr)) || point[1] > fromZ + 1.5f ||
                std::hypot(point[0] - start[0], point[2] - start[2]) > 1.0f)
                continue;
            float const down = fromZ - point[1];
            if (down < best)
            {
                best = down;
                startRef = nearRefs[i];
                dtVcopy(startNear, point);
            }
        }
    }
    std::vector<NavPoint> way;
    float walked = 0.0f;
    // A first drop or swim the character has as good as done: its landing lies nearer than its top and within 3.5 yd.
    // The way is planned again from the landing's part. (Blackfathom Deeps, 2026-11-25: the swim of 5 yd before Ghamoo-ra
    // ended 2 yd short of its landing, between two parts a yard apart in height; from there the start was the top's
    // part again, and the leader walked back to the top and swam the same 5 yd every seven seconds.)
    auto skipCrossedDrop = [&](std::vector<NavPoint>& route, float& routeWalked)
    {
        for (std::size_t i = 1; i < route.size(); ++i)
        {
            if (!route[i].drop)
                continue;
            NavPoint const& top = route[i - 1];
            NavPoint const& land = route[i];
            float const toLand = std::sqrt((land.x - fromX) * (land.x - fromX) + (land.y - fromY) * (land.y - fromY) +
                (land.z - fromZ) * (land.z - fromZ));
            float const toTop = std::sqrt((top.x - fromX) * (top.x - fromX) + (top.y - fromY) * (top.y - fromY) +
                (top.z - fromZ) * (top.z - fromZ));
            if (toLand > 3.5f || toLand >= toTop)
                return;
            float const at[3] = { land.y, land.z, land.x };
            float const near3[3] = { 1.0f, 2.0f, 1.0f };
            dtPolyRef landRef = 0;
            float landNear[3];
            nav->query->findNearestPoly(at, near3, &filter, &landRef, landNear);
            std::vector<NavPoint> again;
            float againWalked = 0.0f;
            if (landRef && Route(nav, landRef, landNear, endRef, endNear, map, again, againWalked, nullptr) && again.size() >= 2)
            {
                route = std::move(again);
                routeWalked = againWalked + toLand;
            }
            return;
        }
    };
    // A swimmer floats between two parts of the navmesh: the water's surface and the ground below it, or the shore it
    // has just swum to and the pool it swam across (Blackfathom Deeps, 2026-11-25: the group had swum to the shore
    // before Twilight Lord Kelris, the start was taken on the pool's part 2 yd below, and every way led back across the
    // swim; it floated there for a quarter of an hour). In the water the start is the part whose way is the shortest,
    // its distance from the swimmer counted.
    // A swimmer in deep water has no ground within 5 yd: the floor of the pool lies below it, the shore above (the
    // group swam up for air on its way to Lady Sarevess, 20 yd over the floor, and was put near her for having no way).
    // The parts are looked for 25 yd up and down.
    if (swimming)
    {
        constexpr int MaxNear = 64;
        float const swimExtents[3] = { 3.0f, 25.0f, 3.0f };
        dtPolyRef nearRefs[MaxNear];
        int nearCount = 0;
        nav->query->queryPolygons(start, swimExtents, &filter, nearRefs, &nearCount, MaxNear);
        std::unordered_map<uint32, std::pair<dtPolyRef, float>> byPart; // part -> its nearest polygon, the distance
        std::unordered_map<uint32, std::array<float, 3>> nearPoint;
        for (int i = 0; i < nearCount; ++i)
        {
            float point[3];
            if (dtStatusFailed(nav->query->closestPointOnPoly(nearRefs[i], start, point, nullptr)))
                continue;
            float const away = dtVdist(start, point);
            uint32 const part = nav->IslandOf(nearRefs[i]);
            auto known = byPart.find(part);
            if (known == byPart.end() || away < known->second.second)
            {
                byPart[part] = { nearRefs[i], away };
                nearPoint[part] = { point[0], point[1], point[2] };
            }
        }
        if (byPart.size() > 1 || (!startRef && !byPart.empty()))
        {
            float best = 1e30f;
            for (auto const& [part, nearest] : byPart)
            {
                std::vector<NavPoint> tried;
                float triedWalked = 0.0f;
                float const* from = nearPoint[part].data();
                if (!Route(nav, nearest.first, from, endRef, endNear, map, tried, triedWalked, nullptr))
                    continue;
                if (triedWalked + nearest.second < best)
                {
                    best = triedWalked + nearest.second;
                    way = std::move(tried);
                    walked = triedWalked;
                }
            }
            if (best < 1e29f)
            {
                skipCrossedDrop(way, walked);
                if (corners)
                    *corners = std::move(way);
                if (length)
                    *length = walked;
                return true;
            }
        }
    }
    if (!startRef)
        return Fail(why, "no navmesh at the start (in the water, none 25 yd up or down)");
    if (!Route(nav, startRef, startNear, endRef, endNear, map, way, walked, why))
        return false;
    skipCrossedDrop(way, walked);
    if (corners)
        *corners = std::move(way);
    if (length)
        *length = walked;
    return true;
}

bool DungeonNav::NearestOnMesh(uint32 mapId, float x, float y, float z, float radius, NavPoint& point)
{
    std::lock_guard<std::mutex> guard(lock);
    dtNavMeshQuery* query = nullptr;
    if (IsOpenWorld(mapId))
    {
        OpenNav* open = LoadOpen(mapId);
        if (!open)
            return false;
        LoadOpenTiles(*open, x, y, x, y);
        query = open->query;
    }
    else if (MapNav* nav = Load(mapId))
        query = nav->query;
    if (!query)
        return false;
    dtQueryFilter filter;
    BotFilter(filter);
    float const centre[3] = { y, z, x };
    float const extents[3] = { radius, radius, radius };
    dtPolyRef ref = 0;
    float nearest[3];
    if (dtStatusFailed(query->findNearestPoly(centre, extents, &filter, &ref, nearest)) || !ref || dtVdist(centre, nearest) > radius)
        return false;
    point = ToWorld(nearest);
    return true;
}

bool DungeonNav::SurfaceLine(uint32 mapId, float fromX, float fromY, float fromZ, float toX, float toY, float toZ, float step,
    std::vector<NavPoint>& points)
{
    points.clear();
    std::lock_guard<std::mutex> guard(lock);
    dtNavMeshQuery* query = nullptr;
    if (IsOpenWorld(mapId))
    {
        if (OpenNav* open = LoadOpen(mapId))
            query = open->query;
    }
    else if (MapNav* nav = Load(mapId))
        query = nav->query;
    if (!query || step < 1.0f)
        return false;
    dtQueryFilter filter;
    BotFilter(filter);
    float const extents[3] = { 3.0f, 5.0f, 3.0f };
    float at[3] = { fromY, fromZ, fromX };
    dtPolyRef ref = 0;
    float on[3];
    if (dtStatusFailed(query->findNearestPoly(at, extents, &filter, &ref, on)) || !ref)
        return false;
    dtVcopy(at, on);
    float const length = std::hypot(toX - fromX, toY - fromY);
    int const steps = std::max(1, int(std::ceil(length / step)));
    constexpr int MaxVisited = 64;
    dtPolyRef visited[MaxVisited];
    for (int i = 1; i <= steps; ++i)
    {
        float const t = float(i) / float(steps);
        float const target[3] = { fromY + (toY - fromY) * t, fromZ + (toZ - fromZ) * t, fromX + (toX - fromX) * t };
        float result[3];
        int visitedCount = 0;
        if (dtStatusFailed(query->moveAlongSurface(ref, at, target, &filter, result, visited, &visitedCount, MaxVisited)) ||
            visitedCount <= 0)
            break;
        ref = visited[visitedCount - 1];
        float height = result[1];
        if (dtStatusFailed(query->getPolyHeight(ref, result, &height)))
            height = result[1];
        result[1] = height;
        // (The surface ends short of the target: the line left the navmesh; no further.)
        bool const stopped = dtVdist2D(result, target) > 1.0f;
        points.push_back(ToWorld(result));
        dtVcopy(at, result);
        if (stopped)
            break;
    }
    return !points.empty();
}

uint32 DungeonNav::BlockDrop(uint32 mapId, float topX, float topY, float topZ, float landX, float landY, float landZ)
{
    std::lock_guard<std::mutex> guard(lock);
    MapNav* nav = Load(mapId);
    if (!nav)
        return 0;
    uint32 blocked = 0;
    for (Drop& drop : nav->drops)
    {
        NavPoint const top = ToWorld(drop.top);
        NavPoint const land = ToWorld(drop.land);
        if (std::hypot(top.x - topX, top.y - topY) > 8.0f || std::fabs(top.z - topZ) > 4.0f ||
            std::hypot(land.x - landX, land.y - landY) > 4.0f || std::fabs(land.z - landZ) > 3.0f)
            continue;
        drop.clear = 0;
        drop.blockedBy += " landed:above";
        ++blocked;
    }
    return blocked;
}

bool DungeonNav::PointAlong(uint32 mapId, float fromX, float fromY, float fromZ, float toX, float toY, float toZ, float ahead,
    NavPoint& point, float* length, NavPoint* dropTop, Map const* map)
{
    std::vector<NavPoint> corners;
    float walked = 0.0f;
    if (!FindWay(mapId, fromX, fromY, fromZ, toX, toY, toZ, &corners, &walked, map))
        return false;
    if (length)
        *length = walked;
    // The first corner at or beyond `ahead` (a corner lies on the navmesh, where the path finder walks to); a point on
    // the straight line between two corners only when that corner is far beyond it. A drop on the way first: the walk
    // stops at its edge (`dropTop`), and the point is its landing.
    float passed = 0.0f;
    for (std::size_t i = 1; i < corners.size(); ++i)
    {
        NavPoint const& a = corners[i - 1];
        NavPoint const& b = corners[i];
        if (b.drop)
        {
            point = b;
            if (dropTop)
                *dropTop = a;
            return true;
        }
        float const leg = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) + (b.z - a.z) * (b.z - a.z));
        if (passed + leg >= ahead)
        {
            if (passed + leg <= ahead + 60.0f || leg <= 0.0f)
                point = b;
            else
            {
                float const t = (ahead - passed) / leg;
                point = NavPoint{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
                // The straight path's corners follow the ground, the line between two of them does not: on the way
                // down from Lethtendris's platform in Dire Maul East the point 35 yd along hung 14 yd over the floor
                // below the ramp, the core's path to it ended on that floor, the ground guard refused the walk as one
                // through solid ground, and the tank tried it again every 25 s for five minutes (2026-11-25). The point
                // is moved along the navmesh's surface from the corner before it instead.
                std::lock_guard<std::mutex> guard(lock);
                dtNavMeshQuery* query = nullptr;
                if (IsOpenWorld(mapId))
                {
                    if (OpenNav* open = LoadOpen(mapId))
                        query = open->query;
                }
                else if (MapNav* nav = Load(mapId))
                    query = nav->query;
                if (query)
                {
                    dtQueryFilter filter;
                    BotFilter(filter);
                    float const from[3] = { a.y, a.z, a.x };
                    float const target[3] = { point.y, point.z, point.x };
                    float const around[3] = { 2.0f, 4.0f, 2.0f };
                    dtPolyRef ref = 0;
                    float on[3];
                    constexpr int MaxVisited = 128;
                    dtPolyRef visited[MaxVisited];
                    int visitedCount = 0;
                    float result[3];
                    if (dtStatusSucceed(query->findNearestPoly(from, around, &filter, &ref, on)) && ref &&
                        dtStatusSucceed(query->moveAlongSurface(ref, on, target, &filter, result, visited, &visitedCount, MaxVisited)) &&
                        visitedCount > 0)
                    {
                        float height = result[1];
                        if (dtStatusFailed(query->getPolyHeight(visited[visitedCount - 1], result, &height)))
                            height = result[1];
                        point = NavPoint{ result[2], result[0], height };
                    }
                }
            }
            return true;
        }
        passed += leg;
    }
    point = corners.back();
    return true;
}
