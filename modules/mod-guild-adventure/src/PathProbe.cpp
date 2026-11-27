/*
 * PathProbe implementation. See PathProbe.h.
 */

#include "PathProbe.h"
#include "DetourNavMeshQuery.h"
#include "GuildAdventureSettings.h"
#include "Map.h"
#include "MapDefines.h"
#include "Object.h"
#include "PathGenerator.h"
#include "Perf.h"
#include "Player.h"
#include "StringFormat.h"
#include "Unit.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

using namespace GuildAdventure;

namespace
{
    // The path is sampled this often.
    constexpr float SampleStep = 1.0f;
    // A floor "under the feet": looked for from this far above the sample, accepted down to this far below it.
    constexpr float FeetAbove = 1.5f;
    constexpr float FloorBelow = 3.0f;
    // A path between two navmesh corners may run under a hump of the ground: a floor up to this far above the sample
    // counts too.
    constexpr float HumpAbove = 4.0f;
    // How far below a sample a floor is looked for (buildings and caves; the terrain is found at any depth).
    constexpr float FloorSearch = 10.0f;
    // The floor under two neighbouring samples differs by more than this: the path steps from one level to another
    // without a slope (from a hill onto the floor of the tunnel underneath).
    constexpr float FloorBreak = 3.5f;
    // A sample this far below the surface (terrain or the top of a building) is inside something: the hill above a
    // mine, a tower. Less deep it only clips a slope on its way down, like a player who jumps off a ledge.
    constexpr float BelowSurface = 6.0f;
    // A stretch that climbs or drops more than DropHeight and is steeper than about 68 degrees is no walk (the straight
    // line down to a mob in the tunnel below). Ceilings and galleries on the way would otherwise pass as floors.
    constexpr float DropHeight = 4.0f;
    constexpr float DropSlope = 2.5f;

    // The floor a walker at the sample stands on, INVALID_HEIGHT when there is none in reach.
    float FloorAt(Map const* map, uint32 phaseMask, G3D::Vector3 const& sample)
    {
        float const floor = map->GetHeight(phaseMask, sample.x, sample.y, sample.z + FeetAbove, true, FloorSearch);
        if (floor > INVALID_HEIGHT && floor >= sample.z - FloorBelow)
            return floor;
        float const hump = map->GetHeight(phaseMask, sample.x, sample.y, sample.z + HumpAbove, true, HumpAbove);
        if (hump > INVALID_HEIGHT && hump > sample.z + FeetAbove)
            return hump;
        return INVALID_HEIGHT;
    }

    void Sample(WorldObject const* source, Movement::PointsArray const& points, float fromEnds, PathReport& report)
    {
        Map const* map = source->FindMap();
        uint32 const phaseMask = source->GetPhaseMask();
        float const collisionHeight = source->GetCollisionHeight();
        // A swimmer's way under the floor of its water is inside the ground, however shallow. The core counts water down
        // to 2 yd under a lake's floor, and a sample less than 4 yd under the terrain finds it as a "hump" above it, so a
        // straight swim could run up to six yards under a sloping lake floor and pass as no way through solid ground at all;
        // a swimmer whose movement ended or stopped on such a line stood inside the ground. A hunter fighting Blighted
        // Surges in Lake Mereldar (one of them spawns 3 yd under its floor) stood 3.7 yd inside the ground and was lifted out,
        // round 204, as two characters had been in rounds 183 and 184. (Not for a walker: its straight lines down a ledge
        // clip the slope on their way, and its own rules judge the drop.)
        Unit const* unit = source->ToUnit();
        bool const swimmer = unit && (IsSwimming(unit) || IsDiving(unit));

        float total = 0.0f;
        for (std::size_t i = 1; i < points.size(); ++i)
            total += (points[i] - points[i - 1]).length();
        report.length = total;

        float walked = 0.0f;
        float lastFloor = INVALID_HEIGHT;
        bool hasLast = false;
        for (std::size_t i = 1; i < points.size(); ++i)
        {
            G3D::Vector3 const& from = points[i - 1];
            G3D::Vector3 const& to = points[i];
            float const segment = (to - from).length();
            if (segment < 0.01f)
                continue;

            float const rise = std::fabs(to.z - from.z);
            float const run = std::sqrt((to.x - from.x) * (to.x - from.x) + (to.y - from.y) * (to.y - from.y));
            // Straight up or down in water is a dive, not a drop.
            G3D::Vector3 const half = (from + to) * 0.5f;
            bool const dive = map->IsInWater(phaseMask, half.x, half.y, half.z, collisionHeight) &&
                (map->IsInWater(phaseMask, to.x, to.y, to.z, collisionHeight) || map->IsInWater(phaseMask, from.x, from.y, from.z, collisionHeight));
            if (rise > DropHeight && rise > DropSlope * run && !dive)
            {
                walked += segment;
                ++report.breaks;
                report.offGround += segment;
                G3D::Vector3 const middle = (from + to) * 0.5f;
                float const top = map->GetHeight(phaseMask, middle.x, middle.y, MAX_HEIGHT, true, MAX_HEIGHT);
                if (top > INVALID_HEIGHT && middle.z < top - BelowSurface)
                    report.throughSolid += segment;
                if (rise > report.worstGap)
                {
                    report.worstGap = rise;
                    report.worstX = to.x;
                    report.worstY = to.y;
                    report.worstZ = to.z;
                }
                hasLast = false;
                continue;
            }

            uint32 const steps = std::max<uint32>(1, uint32(segment / SampleStep));
            float const stepLength = segment / float(steps);
            for (uint32 step = 1; step <= steps; ++step)
            {
                walked += stepLength;
                // Only the ends of a path the navmesh vouches for: that is where the path finder joins the real
                // positions to the mesh.
                if (fromEnds > 0.0f && walked > fromEnds && walked < total - fromEnds)
                {
                    hasLast = false;
                    continue;
                }

                G3D::Vector3 const sample = from + (to - from) * (float(step) / float(steps));
                if (swimmer)
                {
                    // (Under the terrain by more than 2 yd, measured as the ground guard's UnderTheGround does, with no floor
                    // of a building or a cave below it.)
                    float const terrain = map->GetGridHeight(sample.x, sample.y);
                    if (terrain > INVALID_HEIGHT && sample.z < terrain - 1.0f &&
                        map->GetHeight(phaseMask, sample.x, sample.y, sample.z + 2.0f, true, 100.0f) <= INVALID_HEIGHT)
                    {
                        report.offGround += stepLength;
                        report.throughSolid += stepLength;
                        if (terrain - sample.z > report.worstGap)
                        {
                            report.worstGap = terrain - sample.z;
                            report.worstX = sample.x;
                            report.worstY = sample.y;
                            report.worstZ = sample.z;
                        }
                        hasLast = false;
                        continue;
                    }
                }
                if (map->IsInWater(phaseMask, sample.x, sample.y, sample.z, collisionHeight))
                {
                    hasLast = false;
                    continue;
                }

                float const floor = FloorAt(map, phaseMask, sample);
                bool const supported = floor > INVALID_HEIGHT;
                bool const broke = supported && hasLast && std::fabs(floor - lastFloor) > FloorBreak;
                hasLast = supported;
                lastFloor = floor;
                if (supported && !broke)
                    continue;

                if (broke)
                    ++report.breaks;
                report.offGround += stepLength;
                float const surface = map->GetHeight(phaseMask, sample.x, sample.y, MAX_HEIGHT, true, MAX_HEIGHT);
                if (surface > INVALID_HEIGHT && sample.z < surface - BelowSurface)
                    report.throughSolid += stepLength;
                float const gap = supported ? std::fabs(floor - sample.z) + FloorBreak : FloorSearch;
                if (gap > report.worstGap)
                {
                    report.worstGap = gap;
                    report.worstX = sample.x;
                    report.worstY = sample.y;
                    report.worstZ = sample.z;
                }
            }
        }
    }
}

void GuildAdventure::EnsureNavmesh(WorldObject const* source, float x, float y, float destX, float destY)
{
    constexpr float StepYards = 100.0f;   // well under a grid's 533 yd: every grid the line crosses
    constexpr float MaxYards = 3000.0f;   // beyond this only the two ends (routes walk such ways leg by leg)
    Map* map = source ? source->FindMap() : nullptr;
    if (!map)
        return;
    float const length = std::hypot(destX - x, destY - y);
    uint32 const steps = length > MaxYards ? 1 : uint32(std::ceil(length / StepYards));
    int32 lastX = -1, lastY = -1;
    for (uint32 i = 0; i <= steps; ++i)
    {
        float const t = steps ? float(i) / float(steps) : 0.0f;
        GridCoord const grid = Acore::ComputeGridCoord(x + (destX - x) * t, y + (destY - y) * t);
        if (int32(grid.x_coord) == lastX && int32(grid.y_coord) == lastY)
            continue;
        lastX = int32(grid.x_coord);
        lastY = int32(grid.y_coord);
        if (grid.x_coord < MAX_NUMBER_OF_GRIDS && grid.y_coord < MAX_NUMBER_OF_GRIDS && !map->IsGridCreated(grid))
            map->GetGridTerrainData(grid);
    }
}

bool GuildAdventure::NavmeshGroundNear(WorldObject const* source, float x, float y, float z, float across, float upDown,
    float& groundX, float& groundY, float& groundZ)
{
    Map* map = source ? source->FindMap() : nullptr;
    if (!map)
        return false;
    EnsureNavmesh(source, x, y, x, y);
    dtNavMeshQuery const* query = map->GetMapCollisionData().GetMMapData().GetNavMeshQuery();
    if (!query)
        return false;
    dtQueryFilter filter;
    filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
    filter.setExcludeFlags(NAV_MAGMA | NAV_SLIME);
    // (Detour's axes: y, z, x.)
    float const centre[3] = { y, z, x };
    float const extents[3] = { across, upDown, across };
    dtPolyRef ref = 0;
    float nearest[3] = { 0.0f, 0.0f, 0.0f };
    if (dtStatusFailed(query->findNearestPoly(centre, extents, &filter, &ref, nearest)) || !ref)
        return false;
    groundX = nearest[2];
    groundY = nearest[0];
    groundZ = nearest[1];
    return true;
}

bool GuildAdventure::PathEndsAt(WorldObject const* source, float destX, float destY, float destZ)
{
    constexpr float NearYards = 6.0f;   // within this (its height included) the spot is there anyway
    constexpr float EndYards = 6.0f;    // the path's end this far from the spot across
    constexpr float EndHeight = 4.0f;   // ... and up or down
    if (!source || !source->FindMap())
        return false;
    float const dx = destX - source->GetPositionX();
    float const dy = destY - source->GetPositionY();
    float const dz = destZ - source->GetPositionZ();
    if (dx * dx + dy * dy + dz * dz <= NearYards * NearYards)
        return true;
    EnsureNavmesh(source, source->GetPositionX(), source->GetPositionY(), destX, destY);
    PathGenerator path(source);
    if (!path.CalculatePath(destX, destY, destZ, false))
        return false;
    Movement::PointsArray const& points = path.GetPath();
    if ((path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH | PATHFIND_SHORTCUT)) || points.size() < 2)
        return false;
    G3D::Vector3 const& end = points.back();
    return std::hypot(end.x - destX, end.y - destY) <= EndYards && std::fabs(end.z - destZ) <= EndHeight;
}

namespace
{
    bool ComputeWalkReaches(WorldObject const* source, float x, float y, float z, float destX, float destY, float destZ, float* length);

    // The walk cache (docs/performance.md PF-2). Nearly every route search proves the walks between the start and its
    // nearest road nodes and between the nodes nearest to the destination and the destination with the path finder,
    // and nothing changed between two searches: the navmesh, the nodes, the flight masters stay where they are. A
    // walk is remembered by map, the walker's kind (the path finder's filter: a bot keeps off steep slopes, a client
    // does not; in water or not) and both ends rounded to CellYards. A "no" lives shorter: the navmesh of a grid that
    // was not loaded answers no as well.
    constexpr float CellYards = 8.0f;
    constexpr uint64 YesTtlMs = 10 * MINUTE * IN_MILLISECONDS;
    constexpr uint64 NoTtlMs = 3 * MINUTE * IN_MILLISECONDS;
    constexpr uint64 PurgeEveryMs = MINUTE * IN_MILLISECONDS;
    constexpr std::size_t MaxEntries = 400000;

    struct WalkKey
    {
        uint32 map = 0;
        uint32 kind = 0;
        int32 fx = 0, fy = 0, fz = 0, tx = 0, ty = 0, tz = 0;
        bool operator==(WalkKey const& other) const
        {
            return map == other.map && kind == other.kind && fx == other.fx && fy == other.fy && fz == other.fz &&
                tx == other.tx && ty == other.ty && tz == other.tz;
        }
    };

    struct WalkKeyHash
    {
        std::size_t operator()(WalkKey const& key) const
        {
            uint64 h = 1469598103934665603ull;
            for (uint32 part : { key.map, key.kind, uint32(key.fx), uint32(key.fy), uint32(key.fz), uint32(key.tx), uint32(key.ty),
                uint32(key.tz) })
            {
                h ^= part;
                h *= 1099511628211ull;
            }
            return std::size_t(h);
        }
    };

    struct WalkAnswer
    {
        bool reaches = false;
        float length = 0.0f;
        uint64 expiresAt = 0;
    };

    std::mutex walkCacheLock;
    std::unordered_map<WalkKey, WalkAnswer, WalkKeyHash> walkCache;
    uint64 walkCachePurgeAt = 0;

    int32 WalkCell(float coordinate) { return int32(std::floor(coordinate / CellYards)); }
}

bool GuildAdventure::WalkReaches(WorldObject const* source, float x, float y, float z, float destX, float destY, float destZ, float* length)
{
    Player const* player = source ? source->ToPlayer() : nullptr;
    if (!player || !GetSettings().plannerRouteCache)
    {
        Perf::Counters& counters = Perf::Get();
        counters.walkProbes.fetch_add(1, std::memory_order_relaxed);
        Perf::ScopedUs timer(counters.walkUs);
        return ComputeWalkReaches(source, x, y, z, destX, destY, destZ, length);
    }

    // The path finder's filter for this walker (PathGenerator::CreateFilter / UpdateFilter): a headless session's
    // character keeps off steep slopes; one in water may swim from where it is.
    uint32 kind = player->GetPhaseMask() << 2;
    if (player->GetSession() && player->GetSession()->IsHeadless())
        kind |= 1;
    if (player->IsInWater() || player->IsUnderWater())
        kind |= 2;
    WalkKey const key{ player->GetMapId(), kind, WalkCell(x), WalkCell(y), WalkCell(z), WalkCell(destX), WalkCell(destY), WalkCell(destZ) };
    uint64 const now = Perf::NowMs();
    Perf::Counters& counters = Perf::Get();
    {
        std::lock_guard<std::mutex> guard(walkCacheLock);
        if (now >= walkCachePurgeAt)
        {
            walkCachePurgeAt = now + PurgeEveryMs;
            for (auto itr = walkCache.begin(); itr != walkCache.end();)
                itr = now >= itr->second.expiresAt ? walkCache.erase(itr) : std::next(itr);
            if (walkCache.size() > MaxEntries)
                walkCache.clear();
        }
        auto known = walkCache.find(key);
        if (known != walkCache.end() && now < known->second.expiresAt)
        {
            counters.walkHits.fetch_add(1, std::memory_order_relaxed);
            if (length && known->second.reaches)
                *length = known->second.length;
            return known->second.reaches;
        }
    }

    float walked = 0.0f;
    bool reaches;
    {
        counters.walkProbes.fetch_add(1, std::memory_order_relaxed);
        Perf::ScopedUs timer(counters.walkUs);
        reaches = ComputeWalkReaches(source, x, y, z, destX, destY, destZ, &walked);
    }
    {
        std::lock_guard<std::mutex> guard(walkCacheLock);
        walkCache[key] = WalkAnswer{ reaches, walked, now + (reaches ? YesTtlMs : NoTtlMs) };
    }
    if (length && reaches)
        *length = walked;
    return reaches;
}

std::size_t GuildAdventure::WalkCacheSize()
{
    std::lock_guard<std::mutex> guard(walkCacheLock);
    return walkCache.size();
}

void GuildAdventure::ClearWalkCache()
{
    std::lock_guard<std::mutex> guard(walkCacheLock);
    walkCache.clear();
}

namespace
{
bool ComputeWalkReaches(WorldObject const* source, float x, float y, float z, float destX, float destY, float destZ, float* length)
{
    constexpr int MaxHops = 16;
    constexpr float Headway = 15.0f; // a path that ends less than this closer to the target has run into something
    constexpr float Arrived = 12.0f;  // NPCs stand a few yards off the navmesh (on a pier, behind a counter)

    if (!source || !source->FindMap())
        return false;
    float walked = 0.0f;
    EnsureNavmesh(source, x, y, destX, destY);
    for (int hop = 0; hop < MaxHops; ++hop)
    {
        float const left = std::sqrt((destX - x) * (destX - x) + (destY - y) * (destY - y) + (destZ - z) * (destZ - z));
        if (left <= Arrived)
        {
            if (length)
                *length = walked;
            return true;
        }
        PathGenerator path(source);
        if (!path.CalculatePath(x, y, z, destX, destY, destZ, false))
            return false;
        uint32 const type = path.GetPathType();
        Movement::PointsArray const& points = path.GetPath();
        if ((type & (PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH | PATHFIND_SHORTCUT)) || points.size() < 2)
            return false;
        float hopLength = 0.0f;
        for (std::size_t i = 1; i < points.size(); ++i)
            hopLength += (points[i] - points[i - 1]).length();
        if (!(type & PATHFIND_INCOMPLETE))
        {
            if (length)
                *length = walked + hopLength;
            return true;
        }
        // Incomplete: a path of a single polygon ends at the target itself although nothing leads there; any other
        // ends where the search stopped.
        if (points.size() <= 2)
            return false;
        G3D::Vector3 const& end = points.back();
        float const leftThen = std::sqrt((destX - end.x) * (destX - end.x) + (destY - end.y) * (destY - end.y) + (destZ - end.z) * (destZ - end.z));
        if (leftThen <= Arrived)
        {
            // An incomplete path can end at the target itself whatever lies between it and where the navmesh part
            // ends (14 yd of rock: the path over the hill above Booty Bay's tunnel "reached" a node inside the
            // tunnel). Its last stretch has to be one the ground guard lets a bot walk.
            constexpr std::size_t TailPoints = 12;
            Movement::PointsArray tail(points.end() - std::min(points.size(), TailPoints), points.end());
            PathReport tailReport;
            Sample(source, tail, 0.0f, tailReport);
            if (tailReport.LeavesTheGround())
                return false;
            if (length)
                *length = walked + hopLength;
            return true;
        }
        if (leftThen > left - Headway)
            return false;
        walked += hopLength;
        x = end.x;
        y = end.y;
        z = end.z;
    }
    return false;
}
}

bool GuildAdventure::PathPointAhead(WorldObject const* source, float x, float y, float z, float destX, float destY, float destZ,
    float ahead, float& outX, float& outY, float& outZ)
{
    constexpr float MinLength = 15.0f;
    if (!source || !source->FindMap())
        return false;
    EnsureNavmesh(source, x, y, destX, destY);
    PathGenerator path(source);
    if (!path.CalculatePath(x, y, z, destX, destY, destZ, false))
        return false;
    Movement::PointsArray const& points = path.GetPath();
    if ((path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH | PATHFIND_SHORTCUT)) || points.size() < 3)
        return false;
    float along = 0.0f;
    std::size_t at = 0;
    for (std::size_t i = 1; i < points.size() && along < ahead; ++i)
    {
        along += (points[i] - points[i - 1]).length();
        at = i;
    }
    if (along < MinLength)
        return false;
    outX = points[at].x;
    outY = points[at].y;
    outZ = points[at].z;
    return true;
}

bool GuildAdventure::NavmeshStart(WorldObject const* source, float x, float y, float z, float towardX, float towardY, float towardZ,
    float& outX, float& outY, float& outZ)
{
    if (!source || !source->FindMap())
        return false;
    EnsureNavmesh(source, x, y, towardX, towardY);
    PathGenerator path(source);
    if (!path.CalculatePath(x, y, z, towardX, towardY, towardZ, false) || (path.GetPathType() & PATHFIND_NOPATH) ||
        path.GetPath().size() < 2 || (path.GetPathType() & PATHFIND_NOT_USING_PATH))
        return false;
    G3D::Vector3 const& first = path.GetPath().front();
    outX = first.x;
    outY = first.y;
    outZ = first.z;
    return true;
}

float GuildAdventure::WaterProbeZ(Map const* map, float x, float y, float z)
{
    // The core's water test (GridTerrainData::GetLiquidData) wants the point no more than 0.2 yd under the terrain.
    constexpr float BedSlackYards = 3.0f;
    if (!map)
        return z;
    float const ground = map->GetGridHeight(x, y);
    if (ground > INVALID_HEIGHT && z < ground && ground - z <= BedSlackYards)
        return ground + 0.05f;
    return z;
}

bool GuildAdventure::IsSwimming(Unit const* unit)
{
    Map const* map = unit ? unit->FindMap() : nullptr;
    return map && map->IsInWater(unit->GetPhaseMask(), unit->GetPositionX(), unit->GetPositionY(),
        WaterProbeZ(map, unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ()), unit->GetCollisionHeight());
}

bool GuildAdventure::IsDiving(Unit const* unit)
{
    Map const* map = unit ? unit->FindMap() : nullptr;
    return map && map->IsUnderWater(unit->GetPhaseMask(), unit->GetPositionX(), unit->GetPositionY(),
        WaterProbeZ(map, unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ()), unit->GetCollisionHeight());
}

PathReport GuildAdventure::ProbePath(WorldObject const* source, float x, float y, float z, float destX, float destY, float destZ)
{
    PathReport report;
    if (!source || !source->FindMap())
        return report;

    EnsureNavmesh(source, x, y, destX, destY);
    PathGenerator path(source);
    if (!path.CalculatePath(x, y, z, destX, destY, destZ, false))
        return report;

    report.valid = true;
    report.type = path.GetPathType();
    report.points = path.GetPath().size();
    report.straight = std::sqrt((destX - x) * (destX - x) + (destY - y) * (destY - y) + (destZ - z) * (destZ - z));
    // The point movement follows the path's points; without a path (PATHFIND_NOPATH) it runs a straight line to the
    // destination. A path of two points is a straight line too, to wherever the path finder put its end: for a target
    // it cannot reach from the start polygon that is the target itself.
    report.straightLine = (report.type & PATHFIND_NOPATH) || report.points <= 2;
    Movement::PointsArray line = path.GetPath();
    if ((report.type & PATHFIND_NOPATH) || line.size() < 2)
    {
        line.clear();
        line.push_back(G3D::Vector3(x, y, z));
        line.push_back(G3D::Vector3(destX, destY, destZ));
    }
    report.endX = line.back().x;
    report.endY = line.back().y;
    report.endZ = line.back().z;
    Sample(source, line, 0.0f, report);
    return report;
}

PathReport GuildAdventure::ProbeMovement(Unit const* unit, PathGenerator const& path, bool usesPath, float destX, float destY, float destZ)
{
    // The unit stands off the navmesh when the path's first point lies farther from it than this.
    constexpr float OffMeshStart = 3.0f;
    constexpr uint32 Suspicious = PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE | PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH |
        PATHFIND_FARFROMPOLY;

    PathReport report;
    if (!unit || !unit->FindMap())
        return report;

    report.valid = true;
    report.type = path.GetPathType();
    report.points = path.GetPath().size();
    report.straight = unit->GetExactDist(destX, destY, destZ);
    report.straightLine = !usesPath;

    // What PointMovementGenerator does next: MovebyPath along the points, or MoveSplineInit::MoveTo, which takes the
    // path's points again unless there is no path at all (then a straight line to the destination).
    Movement::PointsArray line = path.GetPath();
    if ((report.type & PATHFIND_NOPATH) || line.size() < 2)
    {
        line.clear();
        line.push_back(G3D::Vector3(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ()));
        line.push_back(G3D::Vector3(destX, destY, destZ));
    }
    // The spline starts where the unit stands; the path's first point is the navmesh's idea of it, which can lie on
    // another level when the unit stands off the mesh.
    G3D::Vector3 const position(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ());
    bool const offMesh = (line[0] - position).length() > OffMeshStart;
    line[0] = position;
    report.endX = line.back().x;
    report.endY = line.back().y;
    report.endZ = line.back().z;

    // A plain navmesh path is walkable ground by construction (the floor lookup would only misjudge it inside
    // buildings: stairs, upper storeys). What the mesh does not vouch for is the step onto it when the unit stands
    // off the mesh, and the step off it at the end.
    if (usesPath && !(report.type & Suspicious))
    {
        // Except its last leg, from the last corner to the destination: near the last polygon the core ends a path at
        // the point asked for, also when that lies in a wall. (A Tin Vein in the wall of Thelgen Rock: the navmesh ends
        // 4 yd before it, the last leg of a "normal" path ran into the rock, and the bot stood there "under the ground"
        // until a rescue.)
        constexpr float LastLegMin = 1.0f;
        if (line.size() >= 2 && (line.back() - line[line.size() - 2]).length() > LastLegMin)
        {
            Movement::PointsArray lastLeg;
            lastLeg.push_back(line[line.size() - 2]);
            lastLeg.push_back(line.back());
            Sample(unit, lastLeg, 0.0f, report);
        }
        if (offMesh)
        {
            Movement::PointsArray step;
            step.push_back(line[0]);
            step.push_back(line[1]);
            Sample(unit, step, 0.0f, report);
        }
        // (Sample set the length of the legs it looked at: the whole path's length now.)
        report.length = 0.0f;
        for (std::size_t i = 1; i < line.size(); ++i)
            report.length += (line[i] - line[i - 1]).length();
        return report;
    }
    Sample(unit, line, 0.0f, report);
    return report;
}

std::string GuildAdventure::DescribePathType(uint32 type)
{
    static std::pair<uint32, char const*> const Flags[] =
    {
        { PATHFIND_NORMAL, "normal" }, { PATHFIND_SHORTCUT, "shortcut" }, { PATHFIND_INCOMPLETE, "incomplete" },
        { PATHFIND_NOPATH, "no path" }, { PATHFIND_NOT_USING_PATH, "not using a path" }, { PATHFIND_SHORT, "short" },
        { PATHFIND_FARFROMPOLY_START, "start far from the navmesh" }, { PATHFIND_FARFROMPOLY_END, "end far from the navmesh" }
    };
    std::string text;
    for (auto const& [flag, name] : Flags)
        if (type & flag)
            text += (text.empty() ? "" : " + ") + std::string(name);
    return text.empty() ? "blank" : text;
}

std::string GuildAdventure::DescribeShort(PathReport const& report)
{
    return Acore::StringFormat("{}, {} points, {:.0f} yd{}; {:.0f} yd through solid ground, {:.0f} yd through the air, worst at "
        "({:.1f}, {:.1f}, {:.1f})", DescribePathType(report.type), report.points, report.length,
        report.straightLine ? " as a straight line" : "", report.throughSolid, report.offGround - report.throughSolid, report.worstX,
        report.worstY, report.worstZ);
}

std::vector<std::string> GuildAdventure::Describe(PathReport const& report)
{
    std::vector<std::string> lines;
    if (!report.valid)
    {
        lines.push_back("no path computed (invalid coordinates or no map)");
        return lines;
    }
    lines.push_back(Acore::StringFormat("path: {}; {} points, {:.0f} yd along the path, {:.0f} yd straight to the target; ends at "
        "({:.1f}, {:.1f}, {:.1f}){}", DescribePathType(report.type), report.points, report.length, report.straight, report.endX,
        report.endY, report.endZ, report.straightLine ? " as a STRAIGHT LINE" : ""));
    if (report.offGround > 0.0f)
        lines.push_back(Acore::StringFormat("off the ground: {:.0f} yd through solid ground, {:.0f} yd through the air, worst at "
            "({:.1f}, {:.1f}, {:.1f}){}", report.throughSolid, report.offGround - report.throughSolid, report.worstX, report.worstY,
            report.worstZ, report.LeavesTheGround() ? " -> leaves the ground" : " (passes: a step or a jump)"));
    else
        lines.push_back("on the ground all the way");
    return lines;
}
