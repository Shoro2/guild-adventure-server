/*
 * Walking for the order planner: OrderService::WalkTo (docs/orders.md "Routes"). Part of OrderService; kept in a file of
 * its own.
 *
 * A walk of a task (to a zone hub, an ender, a dock, an NPC) used to be one NewRpg "go camp" to the destination: the
 * navmesh search towards it, step by step, across whatever lies in between. A longer walk now follows a route of the
 * road map (RoadMap): the planner hands NewRpg a point of the route a short way ahead of the bot and moves it on as
 * the bot gets there, so every step is one the navmesh search covers and the way as a whole avoids what is far above
 * the bot's level.
 */

#include "OrderService.h"
#include "DungeonNav.h"
#include "BotDriver.h"
#include "BotSettings.h"
#include "DatabaseEnv.h"
#include "GroundGuard.h"
#include "PathProbe.h"
#include "PathGenerator.h"
#include "GuildAdventureSettings.h"
#include "Log.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "Player.h"
#include "RiskModel.h"
#include "RoadMap.h"
#include "StringFormat.h"
#include "Map.h"
#include "GameObject.h"
#include "ObjectAccessor.h"
#include "Transport.h"
#include "Timer.h"
#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>

using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";
    constexpr uint32 AssertGapMs = 5 * IN_MILLISECONDS;
    // A route is planned again at most this often (a destination that moves, a bot that strayed).
    constexpr uint32 RoutePlanGapMs = 15 * IN_MILLISECONDS;
    // The bot has passed a route point when it is this close to it.
    constexpr float PassDistance = 40.0f;
    constexpr float PassHeight = 25.0f;
    // NewRpg's target lies this many route points ahead of the last one passed (they are about five yards apart), and
    // is moved on when the bot has come this many points further.
    constexpr float AheadDistance = 80.0f;
    // The bot is on its route when the point it has reached is this close; farther away it walks to that point first.
    constexpr float OnRouteDistance = 60.0f;
    // A diving bot whose destination is farther away than this swims up before it sets out.
    constexpr float SurfaceFirstDistance = 60.0f;
    constexpr float RetargetDistance = 40.0f;
    // How far ahead of the last passed point the next one is looked for (a fight may carry the bot along the route).
    constexpr uint32 ScanPoints = 80;
    // Farther than this from its route (a death, a fight that led away): plan again from where the bot is.
    constexpr float StrayDistance = 250.0f;
    // A bot that has not moved this far this long after taking a route does not get going on it.
    constexpr uint32 RouteStartMs = 40 * IN_MILLISECONDS;
    constexpr float RouteStartDistance = 10.0f;
    // A walk that found no route and has not moved that far after this time has no way at all.
    constexpr uint32 StraightStartMs = 60 * IN_MILLISECONDS;
    // A route is not worth it when it is this much longer than the straight line.
    constexpr float MaxDetour = 12.0f;
    // Walks shorter than this are not looked at (the last steps to an NPC).
    constexpr float ShortProbeMin = 20.0f;
    // Walks shorter than ShortWalk go straight when the route would be ShortWalkDetour times as long and nothing more
    // than SafeLevels above the bot lives on the straight line.
    constexpr float ShortWalk = 1000.0f;
    constexpr float ShortWalkDetour = 1.5f;
    constexpr int32 SafeLevels = 0;

    float Dist2d(float x1, float y1, float x2, float y2)
    {
        return std::sqrt((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2));
    }

    float Dist2d(Player const* player, float x, float y)
    {
        float const dx = player->GetPositionX() - x;
        float const dy = player->GetPositionY() - y;
        return std::sqrt(dx * dx + dy * dy);
    }
}

namespace
{
    // The walks to the first and from the last node of a route: not where the path finder has nothing (no path, a
    // straight line, a path that ends where it starts).
    RoadMap::LegCheck LegCheckFor(Player* player)
    {
        return [player](RoutePoint const& from, RoutePoint const& to)
        {
            return WalkReaches(player, from.x, from.y, from.z, to.x, to.y, to.z);
        };
    }
}

float OrderService::WalkLength(Player* player, uint32 mapId, float x, float y, float z) const
{
    if (mapId != player->GetMapId())
        return -1.0f;
    float const direct = Dist2d(player, x, y);
    if (direct <= 15.0f)
        return direct;
    // A route of the road map for long walks (it knows the roads), else what the path finder walks.
    float walked = 0.0f;
    if (direct <= sBotSettings.GetFloat(player, Setting::RoadsMinDistance) &&
        WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), x, y, z, &walked))
        return std::max(walked, direct);
    if (sBotSettings.IsOn(player, Setting::RoadsEnable))
    {
        RoadRoute const route = sRoadMap.FindRoute(mapId, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), x, y, z,
            player->GetTeamId(), player->GetLevel(), LegCheckFor(player));
        if (route.found)
            return std::max(route.length, direct);
    }
    if (direct > sBotSettings.GetFloat(player, Setting::RoadsMinDistance) &&
        WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), x, y, z, &walked))
        return std::max(walked, direct);
    return -1.0f;
}

namespace
{
    struct PathBridge
    {
        uint32 id = 0;
        uint32 mapId = 0;
        RoutePoint a, b;
        std::string comment;
    };
    std::vector<PathBridge> sPathBridges;
    constexpr float BridgeMaxYards = 15.0f; // the two points of a crossing this far apart at most
}

void OrderService::LoadPathBridges()
{
    sPathBridges.clear();
    uint32 refused = 0;
    if (QueryResult result = WorldDatabase.Query("SELECT id, map, x1, y1, z1, x2, y2, z2, comment FROM ga_path_bridge ORDER BY id"))
        do
        {
            Field* fields = result->Fetch();
            PathBridge bridge;
            bridge.id = fields[0].Get<uint32>();
            bridge.mapId = fields[1].Get<uint16>();
            bridge.a = { fields[2].Get<float>(), fields[3].Get<float>(), fields[4].Get<float>() };
            bridge.b = { fields[5].Get<float>(), fields[6].Get<float>(), fields[7].Get<float>() };
            bridge.comment = fields[8].Get<std::string>();
            float const length = std::sqrt((bridge.a.x - bridge.b.x) * (bridge.a.x - bridge.b.x) +
                (bridge.a.y - bridge.b.y) * (bridge.a.y - bridge.b.y) + (bridge.a.z - bridge.b.z) * (bridge.a.z - bridge.b.z));
            if (length > BridgeMaxYards)
            {
                LOG_ERROR("module", "mod-guild-adventure: ga_path_bridge {} ({}): its points are {:.0f} yd apart ({} at most), skipped",
                    bridge.id, bridge.comment, length, uint32(BridgeMaxYards));
                ++refused;
                continue;
            }
            sPathBridges.push_back(std::move(bridge));
        } while (result->NextRow());
    LOG_INFO("module", "mod-guild-adventure: {} crossings of gaps in the path finder's ground (ga_path_bridge){}", sPathBridges.size(),
        refused ? Acore::StringFormat(", {} refused", refused) : std::string());
}

// The keep of Stormwind: the ground of the library wing and the ground of the hall before it are two pieces for the
// path finder. They lie 2.5 yd apart where the corridor to the library leaves the entrance ramp, at (-8471.8, 413.0,
// 111.6) and (-8469.3, 413.0, 112.0); a guard's patrol walks across. A level-28 hunter sailed from Auberdine for its
// hand-in at the historian in the library, stood at the hall's nearest point to him, a wall and 68 yd away ("the way
// leaves the ground"), gave up, sailed back, and set out again, three times in an evening.
// (A search for such gaps by the path finder alone was tried: the two grounds are nearest to each other through a
// wall, and nothing tells the doorway from the wall but a look at the place. So the crossings are hand-listed.)
bool OrderService::BridgeGap(Player* player, BotTask& task, float x, float y, float z)
{
    constexpr float BridgeReach = 400.0f;   // a crossing this near to the bot and to the target
    constexpr float BridgeEdge = 2.5f;      // the bot stands at its first point
    constexpr uint8 BridgesMax = 3;         // per task
    if (sPathBridges.empty() || task.bridges >= BridgesMax)
        return false;
    auto whole = [](PathGenerator const& path) { return !(path.GetPathType() & (PATHFIND_INCOMPLETE | PATHFIND_NOPATH)); };

    if (!task.bridgeFound)
    {
        if (task.bridgeSearched)
            return false;
        task.bridgeSearched = true;
        for (PathBridge const& bridge : sPathBridges)
        {
            if (bridge.mapId != player->GetMapId() || player->GetExactDist2d(bridge.a.x, bridge.a.y) > BridgeReach ||
                std::sqrt((bridge.a.x - x) * (bridge.a.x - x) + (bridge.a.y - y) * (bridge.a.y - y)) > BridgeReach)
                continue;
            for (bool forward : { true, false })
            {
                RoutePoint const& edge = forward ? bridge.a : bridge.b;
                RoutePoint const& beyond = forward ? bridge.b : bridge.a;
                // The bot's ground reaches the one point ...
                if (player->GetExactDist(edge.x, edge.y, edge.z) > BridgeEdge)
                {
                    PathGenerator to(player);
                    to.CalculatePath(edge.x, edge.y, edge.z, false);
                    if (!whole(to))
                        continue;
                }
                // ... and the target is reached from the other.
                PathGenerator on(player);
                on.CalculatePath(beyond.x, beyond.y, beyond.z, x, y, z, false);
                if (!whole(on))
                    continue;
                task.bridgeFound = true;
                task.bridgeEdge = edge;
                task.bridgeBeyond = beyond;
                LOG_INFO(ActivityLog, "activity: {} takes the crossing \"{}\" on its way to ({:.0f}, {:.0f}, {:.0f}): the path finder's "
                    "ground ends at ({:.1f}, {:.1f}, {:.1f}) and goes on at ({:.1f}, {:.1f}, {:.1f}); it walks there ({:.0f} yd) and steps "
                    "across", player->GetName(), bridge.comment, x, y, z, edge.x, edge.y, edge.z, beyond.x, beyond.y, beyond.z,
                    player->GetExactDist(edge.x, edge.y, edge.z));
                break;
            }
            if (task.bridgeFound)
                break;
        }
        if (!task.bridgeFound)
            return false;
    }

    RoutePoint const& edge = task.bridgeEdge;
    RoutePoint const& beyond = task.bridgeBeyond;
    if (BotDriver::GetRpgState(player) != RpgState::Rest)
        BotDriver::Rest(player);
    ++task.ownMade;
    task.ownStepAt = std::max<uint32>(getMSTime(), 1);
    if (player->GetExactDist(edge.x, edge.y, edge.z) > BridgeEdge)
    {
        // To its first point.
        task.ownStepX = edge.x;
        task.ownStepY = edge.y;
        task.ownStepZ = edge.z;
        player->GetMotionMaster()->MovePoint(0, edge.x, edge.y, edge.z);
        return true;
    }
    ++task.bridges;
    task.bridgeFound = false;
    task.bridgeSearched = false;
    LOG_INFO(ActivityLog, "activity: {} steps across the gap in the path finder's ground, from ({:.1f}, {:.1f}, {:.1f}) to ({:.1f}, {:.1f}, "
        "{:.1f})", player->GetName(), edge.x, edge.y, edge.z, beyond.x, beyond.y, beyond.z);
    // (The straight step is the planner's own: the ground guard would ask the path finder, which has no path here.)
    sGroundGuard.AllowFor(player->GetGUID(), 4 * IN_MILLISECONDS);
    task.ownStepX = beyond.x;
    task.ownStepY = beyond.y;
    task.ownStepZ = beyond.z;
    player->GetMotionMaster()->MovePoint(0, beyond.x, beyond.y, beyond.z, FORCED_MOVEMENT_NONE, 0.f, 0.f, false);
    return true;
}

bool OrderService::ChainReaches(Player* player, float x, float y, float z, float& length) const
{
    constexpr float HopYards = 180.0f;     // one hop of the path finder
    constexpr float HopMissYards = 35.0f;  // a hop that ends this near to its point is one (a tree, a rock stands there)
    constexpr int HopsMax = 10;
    Map* map = player->GetMap();
    float cx = player->GetPositionX(), cy = player->GetPositionY(), cz = player->GetPositionZ();
    length = 0.0f;
    for (int hop = 0; hop < HopsMax; ++hop)
    {
        float const left = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
        bool const last = left <= HopYards * 1.3f;
        float nx = x, ny = y, nz = z;
        if (!last)
        {
            nx = cx + (x - cx) * HopYards / left;
            ny = cy + (y - cy) * HopYards / left;
            nz = map->GetHeight(player->GetPhaseMask(), nx, ny, cz + 80.0f, true, 300.0f);
            if (nz <= INVALID_HEIGHT)
                return false;
        }
        PathGenerator path(player);
        path.CalculatePath(cx, cy, cz, nx, ny, nz, false);
        Movement::PointsArray const& points = path.GetPath();
        if ((path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH)) || points.size() < 2)
            return false;
        G3D::Vector3 const& end = points.back();
        float const miss = std::sqrt((end.x - nx) * (end.x - nx) + (end.y - ny) * (end.y - ny));
        if (miss > (last ? 12.0f : HopMissYards))
            return false;
        // (A hop that got no nearer leads nowhere.)
        if (!last && std::sqrt((x - end.x) * (x - end.x) + (y - end.y) * (y - end.y)) > left - HopYards * 0.5f)
            return false;
        for (std::size_t i = 1; i < points.size(); ++i)
            length += (points[i] - points[i - 1]).length();
        if (last)
            return true;
        cx = end.x;
        cy = end.y;
        cz = end.z;
    }
    return false;
}

namespace
{
    constexpr float SwimPi = 3.14159265f;
    constexpr float SwimCellYards = 6.0f;
    constexpr float SwimMinDepth = 2.0f;

    // The water's surface at a point where it is deep enough to swim in (not lava, not slime); INVALID_HEIGHT else.
    float SwimSurface(Player const* player, float x, float y)
    {
        Map* map = player->GetMap();
        float const level = map->GetWaterLevel(x, y);
        if (level <= INVALID_HEIGHT)
            return INVALID_HEIGHT;
        LiquidData const liquid = map->GetLiquidData(player->GetPhaseMask(), x, y, level - 1.0f, player->GetCollisionHeight(), {});
        if (liquid.Status == LIQUID_MAP_NO_WATER || (liquid.Flags & (MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME)))
            return INVALID_HEIGHT;
        float const ground = map->GetHeight(player->GetPhaseMask(), x, y, level + 2.0f, true, 60.0f);
        if (ground > INVALID_HEIGHT && level - ground < SwimMinDepth)
            return INVALID_HEIGHT;
        return level;
    }

    // A straight swim between two points in the water: water all the way, nothing solid in between.
    bool SwimsStraight(Player const* player, float ax, float ay, float bx, float by)
    {
        float const length = std::hypot(bx - ax, by - ay);
        int const steps = std::max(1, int(length / 3.0f));
        float lastLevel = INVALID_HEIGHT;
        for (int i = 0; i <= steps; ++i)
        {
            float const t = float(i) / float(steps);
            float const level = SwimSurface(player, ax + (bx - ax) * t, ay + (by - ay) * t);
            if (level <= INVALID_HEIGHT)
                return false;
            lastLevel = level;
        }
        float const az = SwimSurface(player, ax, ay) - 1.0f;
        float const bz = lastLevel - 1.0f;
        return player->GetMap()->isInLineOfSight(ax, ay, az + 0.5f, bx, by, bz + 0.5f, player->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
            VMAP::ModelIgnoreFlags::Nothing);
    }
}

bool OrderService::StepSwim(Player* player, BotTask& task, float x, float y, float z)
{
    constexpr float MinYards = 30.0f;
    constexpr float MaxYards = 1500.0f;
    constexpr float LandingMax = 60.0f;
    constexpr float EntryMax = 40.0f;
    // (The way out of a bay may lead far around: Booty Bay's mouth lies 200 yd beyond the line from its docks to the
    // Bloodsail hulks off the Wild Shore.)
    constexpr float BoxMargin = 300.0f;
    constexpr uint32 MaxCells = 60000;
    constexpr uint32 LegGiveUpMs = 40 * IN_MILLISECONDS;
    uint32 const now = std::max<uint32>(getMSTime(), 1);

    // On its way through the water: point by point, NewRpg resting.
    if (!task.swim.empty())
    {
        // (A fight on the way holds the leg's clock: Mojowind fought a Drysnap Pincer 24 s on its swim to Shadowprey's flight
        // master and gave the swim up five seconds after it, "gets no nearer", on the j5 instance, 2026-11-26.)
        if (!player->IsAlive() || player->IsInCombat() || player->IsNonMeleeSpellCast(false))
        {
            task.swimLegSince = now;
            return true;
        }
        BotTask::SwimPoint const* next = &task.swim[task.swimNext];
        // (In three dimensions: the last leg may lead straight down to a target on the sea floor, below the point over it.)
        auto const leftTo = [&](BotTask::SwimPoint const* point)
        {
            return point->walk ? Dist2d(player, point->x, point->y) : player->GetExactDist(point->x, point->y, point->z);
        };
        float left = leftTo(next);
        if (left < 4.0f)
        {
            if (++task.swimNext >= task.swim.size())
            {
                LOG_INFO(ActivityLog, "activity: {} has swum to ({:.0f}, {:.0f}): the walk to its target goes on from there", player->GetName(),
                    next->x, next->y);
                task.swim.clear();
                return false;
            }
            next = &task.swim[task.swimNext];
            left = leftTo(next);
            task.swimLegSince = now;
            task.swimLegBest = left;
            task.swimMovedAt = 0;
        }
        if (left + 2.0f < task.swimLegBest)
        {
            task.swimLegBest = left;
            task.swimLegSince = now;
        }
        else if (now - task.swimLegSince >= LegGiveUpMs)
        {
            LOG_INFO(ActivityLog, "activity: {} gets no nearer to ({:.0f}, {:.0f}) on its swim ({:.0f} yd left): it gives the swim up",
                player->GetName(), next->x, next->y, left);
            task.swim.clear();
            return false;
        }
        // Down to a point under the surface: nothing that holds it on the water. A shaman's AI put Water Walking on
        // Mojowind a second after its swim to a Drysnap Crawler 47 yd down began (Clam Bait, on the j5 instance,
        // 2026-11-26): on the water no character dives. The AI rests meanwhile, and the aura goes.
        if (!next->walk)
            if (float const surface = SwimSurface(player, next->x, next->y); surface > INVALID_HEIGHT && next->z < surface - 3.0f &&
                player->GetPositionZ() > next->z + 3.0f)
            {
                BotDriver::PauseAI(player, 2500);
                if (player->HasAuraType(SPELL_AURA_WATER_WALK))
                    player->RemoveAurasByType(SPELL_AURA_WATER_WALK);
            }
        if (!task.swimMovedAt || now - task.swimMovedAt >= 2 * IN_MILLISECONDS || !player->isMoving())
        {
            if (BotDriver::GetRpgState(player) != RpgState::Rest)
                BotDriver::Rest(player);
            task.swimMovedAt = now;
            player->GetMotionMaster()->MovePoint(0, next->x, next->y, next->z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, next->walk);
        }
        return true;
    }
    // Only after a step towards the target was refused for having no way on the ground (UpdateBot arms it).
    float const direct = Dist2d(player, x, y);
    if (!task.swimArmed || task.swimLooked || direct < MinYards || direct > MaxYards || player->GetMap()->IsDungeon())
        return false;
    task.swimLooked = true;

    // Only a target with water beside it ...
    bool waterNear = false;
    for (int i = 0; i < 16 && !waterNear; ++i)
        for (float r : { 20.0f, 45.0f })
            if (SwimSurface(player, x + std::cos(float(i) * SwimPi / 8.0f) * r, y + std::sin(float(i) * SwimPi / 8.0f) * r) >
                INVALID_HEIGHT)
            {
                waterNear = true;
                break;
            }
    // ... or a bot in the water whose target lies on land (the last clam of "Pearl Diving" on the sea floor of the Vile
    // Reef, the next target ashore: it was rescued as "cut off from every road").
    bool const fromWater = IsSwimming(player) || IsDiving(player);
    if (!waterNear && !fromWater)
        return false;

    // The landing: the target itself when it lies in the water (a grind spot among fish on a lake's floor), else the
    // nearest point in the water from which the path finder leads to the target.
    BotTask::SwimPoint landing;
    bool landed = false;
    if (float const level = SwimSurface(player, x, y); level > INVALID_HEIGHT && z < level)
    {
        landing = { x, y, std::min(z, level - 0.5f) };
        landed = true;
    }
    for (float r = 8.0f; r <= LandingMax && !landed && waterNear; r += 6.0f)
        for (int i = 0; i < 24 && !landed; ++i)
        {
            float const px = x + std::cos(float(i) * SwimPi / 12.0f) * r;
            float const py = y + std::sin(float(i) * SwimPi / 12.0f) * r;
            float const level = SwimSurface(player, px, py);
            if (level <= INVALID_HEIGHT)
                continue;
            PathReport const report = ProbePath(player, px, py, level - 0.5f, x, y, z);
            if (!report.valid || (report.type & PATHFIND_NOPATH) || report.LeavesTheGround() ||
                std::hypot(report.endX - x, report.endY - y) > 4.0f || std::fabs(report.endZ - z) > 4.0f)
                continue;
            landing = { px, py, level - 0.5f };
            landed = true;
        }
    // Out of the water towards a target on land: the water near the bot from which the path finder leads onto the land
    // on the target's side.
    if (!landed && fromWater)
    {
        constexpr float AshoreMax = 300.0f;
        constexpr int AshoreTries = 12;
        struct Water { float x, y, z, score; };
        std::vector<Water> waters;
        for (float r = 10.0f; r <= AshoreMax; r += 15.0f)
            for (int i = 0; i < 24; ++i)
            {
                float const px = player->GetPositionX() + std::cos(float(i) * SwimPi / 12.0f) * r;
                float const py = player->GetPositionY() + std::sin(float(i) * SwimPi / 12.0f) * r;
                float const level = SwimSurface(player, px, py);
                if (level > INVALID_HEIGHT)
                    waters.push_back({ px, py, level, r + std::hypot(x - px, y - py) });
            }
        std::sort(waters.begin(), waters.end(), [](Water const& a, Water const& b) { return a.score < b.score; });
        int tries = 0;
        for (Water const& water : waters)
        {
            if (++tries > AshoreTries)
                break;
            PathReport const report = ProbePath(player, water.x, water.y, water.z - 0.5f, x, y, z);
            if (!report.valid || (report.type & PATHFIND_NOPATH) || report.straightLine || report.LeavesTheGround())
                continue;
            // It leads onto the land, towards the target.
            float const endWater = player->GetMap()->GetWaterLevel(report.endX, report.endY);
            if (std::hypot(report.endX - water.x, report.endY - water.y) < 20.0f ||
                (endWater > INVALID_HEIGHT && report.endZ < endWater - 1.0f) ||
                std::hypot(report.endX - x, report.endY - y) + 20.0f > std::hypot(water.x - x, water.y - y))
                continue;
            landing = { water.x, water.y, water.z - 0.5f };
            landed = true;
        }
    }
    if (!landed)
        return false;

    // Into the water: where it swims already, or the nearest water a straight step leads to; else water further off
    // that a walk on the ground reaches, towards the landing (a cliff top above Booty Bay: the docks below, the water
    // beside them leads on).
    BotTask::SwimPoint entry;
    BotTask::SwimPoint shore;
    bool entered = false, walkFirst = false;
    auto const findEntry = [&](float fx, float fy, float fz)
    {
        if (float const here = SwimSurface(player, fx, fy); here > INVALID_HEIGHT && fz < here + 1.0f)
        {
            entry = { fx, fy, here - 0.5f };
            return true;
        }
        for (float r = 4.0f; r <= EntryMax; r += 4.0f)
            for (int i = 0; i < 16; ++i)
            {
                float const px = fx + std::cos(float(i) * SwimPi / 8.0f) * r;
                float const py = fy + std::sin(float(i) * SwimPi / 8.0f) * r;
                float const level = SwimSurface(player, px, py);
                if (level <= INVALID_HEIGHT || !player->GetMap()->isInLineOfSight(fx, fy, fz + 1.5f, px, py, level + 0.5f,
                        player->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                    continue;
                entry = { px, py, level - 0.5f };
                return true;
            }
        return false;
    };
    entered = findEntry(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
    if (!entered)
    {
        constexpr float ShoreMax = 250.0f;   // how far a walk to the water may lead
        constexpr int ShoreTries = 10;       // paths asked for
        struct Water { float x, y, z, score; };
        std::vector<Water> waters;
        for (float r = EntryMax; r <= ShoreMax; r += 15.0f)
            for (int i = 0; i < 24; ++i)
            {
                float const px = player->GetPositionX() + std::cos(float(i) * SwimPi / 12.0f) * r;
                float const py = player->GetPositionY() + std::sin(float(i) * SwimPi / 12.0f) * r;
                float const level = SwimSurface(player, px, py);
                if (level > INVALID_HEIGHT)
                    waters.push_back({ px, py, level, r + std::hypot(landing.x - px, landing.y - py) });
            }
        std::sort(waters.begin(), waters.end(), [](Water const& a, Water const& b) { return a.score < b.score; });
        int tries = 0;
        for (Water const& water : waters)
        {
            if (++tries > ShoreTries)
                break;
            PathReport const report = ProbePath(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                water.x, water.y, water.z - 0.5f);
            if (!report.valid || (report.type & PATHFIND_NOPATH) || report.straightLine || report.LeavesTheGround() ||
                std::hypot(report.endX - water.x, report.endY - water.y) > 8.0f)
                continue;
            shore = { report.endX, report.endY, report.endZ, true };
            entry = { water.x, water.y, water.z - 0.5f };
            entered = walkFirst = true;
            break;
        }
    }
    if (!entered)
    {
        LOG_INFO(ActivityLog, "activity: {} finds no water to swim from towards ({:.0f}, {:.0f}): no way on the ground leads there",
            player->GetName(), x, y);
        return false;
    }

    // The way through the water: a search over cells of water around both ends, then straight stretches.
    float const minX = std::min(entry.x, landing.x) - BoxMargin, maxX = std::max(entry.x, landing.x) + BoxMargin;
    float const minY = std::min(entry.y, landing.y) - BoxMargin, maxY = std::max(entry.y, landing.y) + BoxMargin;
    int const width = int((maxX - minX) / SwimCellYards) + 1, height = int((maxY - minY) / SwimCellYards) + 1;
    auto const cellOf = [&](float px, float py) { return std::make_pair(int((px - minX) / SwimCellYards), int((py - minY) / SwimCellYards)); };
    auto const keyOf = [&](int cx, int cy) { return cy * width + cx; };
    auto const centre = [&](int cx, int cy) { return std::make_pair(minX + (float(cx) + 0.5f) * SwimCellYards, minY + (float(cy) + 0.5f) * SwimCellYards); };
    std::unordered_map<int, int8> water; // cell -> 1 water, 0 none
    auto const isWater = [&](int cx, int cy)
    {
        if (cx < 0 || cy < 0 || cx >= width || cy >= height)
            return false;
        auto [itr, added] = water.emplace(keyOf(cx, cy), 0);
        if (added)
        {
            auto const [px, py] = centre(cx, cy);
            itr->second = SwimSurface(player, px, py) > INVALID_HEIGHT ? 1 : 0;
        }
        return itr->second == 1;
    };
    auto const [sx, sy] = cellOf(entry.x, entry.y);
    auto const [gx, gy] = cellOf(landing.x, landing.y);
    std::unordered_map<int, int> from;
    std::unordered_map<int, float> cost;
    using Open = std::pair<float, int>;
    std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
    int const start = keyOf(sx, sy), goal = keyOf(gx, gy);
    cost[start] = 0.0f;
    from[start] = start;
    open.push({ 0.0f, start });
    bool found = false;
    uint32 expanded = 0;
    while (!open.empty() && expanded < MaxCells)
    {
        int const current = open.top().second;
        open.pop();
        ++expanded;
        if (current == goal)
        {
            found = true;
            break;
        }
        int const cx = current % width, cy = current / width;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
            {
                if (!dx && !dy)
                    continue;
                int const nx = cx + dx, ny = cy + dy;
                int const next = keyOf(nx, ny);
                // (The two ends count as water: the entry may lie at the shore, the landing beside a hull.)
                if (next != goal && next != start && !isWater(nx, ny))
                    continue;
                float const step = (dx && dy) ? 1.4142f : 1.0f;
                float const total = cost[current] + step;
                auto const known = cost.find(next);
                if (known != cost.end() && known->second <= total)
                    continue;
                cost[next] = total;
                from[next] = current;
                open.push({ total + std::hypot(float(gx - nx), float(gy - ny)), next });
            }
    }
    if (!found)
    {
        LOG_INFO(ActivityLog, "activity: {} finds no way through the water towards ({:.0f}, {:.0f}) from ({:.0f}, {:.0f}) to ({:.0f}, "
            "{:.0f}) ({} cells looked at): no way on the ground leads there either", player->GetName(), x, y, entry.x, entry.y, landing.x,
            landing.y, expanded);
        return false;
    }
    std::vector<BotTask::SwimPoint> cells;
    for (int at = goal; ; at = from[at])
    {
        auto const [px, py] = centre(at % width, at / width);
        cells.push_back({ px, py, 0.0f });
        if (at == start)
            break;
    }
    std::reverse(cells.begin(), cells.end());
    cells.front() = entry;
    cells.back() = landing;
    // Straight stretches: from each point the farthest one a straight swim reaches.
    std::vector<BotTask::SwimPoint> way;
    std::size_t at = 0;
    while (at + 1 < cells.size())
    {
        std::size_t reach = at + 1;
        for (std::size_t j = cells.size() - 1; j > at + 1; --j)
            if (SwimsStraight(player, cells[at].x, cells[at].y, cells[j].x, cells[j].y))
            {
                reach = j;
                break;
            }
        BotTask::SwimPoint point = cells[reach];
        if (reach + 1 < cells.size())
            point.z = SwimSurface(player, point.x, point.y) - 1.0f;
        way.push_back(point);
        at = reach;
    }
    // A landing deep in the water (a target on the sea floor): over it at the surface first, then straight down. The last
    // stretch went from the surface to the target's depth in one slanting line, and where the floor rises between them
    // that line runs through the rock: Ainuha (16) swam 211 yd towards a Drysnap Crawler of "Clam Bait" off Shadowprey
    // Village and was 34 yd under the sea floor 26 s later, rescued; Frostvein's next swim there ended 11 yd under it,
    // 2026-11-26.
    if (!way.empty())
        if (float const above = SwimSurface(player, landing.x, landing.y); above > INVALID_HEIGHT && landing.z < above - 3.0f)
            way.insert(way.end() - 1, BotTask::SwimPoint{ landing.x, landing.y, above - 1.0f });
    float length = Dist2d(player, entry.x, entry.y);
    for (std::size_t i = 0; i < way.size(); ++i)
        length += std::hypot(way[i].x - (i ? way[i - 1].x : entry.x), way[i].y - (i ? way[i - 1].y : entry.y));
    task.swim.clear();
    if (walkFirst)
    {
        length += Dist2d(player, shore.x, shore.y) + std::hypot(entry.x - shore.x, entry.y - shore.y) - Dist2d(player, entry.x, entry.y);
        task.swim.push_back(shore);
    }
    task.swim.push_back(entry);
    task.swim.insert(task.swim.end(), way.begin(), way.end());
    task.swimNext = 0;
    task.swimLegSince = now;
    task.swimLegBest = task.swim.front().walk ? Dist2d(player, task.swim.front().x, task.swim.front().y) :
        player->GetExactDist(task.swim.front().x, task.swim.front().y, task.swim.front().z);
    task.swimMovedAt = 0;
    LOG_INFO(ActivityLog, "activity: {} swims to ({:.0f}, {:.0f}) for its target at ({:.0f}, {:.0f}): no way on the ground leads there; "
        "{:.0f} yd through the water over {} points, from ({:.0f}, {:.0f}){}", player->GetName(), landing.x, landing.y, x, y, length,
        task.swim.size(), entry.x, entry.y, walkFirst ? Acore::StringFormat(", where it walks first to the shore at ({:.0f}, {:.0f})",
        shore.x, shore.y) : std::string());
    return StepSwim(player, task, x, y, z);
}

bool OrderService::OwnStep(Player* player, BotTask& task, uint32 mapId, float x, float y, float z)
{
    constexpr float StepYards = 45.0f;       // how far one step leads along the path
    constexpr uint32 StepGapMs = 1500;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (mapId != player->GetMapId() || !player->IsAlive() || player->IsInCombat() || player->IsNonMeleeSpellCast(false))
        return true;
    // mod-playerbots' AI is held while the planner walks: its own movements (a grind target it goes for, the reach of
    // a creature beside the way) took each step over a moment after it began. A tauren's own steps towards the tigers
    // of "Tiger Mastery" came every 2 s and each got it 2 yd on, 19 steps for 70 yd, on j5, 2026-11-26. (A fight is not
    // held: the step above returns before this, and the hold runs out within a step's gap.)
    constexpr uint32 HoldMs = StepGapMs;
    // The step it is on, while it gets nearer to its end.
    if (task.ownStepAt && now - task.ownStepAt < StepGapMs)
    {
        BotDriver::PauseAI(player, HoldMs);
        return true;
    }
    // (By its spline, not Player::isMoving: a bot's movement flags can stay set after its movement ended, and its own
    // steps then waited 20 s each.)
    if (task.ownStepAt && !player->movespline->Finalized() &&
        player->GetExactDist(task.ownStepX, task.ownStepY, task.ownStepZ) > 4.0f && now - task.ownStepAt < 20 * IN_MILLISECONDS)
    {
        BotDriver::PauseAI(player, HoldMs);
        return true;
    }
    // (With the debug setting: how the step before ended, to tell what cut it short.)
    float const leftOfLast = task.ownStepAt ? player->GetExactDist(task.ownStepX, task.ownStepY, task.ownStepZ) : 0.0f;
    uint32 const motionBefore = uint32(player->GetMotionMaster()->GetCurrentMovementGeneratorType());
    bool const splineDone = player->movespline->Finalized();

    EnsureNavmesh(player, player->GetPositionX(), player->GetPositionY(), x, y);
    PathGenerator path(player);
    path.CalculatePath(x, y, z, false);
    Movement::PointsArray const& points = path.GetPath();
    auto fail = [&](char const* what, float length)
    {
        ++task.ownFailed;
        task.ownNote = Acore::StringFormat("{} from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f}): path type {:#x}, {} points, "
            "{:.0f} yd", what, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), x, y, z, uint32(path.GetPathType()),
            points.size(), length);
        return false;
    };
    // The path ends short of the target: a hand-listed crossing of a gap in the path finder's ground leads on, perhaps.
    if ((path.GetPathType() & (PATHFIND_INCOMPLETE | PATHFIND_NOPATH)) && BridgeGap(player, task, x, y, z))
        return true;
    // The core's path finder gives up after its limit of polygons, and in a city's dense navmesh a way of 80 yd is past it:
    // a paladin in Stormwind's Trade District on its way to the harbour had every step towards its route's next point
    // handed out as a straight line ("normal + not using a path"), refused, and it left its route and stood, while the
    // whole navmesh has a way of 84 yd there (2026-11-28, the owner's test). The whole navmesh's way then gives the step: a
    // point a step's length along it, which the core's path finder reaches.
    constexpr uint32 NoWholePath = PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH | PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE;
    if (((path.GetPathType() & NoWholePath) || points.size() < 2) && !player->GetTransport())
    {
        NavPoint step;
        float length = 0.0f;
        float const straight = player->GetExactDist(x, y, z);
        if (sDungeonNav.PointAlong(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), x, y, z,
                StepYards, step, &length, nullptr, player->GetMap()) && !step.drop && length <= straight * 3.0f + 60.0f &&
            player->GetExactDist(step.x, step.y, step.z) >= 3.0f)
        {
            PathGenerator toStep(player);
            toStep.CalculatePath(step.x, step.y, step.z, false);
            if (!(toStep.GetPathType() & NoWholePath) && toStep.GetPath().size() >= 2)
            {
                if (BotDriver::GetRpgState(player) != RpgState::Rest)
                    BotDriver::Rest(player);
                ++task.ownMade;
                task.ownStepAt = now;
                task.ownStepX = step.x;
                task.ownStepY = step.y;
                task.ownStepZ = step.z;
                if (GetSettings().debug)
                    LOG_INFO(ActivityLog, "activity: {} own step {} along the navmesh's way ({:.0f} yd) to ({:.0f}, {:.0f}, {:.0f}): the "
                        "path finder has no whole way to ({:.0f}, {:.0f}, {:.0f}) (path type {:#x})", player->GetName(), task.ownMade, length,
                        step.x, step.y, step.z, x, y, z, uint32(path.GetPathType()));
                player->GetMotionMaster()->MovePoint(0, step.x, step.y, step.z);
                BotDriver::PauseAI(player, HoldMs);
                return true;
            }
        }
    }
    if ((path.GetPathType() & PATHFIND_NOPATH) || points.size() < 2)
        return fail("no path", 0.0f);
    // A straight line the core hands out for want of a path is no step (a warlock's own steps "began" 55 times on the
    // Searing Gorge's scaffold towards a vendor 2017 yd away, each one the line through 677 yd of rock, and it stood).
    if (path.GetPathType() & (PATHFIND_NOT_USING_PATH | PATHFIND_SHORTCUT))
        return fail("only a straight line", 0.0f);
    // The point about a step's length along the path.
    std::size_t target = 1;
    float along = 0.0f;
    for (; target < points.size(); ++target)
    {
        along += (points[target] - points[target - 1]).length();
        if (along >= StepYards)
            break;
    }
    target = std::min(target, points.size() - 1);
    // A partial path that ends where the bot stands leads nowhere.
    if (along < 3.0f)
        return fail("the path ends where it stands", along);

    // NewRpg would pull the bot back to the point it holds for the nearest: it rests while the planner walks.
    if (BotDriver::GetRpgState(player) != RpgState::Rest)
        BotDriver::Rest(player);
    ++task.ownMade;
    task.ownStepAt = now;
    task.ownStepX = points[target].x;
    task.ownStepY = points[target].y;
    task.ownStepZ = points[target].z;
    // (With the debug setting: each step, its goal and the path it was taken from, to tell a walk that goes to and fro
    // from one that gets on.)
    if (GetSettings().debug)
        LOG_INFO(ActivityLog, "activity: {} own step {} from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f}), {:.0f} yd along a path "
            "of type {:#x} with {} points towards ({:.0f}, {:.0f}, {:.0f}) ({:.0f} yd straight); the step before: {:.0f} yd short of its "
            "end, movement {}, spline {}", player->GetName(), task.ownMade, player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ(), points[target].x, points[target].y, points[target].z, along, uint32(path.GetPathType()), points.size(),
            x, y, z, player->GetExactDist2d(x, y), leftOfLast, motionBefore, splineDone ? "ended" : "running");
    player->GetMotionMaster()->MovePoint(0, points[target].x, points[target].y, points[target].z);
    BotDriver::PauseAI(player, HoldMs);
    return true;
}

bool OrderService::WadeOut(Player* player, BotTask& task, float x, float y, float z, char const* why)
{
    // The floor of the Undercity's canals lies 2 yd under the water and is a part of the navmesh of its own (101
    // polygons from the War Quarter to the east wall); the water's surface over it joins the city by a ramp out of the
    // canal at (1747, 375). The core keeps a swimmer 1.6 yd under the surface, half a yard over that floor, and every
    // path starts on the floor: none leads out. A level-15 shaman walked into the canal on its way to Brill, its routes,
    // voyages and flights all ended "no way on the ground" there for an hour, round 226. A player swims along the
    // surface to the ramp and walks up: the bot follows the path from the surface, as far as the shore and a few yards
    // on, in one movement (the points are walked as they are; a new path would start on the floor again).
    constexpr float MinDepth = 1.0f;        // water this deep over the floor (a wade through a stream needs nothing)
    constexpr float BeyondShoreYards = 12.0f;
    constexpr float MaxSwimYards = 300.0f;
    constexpr int NodesAsked = 6;
    // (Wading only: under water the path finder has straight lines and no whole path anyway, and a diver works its
    // target down there or comes up for air by its own rules. A warrior fighting naga 24 yd under Desolace's coast was
    // sent "out along the surface" to the shore twice on its way to the next naga, 2026-11-25.)
    if (task.wadeLooked || player->GetTransport() || player->IsInCombat() || player->IsInFlight() || !player->IsAlive() ||
        !IsSwimming(player) || IsDiving(player))
        return false;
    Map* map = player->GetMap();
    float const px = player->GetPositionX(), py = player->GetPositionY(), pz = player->GetPositionZ();
    // (Asked half a yard over its feet: at the floor's own height the core gave no level at all.)
    LiquidData const liquid = map->GetLiquidData(player->GetPhaseMask(), px, py, pz + 0.5f, player->GetCollisionHeight(), {});
    if (liquid.Level <= INVALID_HEIGHT || liquid.Level - pz < MinDepth)
        return false;
    task.wadeLooked = true;
    auto inWater = [&](G3D::Vector3 const& p)
    {
        return map->IsInWater(player->GetPhaseMask(), p.x, p.y, WaterProbeZ(map, p.x, p.y, p.z), player->GetCollisionHeight());
    };
    // A walk to a place in this water (a diver's objective) stays in it.
    constexpr float WaterGoalYards = 400.0f;
    float const goalYards = Dist2d(px, py, x, y);
    if (goalYards > 5.0f && goalYards <= WaterGoalYards && inWater(G3D::Vector3(x, y, z)))
        return false;
    // A point of the path on dry ground: ground right under it, and no water over that ground. (Not "a point out of
    // the water": at the canal's edge the core puts a point 3 yd over the water, between its floor and the walkway.)
    auto ashore = [&](G3D::Vector3 const& p)
    {
        float const ground = map->GetHeight(player->GetPhaseMask(), p.x, p.y, p.z + 1.5f, true, 4.0f);
        if (ground <= INVALID_HEIGHT || std::fabs(ground - p.z) > 1.5f)
            return false;
        LiquidData const there = map->GetLiquidData(player->GetPhaseMask(), p.x, p.y, ground + 0.3f, player->GetCollisionHeight(), {});
        return there.Level <= INVALID_HEIGHT || there.Level < ground + 0.5f;
    };
    // Where a path out of the water reaches the shore: the index of its first point on dry ground from which it stays
    // dry for DryYards (or to its end), or none within MaxSwimYards. (A step in the canal's bend is dry ground for 3 yd,
    // and the path goes on in the water.)
    constexpr float DryYards = 10.0f;
    auto shoreOf = [&](Movement::PointsArray const& points, float& swum)
    {
        swum = 0.0f;
        for (std::size_t i = 1; i < points.size(); ++i)
        {
            swum += (points[i] - points[i - 1]).length();
            if (swum > MaxSwimYards)
                return std::size_t(0);
            if (!ashore(points[i]))
                continue;
            float dry = 0.0f;
            std::size_t j = i + 1;
            for (; j < points.size() && dry < DryYards; ++j)
            {
                if (!ashore(points[j]))
                    break;
                dry += (points[j] - points[j - 1]).length();
            }
            if (dry >= DryYards || j >= points.size())
                return i;
        }
        return std::size_t(0);
    };
    // The goals: the walk's own destination, and the nearest nodes of the road map (a far destination's tile may not
    // be loaded: the path finder then has only a straight line).
    std::vector<G3D::Vector3> goals{ G3D::Vector3(x, y, z) };
    float minDistance = 20.0f;
    for (int i = 0; i < NodesAsked; ++i)
    {
        RoutePoint node;
        std::string name;
        if (!sRoadMap.FindNearestNode(player->GetMapId(), px, py, minDistance, MaxSwimYards * 2.0f, node, name))
            break;
        minDistance = Dist2d(px, py, node.x, node.y) + 10.0f;
        goals.emplace_back(node.x, node.y, node.z);
    }
    // (The path is asked from a hair over the surface: from under it Detour takes the floor's polygon below, which the
    // point stands over within climbing height, before the water's above.)
    float const surfaceZ = liquid.Level + 0.3f;
    // First the floor's own paths: one whole path to a goal on dry ground and the floor is no pocket, the walk lacked a
    // whole path for another reason (its destination). Nothing to do and nothing to say: in round 230 a wading bot
    // wrote "none from the surface leads out" about every five minutes in ordinary lakes, its floor reaching 3 or 4 of
    // the road nodes asked. (A goal in the water, a node on the canal's floor itself, says nothing about the way out.)
    std::vector<bool> reached(goals.size(), false);
    uint32 fromFloor = 0;
    for (std::size_t i = 0; i < goals.size(); ++i)
    {
        PathGenerator own(player);
        own.CalculatePath(goals[i].x, goals[i].y, goals[i].z, false);
        if (own.GetPathType() != PATHFIND_NORMAL)
            continue;
        reached[i] = true;
        ++fromFloor;
        if (!inWater(goals[i]))
            return false;
    }
    for (std::size_t i = 0; i < goals.size(); ++i)
    {
        // One the floor has no whole path to and the surface has is the floor's pocket.
        if (reached[i])
            continue;
        G3D::Vector3 const& goal = goals[i];
        float swum = 0.0f;
        PathGenerator surface(player);
        if (!surface.CalculatePath(px, py, surfaceZ, goal.x, goal.y, goal.z, false) || surface.GetPathType() != PATHFIND_NORMAL)
            continue;
        Movement::PointsArray const& points = surface.GetPath();
        std::size_t const shore = shoreOf(points, swum);
        if (!shore)
            continue;
        // On to a few yards beyond the shore, along the path.
        Movement::PointsArray way(points.begin(), points.begin() + shore + 1);
        float beyond = 0.0f;
        for (std::size_t i = shore + 1; i < points.size() && beyond < BeyondShoreYards; ++i)
        {
            beyond += (points[i] - points[i - 1]).length();
            way.push_back(points[i]);
        }
        if (way.size() < 2)
            continue;
        if (BotDriver::GetRpgState(player) != RpgState::Rest)
            BotDriver::Rest(player);
        player->GetMotionMaster()->Clear();
        player->GetMotionMaster()->MoveSplinePath(&way, FORCED_MOVEMENT_RUN);
        float const yards = swum + beyond;
        uint32 const now = std::max<uint32>(getMSTime(), 1);
        task.wadeUntil = now + uint32(yards / 3.0f * float(IN_MILLISECONDS)) + 10 * IN_MILLISECONDS;
        task.walkX = task.walkY = 0.0f; // the walk is set again from the shore
        BotDriver::PauseAI(player, 3 * IN_MILLISECONDS);
        LOG_INFO(ActivityLog, "activity: {} stands on a floor {:.1f} yd under the water at ({:.0f}, {:.0f}, {:.0f}) map {} that leads "
            "nowhere ({}): it swims out along the surface, {:.0f} yd to the shore at ({:.0f}, {:.0f}, {:.0f})", player->GetName(),
            liquid.Level - pz, px, py, pz, player->GetMapId(), why, swum, points[shore].x, points[shore].y, points[shore].z);
        return true;
    }
    LOG_INFO(ActivityLog, "activity: {} stands {:.1f} yd under the water at ({:.0f}, {:.0f}, {:.0f}) map {} with no whole path from there "
        "({}), and none from the surface leads out within {:.0f} yd ({} goals asked, {} of them reached from where it stands)",
        player->GetName(), liquid.Level - pz, px, py, pz, player->GetMapId(), why, MaxSwimYards, uint32(goals.size()), fromFloor);
    return false;
}

bool OrderService::LiftLanding(Player* player, BotTask& task, float stopX, float stopY, float stopZ, bool quiet)
{
    // A lift's stop is the platform's own place: over its empty shaft while the platform is at the other stop. The walk
    // to the upper stop of Freewind Post's lift went to the shaft's floor 130 yd below (its only ground there), the
    // ground guard refused it, and the guild leader stood on the mesa 15 yd from the lift (2026-11-25). The bot walks to
    // the landing beside the stop instead: ground at the stop's height (the map and the world's models, not the
    // platform) 4 to 14 yd from it, the nearest with a way from the bot.
    if (task.liftLandLooked && std::fabs(task.liftLandForX - stopX) + std::fabs(task.liftLandForY - stopY) +
        std::fabs(task.liftLandForZ - stopZ) < 1.0f)
        return task.liftLandOk;
    task.liftLandOk = false;
    task.liftLandLooked = true;
    task.liftLandForX = stopX;
    task.liftLandForY = stopY;
    task.liftLandForZ = stopZ;
    Map* map = player->GetMap();
    if (!map)
        return false;
    struct Candidate
    {
        float x, y, z, score;
    };
    std::vector<Candidate> candidates;
    constexpr int Directions = 16;
    for (float ring = 4.0f; ring <= 14.0f; ring += 2.0f)
        for (int i = 0; i < Directions; ++i)
        {
            float const angle = float(i) * 2.0f * float(M_PI) / float(Directions);
            float const x = stopX + ring * std::cos(angle);
            float const y = stopY + ring * std::sin(angle);
            float const ground = map->GetHeight(x, y, stopZ + 3.0f, true, 6.0f);
            if (ground <= INVALID_HEIGHT || std::fabs(ground - stopZ) > 4.0f)
                continue;
            candidates.push_back({ x, y, ground, ring + 0.05f * player->GetExactDist2d(x, y) });
        }
    std::sort(candidates.begin(), candidates.end(), [](Candidate const& a, Candidate const& b) { return a.score < b.score; });
    constexpr std::size_t MaxPaths = 8;
    for (std::size_t i = 0; i < candidates.size() && i < MaxPaths; ++i)
    {
        Candidate const& c = candidates[i];
        if (!PathEndsAt(player, c.x, c.y, c.z))
            continue;
        task.liftLandOk = true;
        task.liftLandX = c.x;
        task.liftLandY = c.y;
        task.liftLandZ = c.z;
        if (!quiet)
            LOG_INFO(ActivityLog, "activity: {} walks to the landing at ({:.0f}, {:.0f}, {:.0f}) beside the lift's stop at ({:.0f}, {:.0f}, "
            "{:.0f}), {:.0f} yd from it", player->GetName(), c.x, c.y, c.z, stopX, stopY, stopZ, std::hypot(c.x - stopX, c.y - stopY));
        return true;
    }
    return false;
}

bool OrderService::StepStrayLiftPassenger(Player* player, BotRuntime& rt, Transport* platform)
{
    // The planner waits out anything a bot rides, and a passenger of a lift without a ride of its own was waited out for
    // good: Mojowind entered the world on the Mesa Elevator of Freewind Post after a restart during its ride and stood
    // there from 18:28 to past 19:22, "task none", moved 0 yd; Ashmire stood on it half an hour after it had followed its
    // leader onto it (2026-11-25). It gets off at the platform's next stop, onto the landing beside it.
    constexpr float NearYards = 12.0f;         // farther from its platform: it walked off it without leaving it
    constexpr float StillYards = 0.1f;         // the platform stands at a stop while its height changes less than this ..
    constexpr uint32 StillMs = 1500;           // .. for this long
    constexpr uint32 GiveUpMs = 2 * MINUTE * IN_MILLISECONDS;
    uint32 const now = getMSTime();
    float const x = player->GetPositionX(), y = player->GetPositionY(), z = player->GetPositionZ();
    if (!rt.strayLiftSince)
    {
        rt.strayLiftSince = now;
        rt.strayLiftStillSince = now;
        rt.strayLiftZ = platform->GetPositionZ();
        LOG_INFO(ActivityLog, "activity: {} is a passenger of the lift {} without a ride of its own at ({:.0f}, {:.0f}, {:.0f}) (task {}): "
            "it gets off at the platform's next stop", player->GetName(), platform->GetName(), x, y, z, ToString(rt.task.kind));
    }
    auto done = [&]()
    {
        rt.strayLiftSince = 0;
        rt.hasLastPosition = false;
        rt.strandSince = 0;
        return true;
    };

    // Away from its platform (it walked off it without leaving it: every ride carries it up and down through the rock,
    // Thunder Bluff 2026-11-24), or no stop in two minutes: off the lift where it stands, on the ground under it or above
    // it when the rides took it into the rock.
    if (player->GetExactDist2d(platform) > NearYards || now - rt.strayLiftSince > GiveUpMs)
    {
        BotDriver::LeaveTransport(player);
        float ground = player->GetMap()->GetHeight(player->GetPhaseMask(), x, y, z + 2.0f, true, 30.0f);
        if (ground <= INVALID_HEIGHT || z - ground > 30.0f)
            ground = player->GetMap()->GetHeight(player->GetPhaseMask(), x, y, z + 80.0f, true, 100.0f);
        LOG_INFO(ActivityLog, "activity: {} is a passenger of the lift {} {:.0f} yd away from it at ({:.0f}, {:.0f}, {:.0f}) (task {}, "
            "lift step {}): it is taken off the lift{}", player->GetName(), platform->GetName(), player->GetExactDist2d(platform), x, y, z,
            ToString(rt.task.kind), uint32(rt.task.liftAt), ground > INVALID_HEIGHT && std::fabs(ground - z) > 1.0f ?
            Acore::StringFormat(" and stands on the ground at height {:.0f}", ground) : std::string());
        if (ground > INVALID_HEIGHT && std::fabs(ground - z) > 1.0f)
            player->NearTeleportTo(x, y, ground + 0.5f, player->GetOrientation());
        return done();
    }

    if (std::fabs(platform->GetPositionZ() - rt.strayLiftZ) > StillYards)
    {
        rt.strayLiftZ = platform->GetPositionZ();
        rt.strayLiftStillSince = now;
    }
    BotDriver::PauseAI(player, 2 * IN_MILLISECONDS);
    rt.why = Acore::StringFormat("Riding the lift {}", platform->GetName());
    if (now - rt.strayLiftStillSince < StillMs || player->IsBeingTeleported())
        return true;

    // At a stop: onto the landing beside it, at the height the platform carries it at (its own position is not its
    // floor: the lifts of Thunder Bluff have it 8 yd above), else at the platform's (Ashmire was carried 24 yd under the
    // lower stop of the Mesa Elevator, its place on the platform taken where it boarded).
    BotTask look;
    bool landing = LiftLanding(player, look, platform->GetPositionX(), platform->GetPositionY(), z, true);
    if (!landing && std::fabs(z - platform->GetPositionZ()) > 4.0f)
        landing = LiftLanding(player, look, platform->GetPositionX(), platform->GetPositionY(), platform->GetPositionZ(), true);
    float lx = landing ? look.liftLandX : x, ly = landing ? look.liftLandY : y, lz = landing ? look.liftLandZ : z;
    if (!landing)
        SnapToNavmesh(player, player->GetMapId(), lx, ly, lz);
    BotDriver::LeaveTransport(player);
    BotDriver::Hop(player, lx, ly, lz + 0.5f);
    LOG_INFO(ActivityLog, "activity: {} gets off the lift {} at its stop at height {:.0f} after {} s: onto {} at ({:.0f}, {:.0f}, {:.0f})",
        player->GetName(), platform->GetName(), platform->GetPositionZ(), (now - rt.strayLiftSince) / IN_MILLISECONDS,
        landing ? "the landing beside it" : "the walkable ground nearest to it", lx, ly, lz);
    return done();
}

bool OrderService::StepLift(Player* player, BotRuntime& rt)
{
    BotTask& task = rt.task;
    if (!task.liftAt)
        return false;
    // A lift comes round within a minute or two; a ride takes less than one.
    constexpr uint32 LiftWaitMs = 150 * IN_MILLISECONDS;
    constexpr uint32 LiftRideMs = 90 * IN_MILLISECONDS;
    // The platform stands at a stop while it is this close to the stop's height (the stops are its own positions there).
    constexpr float StopHeight = 1.0f;
    constexpr float PlatformYards = 15.0f;
    uint32 const now = getMSTime();
    auto giveUp = [&](std::string const& why)
    {
        LOG_INFO(ActivityLog, "activity: {} gives up its ride on the lift at ({:.0f}, {:.0f}, {:.0f}): {}; the rest of the walk "
            "without the road map", player->GetName(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), why);
        if (player->GetTransport())
            BotDriver::LeaveTransport(player);
        task.liftAt = 0;
        task.liftRiding = false;
        task.liftGuid.Clear();
        task.route.found = false;
        task.routeBlocked = true;
        return false;
    };
    if (task.liftAt >= task.route.points.size() || player->GetMapId() != task.route.mapId)
        return giveUp("its route changed");
    RoutePoint const& from = task.route.points[task.liftAt - 1];

    // The platform: a static transport of the map (they are not stored in the grid: a grid search does not find them).
    Transport* platform = nullptr;
    float best = PlatformYards;
    for (Transport* one : player->GetMap()->GetAllTransports())
    {
        if (!one || !one->IsInWorld() || one->GetEntry() != task.liftEntry)
            continue;
        if (task.liftGuid ? one->GetGUID() == task.liftGuid :
            Dist2d(one->GetPositionX(), one->GetPositionY(), from.x, from.y) < best)
        {
            best = Dist2d(one->GetPositionX(), one->GetPositionY(), from.x, from.y);
            platform = one;
            if (task.liftGuid)
                break;
        }
    }
    if (!platform)
        return giveUp("no platform of it in sight");
    task.liftGuid = platform->GetGUID();
    GameObject* lift = platform;

    // The AI does nothing while the bot waits at the stop or rides (no walk off the platform).
    BotDriver::PauseAI(player, 2 * IN_MILLISECONDS);
    if (player->GetTransport() == platform)
    {
        if (!task.liftRiding)
        {
            task.liftRiding = true;
            task.liftSince = now;
        }
        if (std::fabs(lift->GetPositionZ() - task.liftToZ) <= StopHeight)
        {
            // At the other stop: off the platform onto the landing, a step along the route beyond the stop (the platform
            // hangs beside the landing, off the walkable ground: from Thunder Bluff's lift the next walk "started far
            // from the navmesh"), at the height the platform carried it to.
            float x = player->GetPositionX(), y = player->GetPositionY(), z = player->GetPositionZ();
            constexpr float LandingMin = 5.0f, LandingMax = 20.0f;
            std::vector<RoutePoint> const& points = task.route.points;
            for (std::size_t i = task.liftAt + 1; i < points.size(); ++i)
            {
                float const off = Dist2d(points[i].x, points[i].y, task.liftToX, task.liftToY);
                if (off > LandingMax)
                    break;
                if (off < LandingMin)
                    continue;
                float const ground = player->GetMap()->GetHeight(player->GetPhaseMask(), points[i].x, points[i].y,
                    std::max(points[i].z, z) + 3.0f, true, 12.0f);
                if (ground > INVALID_HEIGHT)
                {
                    x = points[i].x;
                    y = points[i].y;
                    z = ground;
                }
                break;
            }
            SnapToNavmesh(player, player->GetMapId(), x, y, z);
            BotDriver::LeaveTransport(player);
            BotDriver::Hop(player, x, y, z + 0.5f);
            LOG_INFO(ActivityLog, "activity: {} rode the lift {} from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f}) in {} s",
                player->GetName(), lift->GetName(), from.x, from.y, from.z, x, y, z, (now - task.liftSince) / IN_MILLISECONDS);
            task.routeIndex = task.liftAt;
            task.routeTarget = -1;
            task.lastAssert = 0;
            task.walkX = task.walkY = 0.0f;
            task.liftAt = 0;
            task.liftRiding = false;
            task.liftGuid.Clear();
            return false;
        }
        if (now - task.liftSince > LiftRideMs)
            return giveUp("the ride does not end");
        rt.why = Acore::StringFormat("Riding the lift {}", lift->GetName());
        return true;
    }
    if (task.liftRiding)
        return giveUp("it came off the platform during the ride");

    if (!task.liftSince)
        task.liftSince = now;
    if (now - task.liftSince > LiftWaitMs)
        return giveUp(Acore::StringFormat("the platform did not come in {} s", LiftWaitMs / IN_MILLISECONDS));
    rt.why = Acore::StringFormat("Waiting for the lift {}", lift->GetName());
    // The platform stands at this stop: onto it (a hop, as onto a ship's deck), and once the bot stands there it becomes
    // its passenger (it carries the bot; StaticTransport moves its passengers). The teleport of the hop would take a
    // passenger off its transport, so the two are a tick apart.
    if (std::fabs(lift->GetPositionZ() - from.z) <= StopHeight &&
        Dist2d(lift->GetPositionX(), lift->GetPositionY(), from.x, from.y) <= StopHeight + 2.0f && !player->IsBeingTeleported())
    {
        // (The platform's position is not its floor: the lifts of Thunder Bluff have it 8 yd above. The floor is where
        // the bot stands at the stop, the platform's or the ground under it.)
        if (Dist2d(player, lift->GetPositionX(), lift->GetPositionY()) <= 2.0f)
        {
            platform->AddPassenger(player, true);
            task.liftRiding = true;
            task.liftSince = now;
            LOG_INFO(ActivityLog, "activity: {} steps onto the lift {} at ({:.0f}, {:.0f}, {:.0f}) for the stop at height {:.0f}",
                player->GetName(), lift->GetName(), from.x, from.y, from.z, task.liftToZ);
        }
        else
        {
            float floor = player->GetMap()->GetHeight(player->GetPhaseMask(), lift->GetPositionX(), lift->GetPositionY(),
                player->GetPositionZ() + 3.0f, true, 12.0f);
            if (floor <= INVALID_HEIGHT)
                floor = player->GetPositionZ();
            BotDriver::Hop(player, lift->GetPositionX(), lift->GetPositionY(), floor + 0.3f);
        }
        return true;
    }
    // Not at the stop (a fight pushed it away): back to it, to the landing beside it when it has one (the stop itself is
    // over the shaft while the platform is away).
    bool const landing = LiftLanding(player, task, from.x, from.y, from.z);
    float const waitX = landing ? task.liftLandX : from.x;
    float const waitY = landing ? task.liftLandY : from.y;
    float const waitZ = landing ? task.liftLandZ : player->GetPositionZ();
    // Over the shaft with the platform gone: the hop onto it came as it left (the window above is a yard of its way), and
    // the bot stood in the air at the stop until a rescue (Bullhide at the upper stop of Thunder Bluff's Mesa Elevator,
    // 32 yd above the platform, 2026-11-27). Back onto the landing, where it waits for the next one.
    if (landing && !player->GetTransport() && Dist2d(player, from.x, from.y) <= 3.0f && Dist2d(player, waitX, waitY) > 1.5f)
    {
        constexpr float FloorYards = 3.0f;
        float const below = player->GetMap()->GetHeight(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ() + 1.0f, true, FloorYards + 1.0f);
        if (below <= INVALID_HEIGHT || player->GetPositionZ() - below > FloorYards)
        {
            LOG_INFO(ActivityLog, "activity: {} stands over the shaft of the lift {} at ({:.0f}, {:.0f}, {:.0f}) with the platform "
                "gone: back onto the landing at ({:.0f}, {:.0f}, {:.0f})", player->GetName(), lift->GetName(), player->GetPositionX(),
                player->GetPositionY(), player->GetPositionZ(), waitX, waitY, waitZ);
            BotDriver::Hop(player, waitX, waitY, waitZ + 0.3f);
            return true;
        }
    }
    if (Dist2d(player, waitX, waitY) > 3.0f && Dist2d(player, from.x, from.y) > 3.0f && now - task.lastAssert >= AssertGapMs)
    {
        BotDriver::GoTo(player, player->GetMapId(), waitX, waitY, waitZ, false);
        task.lastAssert = now;
    }
    else if (player->isMoving() && (Dist2d(player, waitX, waitY) <= 3.0f || Dist2d(player, from.x, from.y) <= 3.0f))
        player->StopMoving();
    return true;
}

bool OrderService::ViaDungeonPath(Player* player, BotRuntime& rt, float& x, float& y, float& z)
{
    // (Shadowfang Keep, 2026-11-24: the way from the entrance to Odo the Blindwatcher, 23 yd above it, is 620 yd through
    // the courtyard and the stables; the core's path ended below Odo, and the group was put on his floor by a teleport.
    // the owner: [quote left out].)
    constexpr float AheadYards = 35.0f;    // the next point along the way
    constexpr float NearYards = 12.0f;     // a destination this near is walked to straight
    constexpr float NextYards = 10.0f;     // the next point is taken when the bot is this near the last one
    constexpr uint32 RefreshMs = 5 * IN_MILLISECONDS;
    BotTask& task = rt.task;
    Map* map = player->GetMap();
    if (!map || (!map->IsDungeon() && !task.openWay) || player->GetTransport() || player->IsInFlight())
    {
        task.navActive = false;
        return false;
    }
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    // A point a few yards off that nothing brings the bot to (the rule is below, at the way's next point): the same for
    // a destination this near. The run's stretches give the top of a drop 4.7 yd away as the destination, which NewRpg
    // walked to from where the leader floated at the pool's edge without a yard for eight minutes (Blackfathom Deeps,
    // Ghamoo-ra, 2026-11-25, after the rule for the way's next point was in).
    if (player->GetExactDist(x, y, z) <= NearYards)
    {
        task.navActive = false;
        if (map->IsDungeon() && !player->IsInCombat() && !rt.rest.active && std::fabs(player->GetPositionZ() - z) <= 8.0f && player->GetExactDist(x, y, z) > 2.0f)
        {
            if (!task.navStillSince || Dist2d(player, task.navStillX, task.navStillY) > 1.0f)
            {
                task.navStillSince = now;
                task.navStillX = player->GetPositionX();
                task.navStillY = player->GetPositionY();
            }
            else if (getMSTimeDiff(task.navStillSince, now) >= 6 * IN_MILLISECONDS)
            {
                LOG_INFO(ActivityLog, "activity: {} does not get to ({:.0f}, {:.0f}, {:.0f}) in {}, {:.0f} yd away, in {} s: it goes there straight",
                    player->GetName(), x, y, z, map->GetMapName(), player->GetExactDist(x, y, z), getMSTimeDiff(task.navStillSince, now) / IN_MILLISECONDS);
                task.navStillSince = now;
                BotDriver::Rest(player);
                BotDriver::PauseAI(player, 3 * IN_MILLISECONDS);
                if (player->IsNonMeleeSpellCast(false))
                    player->InterruptNonMeleeSpells(false);
                player->StopMoving();
                player->GetMotionMaster()->Clear();
                player->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false);
                return true;
            }
        }
        else
            task.navStillSince = 0;
        return false;
    }
    // A drop or swim under way: on to its landing, nothing planned anew meanwhile (DropDown).
    if (rt.dropUntil && now < rt.dropUntil)
    {
        if (player->GetExactDist(rt.dropX, rt.dropY, rt.dropZ) > 2.0f)
        {
            KeepDropMoving(player, rt);
            return true;
        }
        rt.dropUntil = 0;
    }
    else if (rt.dropUntil)
        DropDone(player, rt);
    // A walk along the navmesh's surface under way (below): nothing else moves it meanwhile.
    if (task.navSplineUntil)
    {
        if (now < task.navSplineUntil && !player->movespline->Finalized() && !player->IsInCombat() && player->IsAlive())
        {
            BotDriver::PauseAI(player, 2 * IN_MILLISECONDS);
            return true;
        }
        task.navSplineUntil = 0;
    }
    bool const sameGoal = task.navActive && std::fabs(task.navToX - x) + std::fabs(task.navToY - y) + std::fabs(task.navToZ - z) < 3.0f;
    if (!sameGoal || getMSTimeDiff(task.navAt, now) >= RefreshMs ||
        (!task.navDrop && player->GetExactDist(task.navX, task.navY, task.navZ) <= NextYards))
    {
        NavPoint point, top;
        float length = 0.0f;
        float fromX, fromY, fromZ;
        bool const fromLanding = NavFrom(player, rt, fromX, fromY, fromZ);
        if (!sDungeonNav.PointAlong(player->GetMapId(), fromX, fromY, fromZ, x, y, z, AheadYards, point, &length, &top, map))
        {
            task.navActive = false;
            return false;
        }
        if (fromLanding && !point.drop && player->GetPositionZ() - fromZ > 2.5f && !player->IsInCombat())
        {
            SwimAlong(player, rt, point.x, point.y, point.z);
            task.navActive = false;
            return true;
        }
        // (A line when the way winds: it is not the straight line the walk would have taken.)
        if ((!task.navActive || !sameGoal) && length > 1.3f * player->GetExactDist(x, y, z) + 20.0f)
            LOG_INFO(ActivityLog, "activity: {} walks the navmesh's way to ({:.0f}, {:.0f}, {:.0f}) in {}: {:.0f} yd ({:.0f} yd straight)",
                player->GetName(), x, y, z, map->GetMapName(), length, player->GetExactDist(x, y, z));
        task.navActive = true;
        task.navAt = now;
        task.navLeft = length;
        task.navDrop = point.drop;
        task.navSwim = point.swim;
        task.navX = point.drop ? top.x : point.x;
        task.navY = point.drop ? top.y : point.y;
        task.navZ = point.drop ? top.z : point.z;
        task.navLandX = point.x;
        task.navLandY = point.y;
        task.navLandZ = point.z;
        task.navToX = x;
        task.navToY = y;
        task.navToZ = z;
    }
    // The core's path finder has no way to the next point where the dungeon's navmesh has one: the owner's group stood twelve
    // minutes at the top of the ramp down to Highlord Omokk in Lower Blackrock Spire, every walk refused as "shortcut + no
    // path" (no polygon found for its start or end by the core's query of that instance), while the navmesh had a way of
    // 91 yd and a fresh instance's path finder found it, 2026-11-25. After a refused walk the bot walks the straight leg
    // to the next point along the navmesh's surface itself (a spline over points on it, no path finder).
    constexpr float SurfaceStepYards = 6.0f;
    constexpr uint32 SurfaceRefusalWindowMs = 30 * IN_MILLISECONDS;
    // ... and also when the bot gets no nearer to the next point for a while, refused or not: where the core's path finder
    // has no way, mod-playerbots' long walk (NewRpg's MoveFarTo) samples points in a cone towards the goal and then walks to
    // a random point near by, again and again. The owner's group in Scholomance, 2026-11-25 (23:53 to 00:02, [quote left out]): its tank walked 1611 yd in five minutes with 19 turns back and came 35 yd on,
    // then 1770 yd with 34, its walk to its place in Jandice Barov's room refused once and then handed to that walk, until
    // the room's ten minutes ran out.
    constexpr uint32 SurfaceStallMs = 8 * IN_MILLISECONDS;
    constexpr float SurfaceStallGain = 3.0f;
    bool stalled = false;
    if (task.navActive && !task.navDrop)
    {
        float const toPoint = player->GetExactDist(task.navX, task.navY, task.navZ);
        bool const samePoint = std::fabs(task.navBestForX - task.navX) + std::fabs(task.navBestForY - task.navY) < 1.0f;
        if (!samePoint || !task.navBestAt || toPoint + SurfaceStallGain < task.navBestDist || player->IsInCombat() || rt.rest.active)
        {
            task.navBestForX = task.navX;
            task.navBestForY = task.navY;
            task.navBestDist = toPoint;
            task.navBestAt = now;
        }
        else
            stalled = getMSTimeDiff(task.navBestAt, now) >= SurfaceStallMs;
    }
    if (task.navActive && !task.navDrop && !player->IsInCombat() && !rt.rest.active &&
        player->GetExactDist(task.navX, task.navY, task.navZ) > NearYards &&
        (stalled || ((std::fabs(task.navSplineX - task.navX) + std::fabs(task.navSplineY - task.navY) > 1.0f) &&
            (sGroundGuard.GetRefusals(player->GetGUID(), SurfaceRefusalWindowMs, false).count > 0 ||
             (task.navRefusedAt && getMSTimeDiff(task.navRefusedAt, now) <= SurfaceRefusalWindowMs)))))
    {
        std::vector<NavPoint> surface;
        auto reachesPoint = [&]() { return surface.size() >= 2 && Dist2d(surface.back().x, surface.back().y, task.navX, task.navY) <= 3.0f; };
        sDungeonNav.SurfaceLine(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), task.navX,
            task.navY, task.navZ, SurfaceStepYards, surface);
        // The straight line to the next point clips a corner (a door frame, the edge of a stair): it stops short there. Then
        // the navmesh's own corners to that point, the surface walked leg by leg (Palatwo stood an hour at the top of the
        // East Garrison's stair in Blackrock Depths, (597, -64, -52), its next point 77 yd on: the straight line from there
        // clipped the stair's side 4 yd out, the way bends round it; from 6 yd beside it the same line had gone through,
        // 2026-11-26).
        if (!reachesPoint())
        {
            std::vector<NavPoint> corners;
            float walked = 0.0f;
            surface.clear();
            if (sDungeonNav.FindWay(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), task.navX,
                    task.navY, task.navZ, &corners, &walked, map) && corners.size() >= 2)
            {
                for (std::size_t i = 1; i < corners.size(); ++i)
                {
                    std::vector<NavPoint> leg;
                    if (!sDungeonNav.SurfaceLine(player->GetMapId(), corners[i - 1].x, corners[i - 1].y, corners[i - 1].z, corners[i].x,
                            corners[i].y, corners[i].z, SurfaceStepYards, leg) ||
                        Dist2d(leg.back().x, leg.back().y, corners[i].x, corners[i].y) > 1.5f)
                    {
                        surface.clear();
                        break;
                    }
                    surface.insert(surface.end(), leg.begin(), leg.end());
                }
            }
        }
        if (reachesPoint())
        {
            Movement::PointsArray way;
            way.emplace_back(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
            for (NavPoint const& p : surface)
                way.emplace_back(p.x, p.y, p.z);
            float yards = 0.0f;
            for (std::size_t i = 1; i < way.size(); ++i)
                yards += (way[i] - way[i - 1]).length();
            LOG_INFO(ActivityLog, "activity: {} walks the navmesh's surface to ({:.0f}, {:.0f}, {:.0f}) in {}, {:.0f} yd: {}", player->GetName(),
                task.navX, task.navY, task.navZ, map->GetMapName(), yards, stalled ? Acore::StringFormat("it got no nearer in {} s",
                getMSTimeDiff(task.navBestAt, now) / IN_MILLISECONDS) : std::string("the path finder had no way there"));
            task.navSplineX = task.navX;
            task.navSplineY = task.navY;
            task.navBestAt = now;
            task.navSplineUntil = std::max<uint32>(now + uint32(yards / 3.0f * float(IN_MILLISECONDS)) + 5 * IN_MILLISECONDS, 1);
            BotDriver::Rest(player);
            BotDriver::PauseAI(player, 2 * IN_MILLISECONDS);
            if (player->IsNonMeleeSpellCast(false))
                player->InterruptNonMeleeSpells(false);
            player->StopMoving();
            player->GetMotionMaster()->Clear();
            player->GetMotionMaster()->MoveSplinePath(&way, FORCED_MOVEMENT_RUN);
            return true;
        }
    }
    // A next point a few yards off that nothing brings the bot to: NewRpg's walk from where it floats or stands beside
    // the navmesh has no path, and the stall rule of WalkTo waits only for points 8 yd and more away. Blackfathom Deeps,
    // 2026-11-25: the leader stood eight minutes each at Ghamoo-ra, Lady Sarevess and Old Serra'kis, 6 to 7 yd from the
    // edge its next swim began at, without a log line, and gave the boss up. After 6 s without a yard it goes there
    // straight.
    constexpr float NearPointYards = 12.0f;
    constexpr float NearPointHeight = 8.0f;
    constexpr uint32 NearStillMs = 6 * IN_MILLISECONDS;
    if (task.navActive && !player->IsInCombat() && !rt.rest.active && Dist2d(player, task.navX, task.navY) <= NearPointYards &&
        std::fabs(player->GetPositionZ() - task.navZ) <= NearPointHeight && player->GetExactDist(task.navX, task.navY, task.navZ) > 2.0f)
    {
        if (!task.navStillSince || Dist2d(player, task.navStillX, task.navStillY) > 1.0f)
        {
            task.navStillSince = now;
            task.navStillX = player->GetPositionX();
            task.navStillY = player->GetPositionY();
        }
        else if (getMSTimeDiff(task.navStillSince, now) >= NearStillMs)
        {
            LOG_INFO(ActivityLog, "activity: {} does not get to the next point of its way at ({:.0f}, {:.0f}, {:.0f}) in {}, {:.0f} yd away, in {} s: "
                "it goes there straight", player->GetName(), task.navX, task.navY, task.navZ, map->GetMapName(),
                player->GetExactDist(task.navX, task.navY, task.navZ), getMSTimeDiff(task.navStillSince, now) / IN_MILLISECONDS);
            task.navStillSince = now;
            // (The edge of a drop or swim: down it from where the bot stands, the walk to the edge was what failed.)
            if (task.navDrop && player->GetExactDist(task.navLandX, task.navLandY, task.navLandZ) <= 15.0f)
            {
                DropDown(player, task.navLandX, task.navLandY, task.navLandZ, task.navSwim);
                task.navActive = false;
                task.navDrop = false;
                return true;
            }
            BotDriver::Rest(player);
            BotDriver::PauseAI(player, 3 * IN_MILLISECONDS);
            if (player->IsNonMeleeSpellCast(false))
                player->InterruptNonMeleeSpells(false);
            player->StopMoving();
            player->GetMotionMaster()->Clear();
            player->GetMotionMaster()->MovePoint(0, task.navX, task.navY, task.navZ, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false);
            return true;
        }
    }
    else
        task.navStillSince = 0;
    // At the edge of a drop: down it, as a player jumps down a ledge (a straight fall onto the part below, which no
    // walk joins: the plateau of Lord Serpentis in the Wailing Caverns lies 6 yd below the way above it).
    if (task.navDrop && Dist2d(player, task.navX, task.navY) <= 2.5f && std::fabs(player->GetPositionZ() - task.navZ) <= 3.0f &&
        !player->IsInCombat())
    {
        DropDown(player, task.navLandX, task.navLandY, task.navLandZ, task.navSwim);
        task.navActive = false;
        task.navDrop = false;
        return true;
    }
    x = task.navX;
    y = task.navY;
    z = task.navZ;
    return false;
}

void OrderService::DropDown(Player* player, float x, float y, float z, bool swim)
{
    if (swim)
        LOG_INFO(ActivityLog, "activity: {} swims {:.0f} yd from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f}) in {}: no walk leads "
            "there, the water does", player->GetName(), player->GetExactDist2d(x, y), player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ(), x, y, z, player->GetMap()->GetMapName());
    else
        LOG_INFO(ActivityLog, "activity: {} jumps down {:.0f} yd from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f}) in {}: no walk leads "
            "there, a drop does", player->GetName(), player->GetPositionZ() - z, player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ(), x, y, z, player->GetMap()->GetMapName());
    BotDriver::Rest(player);
    BotDriver::PauseAI(player, 3 * IN_MILLISECONDS);
    // (A cast holds a player in place: a warlock of the test group began Create Soulstone again and again at the shore
    // and "swam" from it every two seconds for half a minute without a yard, 2026-11-25.)
    if (player->IsNonMeleeSpellCast(false))
        player->InterruptNonMeleeSpells(false);
    player->StopMoving();
    player->GetMotionMaster()->Clear();
    // (Straight, without a path: the path finder has none between the two parts; the ground guard lets a drop of up to
    // 25 yd inside a dungeon through.)
    player->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false);
    // (A member of the test group swam 9 yd every two seconds for a minute: half way across its way was planned again
    // from the water, began at the edge behind it, and it turned back to that edge, 2026-11-25.)
    if (BotRuntime* rt = FindRuntime(player->GetGUID()))
    {
        float const across = player->GetExactDist(x, y, z);
        rt->dropUntil = std::max<uint32>(getMSTime() + uint32(across / 3.0f * IN_MILLISECONDS) + 2 * IN_MILLISECONDS, 1);
        rt->dropX = x;
        rt->dropY = y;
        rt->dropZ = z;
        rt->dropAt = std::max<uint32>(getMSTime(), 1);
        rt->dropFromX = player->GetPositionX();
        rt->dropFromY = player->GetPositionY();
        rt->dropFromZ = player->GetPositionZ();
    }
}

void OrderService::DropDone(Player* player, BotRuntime& rt)
{
    rt.dropUntil = 0;
    // (Blackfathom Deeps, 2026-11-25: the jump of 4 yd off a ledge before Ghamoo-ra ended on a floor the collision data has
    // and the navmesh lacks, 4 yd above its landing and beside the ledge it came from; every way began with the same
    // jump again, for minutes.)
    if (player->GetPositionZ() - rt.dropZ <= 2.5f || Dist2d(player, rt.dropX, rt.dropY) > 6.0f)
        return;
    // (Over water it floats above its landing: that is a swim, not a drop that failed.)
    if (player->GetMap()->IsInWater(player->GetPhaseMask(), rt.dropX, rt.dropY, rt.dropZ + 1.0f, 2.0f) ||
        player->GetMap()->IsInWater(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ() - 1.0f, 2.0f))
        return;
    uint32 const blocked = sDungeonNav.BlockDrop(player->GetMapId(), rt.dropFromX, rt.dropFromY, rt.dropFromZ, rt.dropX, rt.dropY, rt.dropZ);
    rt.dropAt = 0;
    LOG_INFO(ActivityLog, "activity: {} came down at ({:.0f}, {:.0f}, {:.0f}) in {}, {:.0f} yd above the landing at ({:.0f}, {:.0f}, {:.0f}): "
        "the {} of the navmesh there no longer count{}", player->GetName(), player->GetPositionX(), player->GetPositionY(),
        player->GetPositionZ(), player->GetMap()->GetMapName(), player->GetPositionZ() - rt.dropZ, rt.dropX, rt.dropY, rt.dropZ,
        blocked == 1 ? "drop" : Acore::StringFormat("{} drops", blocked), blocked == 1 ? "s" : "");
}

bool OrderService::NavFrom(Player* player, BotRuntime const& rt, float& x, float& y, float& z) const
{
    // (Blackfathom Deeps, 2026-11-25: after the jump of 4 yd into the dry pool before Ghamoo-ra the leader stood on the
    // floor of the collision data 4 yd above the navmesh's part there, 1 yd under the ledge it had come from; planned
    // from where it stood, every way began on that ledge and with the swim and the jump again, every ten seconds for
    // eight minutes.)
    x = player->GetPositionX();
    y = player->GetPositionY();
    z = player->GetPositionZ();
    if (rt.dropAt && getMSTimeDiff(rt.dropAt, getMSTime()) < 30 * IN_MILLISECONDS && Dist2d(player, rt.dropX, rt.dropY) <= 6.0f &&
        std::fabs(z - rt.dropZ) <= 6.0f)
    {
        x = rt.dropX;
        y = rt.dropY;
        z = rt.dropZ;
        return true;
    }
    return false;
}

void OrderService::SwimAlong(Player* player, BotRuntime& rt, float x, float y, float z)
{
    // (Blackfathom Deeps, 2026-11-25: after the jump into the pool before Ghamoo-ra the leader floated at the surface 4 yd
    // above the pool's floor, the way's part; every walk the path finder made from there began on the ledge beside the
    // pool and had no way, and it stood or went back to the ledge for half an hour.)
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (!rt.swimAlongLogged || getMSTimeDiff(rt.swimAlongLogged, now) >= 30 * IN_MILLISECONDS)
    {
        rt.swimAlongLogged = now;
        LOG_INFO(ActivityLog, "activity: {} swims along its way in {}, {:.0f} yd above the way's ground under the water: on to ({:.0f}, {:.0f}, "
            "{:.0f})", player->GetName(), player->GetMap()->GetMapName(), player->GetPositionZ() - z, x, y, z);
    }
    BotDriver::Rest(player);
    BotDriver::PauseAI(player, 2 * IN_MILLISECONDS);
    if (player->IsNonMeleeSpellCast(false))
        player->InterruptNonMeleeSpells(false);
    player->StopMoving();
    player->GetMotionMaster()->Clear();
    player->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false);
    float const across = player->GetExactDist(x, y, z);
    rt.dropFromX = player->GetPositionX();
    rt.dropFromY = player->GetPositionY();
    rt.dropFromZ = player->GetPositionZ();
    rt.dropUntil = std::max<uint32>(now + uint32(across / 4.0f * IN_MILLISECONDS) + IN_MILLISECONDS, 1);
    rt.dropX = x;
    rt.dropY = y;
    rt.dropZ = z;
    rt.dropAt = now;
}

void OrderService::KeepDropMoving(Player* player, BotRuntime& rt)
{
    BotDriver::PauseAI(player, 2 * IN_MILLISECONDS);
    if (player->IsNonMeleeSpellCast(false))
        return;
    bool onIt = false;
    if (player->isMoving() && !player->movespline->Finalized())
    {
        G3D::Vector3 const end = player->movespline->FinalDestination();
        onIt = std::fabs(end.x - rt.dropX) + std::fabs(end.y - rt.dropY) + std::fabs(end.z - rt.dropZ) < 3.0f;
    }
    if (onIt)
        return;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (!rt.dropKeptLogged || getMSTimeDiff(rt.dropKeptLogged, now) >= 30 * IN_MILLISECONDS)
    {
        rt.dropKeptLogged = now;
        LOG_INFO(ActivityLog, "activity: {} is off its way down or through the water in {} at ({:.0f}, {:.0f}, {:.0f}) ({}, movement {}): "
            "on again to ({:.0f}, {:.0f}, {:.0f})", player->GetName(), player->GetMap()->GetMapName(), player->GetPositionX(),
            player->GetPositionY(), player->GetPositionZ(), player->isMoving() ? "moving elsewhere" : "standing",
            uint32(player->GetMotionMaster()->GetCurrentMovementGeneratorType()), rt.dropX, rt.dropY, rt.dropZ);
    }
    player->StopMoving();
    player->GetMotionMaster()->Clear();
    player->GetMotionMaster()->MovePoint(0, rt.dropX, rt.dropY, rt.dropZ, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false);
}

bool OrderService::WalkDungeonWay(Player* player, float x, float y, float z, bool inFight)
{
    // A drop or swim under way: on to its landing, nothing planned anew meanwhile.
    if (BotRuntime* rt = FindRuntime(player->GetGUID()); rt && rt->dropUntil && getMSTime() < rt->dropUntil)
    {
        if (player->GetExactDist(rt->dropX, rt->dropY, rt->dropZ) > 2.0f)
        {
            // (Something stopped it on the way or moved it elsewhere: on again, straight.)
            KeepDropMoving(player, *rt);
            return true;
        }
        rt->dropUntil = 0;
    }
    // A member of a dungeon group on the way to its tank: mod-playerbots' follow walks only what the core's path finder
    // has (148 polygons) and no drop, so far behind the member walks the navmesh's way itself, a stretch at a time.
    Map* map = player->GetMap();
    if (!map || !map->IsDungeon() || (player->IsInCombat() && !inFight) || player->GetTransport() || !player->IsAlive())
        return false;
    NavPoint point, top;
    if (!sDungeonNav.PointAlong(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), x, y, z, 30.0f,
            point, nullptr, &top, map))
        return false;
    if (point.drop)
    {
        if (Dist2d(player, top.x, top.y) <= 2.5f && std::fabs(player->GetPositionZ() - top.z) <= 3.0f)
        {
            DropDown(player, point.x, point.y, point.z, point.swim);
            return true;
        }
        point = top;
    }
    BotDriver::Rest(player);
    BotDriver::PauseAI(player, 2500);
    if (player->IsNonMeleeSpellCast(false))
        player->InterruptNonMeleeSpells(false);
    player->GetMotionMaster()->MovePoint(0, point.x, point.y, point.z);
    return true;
}

void OrderService::DungeonGoTo(Player* player, uint32 mapId, float x, float y, float z)
{
    if (mapId == player->GetMapId())
        if (BotRuntime* rt = FindRuntime(player->GetGUID()))
            if (ViaDungeonPath(player, *rt, x, y, z))
                return;
    BotDriver::GoTo(player, mapId, x, y, z, false);
}

void OrderService::WalkTo(Player* player, BotRuntime& rt, uint32 mapId, float x, float y, float z)
{
    uint32 const now = getMSTime();
    BotTask& task = rt.task;
    // Out of a room only a teleporter leads out of: to the teleporter first (LeaveLocalRoom changes the destination).
    if (mapId == player->GetMapId() && LeaveLocalRoom(player, rt, x, y, z))
        return;
    // A target on another level reached from a road node only: that node first (ViaRoadNode changes the destination).
    if (mapId == player->GetMapId())
        ViaRoadNode(player, rt, x, y, z);
    // Inside a dungeon along the whole navmesh's way (the core's path finder stops after 148 polygons).
    if (mapId == player->GetMapId() && ViaDungeonPath(player, rt, x, y, z))
        return;
    // A target across water that no way on the ground leads to: it swims there (the Bloodsail Charts on the ships off
    // Booty Bay; the road map's only way led over the ship's pier and the Wild Shore, and every step was refused).
    if (mapId == player->GetMapId() && StepSwim(player, task, x, y, z))
        return;
    // The grids on the way exist before the path finder is asked (the core's own path of the walk included: with a
    // grid not created after a restart it is a straight line through the hills, which the ground check refuses).
    if (mapId == player->GetMapId())
        EnsureNavmesh(player, player->GetPositionX(), player->GetPositionY(), x, y);
    // Out of a canal whose floor leads nowhere: along the surface to the shore first (WadeOut), and while it swims there
    // nothing else moves it.
    if (task.wadeUntil)
    {
        if (now < task.wadeUntil && player->isMoving() && !player->IsInCombat() && player->IsAlive())
        {
            BotDriver::PauseAI(player, 3 * IN_MILLISECONDS);
            rt.why = "Swimming out of the water to the shore";
            return;
        }
        task.wadeUntil = 0;
        task.walkX = task.walkY = 0.0f;
        task.stallSince = 0;
    }
    if (mapId == player->GetMapId() && WadeOut(player, task, x, y, z, "its walk"))
        return;
    float const direct = Dist2d(player, x, y);
    Settings const settings = sBotSettings.For(player);

    // NewRpg's long-distance walk goes for the point nearest to its goal and stays there when the way on leads away
    // from the goal first (the rim of Whelgar's Excavation in the Wetlands: bot after bot stood there, mounted, for
    // five minutes and again, with a path down the slope a few yards beside it). A walk that has not moved the bot
    // for ten seconds is walked by the planner itself from then on: along the core's own path, a stretch at a time.
    constexpr uint32 WalkStallMs = 10 * IN_MILLISECONDS;
    if (!task.ownSteps && mapId == player->GetMapId())
    {
        bool const busy = !player->IsAlive() || player->IsInCombat() || player->IsNonMeleeSpellCast(false) || player->IsInFlight() ||
            player->GetTransport() || BotDriver::GetRpgState(player) == RpgState::TravelFlight || task.liftAt;
        if (busy || !task.stallSince || now - task.stallSeen > 3 * IN_MILLISECONDS || Dist2d(player, task.stallX, task.stallY) > 3.0f)
        {
            task.stallSince = now ? now : 1;
            task.stallX = player->GetPositionX();
            task.stallY = player->GetPositionY();
        }
        else if (now - task.stallSince >= WalkStallMs && direct > 8.0f)
        {
            task.ownSteps = true;
            task.ownFromX = player->GetPositionX();
            task.ownFromY = player->GetPositionY();
            ++task.ownRounds;
            // (The line names NewRpg's state and the AI's, so the cause of these stalls can be read off the log: in an hour
            // of the integration server, 2026-11-26, 145 walks stood 10 s here, most of them seconds after a fight or a
            // loot; no moving flag was left set in any of them.)
            LOG_INFO(ActivityLog, "activity: {} does not get on with its walk ({} s at ({:.0f}, {:.0f}), {:.0f} yd to go; rpg {}, movement "
                "{}; ai {}): the planner walks it along the path itself", player->GetName(), (now - task.stallSince) / IN_MILLISECONDS,
                player->GetPositionX(), player->GetPositionY(), direct, ToString(BotDriver::GetRpgState(player)),
                uint32(player->GetMotionMaster()->GetCurrentMovementGeneratorType()), BotDriver::AIState(player));
        }
        task.stallSeen = now;
    }
    // The own steps are a stretch, not the rest of the walk: NewRpg rides, the planner's steps go on foot, and past the
    // place it hung at NewRpg walks as well as ever. After 150 yd it gets the walk back; a walk that went over to own
    // steps three times stays with them.
    constexpr float OwnStretch = 150.0f;
    constexpr uint8 OwnRoundsMax = 3;
    if (task.ownSteps && task.ownRounds && task.ownRounds < OwnRoundsMax && !task.tookOver &&
        Dist2d(player, task.ownFromX, task.ownFromY) >= OwnStretch)
    {
        task.ownSteps = false;
        task.stallSince = 0;
        task.ownStepAt = 0;
        task.walkX = task.walkY = 0.0f; // the walk is set again for NewRpg
    }
    // Where NewRpg is sent, or where the planner's own step leads.
    // A walk straight to its destination (no route) whose own steps find no way this often in a row has none: the
    // task's stuck rule fires at once. The rim of Whelgar's Excavation again: "War Banners" lies 89 yd from it and 20 yd
    // up a cliff; the bot stood four and a half minutes before the task ended, and the next task began at the same
    // place.
    constexpr uint8 OwnFailLimit = 3;
    auto go = [&](float px, float py, float pz)
    {
        if (task.ownSteps)
        {
            if (OwnStep(player, task, mapId, px, py, pz))
            {
                task.ownFailStreak = 0;
                return;
            }
            if (!task.route.found && !player->IsInCombat() && task.ownFailStreak < 255 && ++task.ownFailStreak >= OwnFailLimit &&
                !task.noWay)
            {
                // Perhaps the water leads there (a ship's deck off the coast): a swim is looked for once first.
                if (!task.swimLooked)
                {
                    task.swimArmed = true;
                    if (StepSwim(player, task, x, y, z))
                        return;
                }
                // The whole navmesh may have it: the core's path finder sees 148 polygons of the loaded grids, and a long
                // straight walk often lies beyond that (Liriel, level 36, in Arathi on 2026-11-26: "no path ... path type
                // 0xa" for Syndicate Highwaymen 877 and 640 yd away, which the navmesh reaches in 893 and 932 yd along an
                // almost straight line; the end was counted against the spot for the whole guild, which then left two of
                // the quest's camps alone for a day). Once per task, in the open world.
                constexpr float OpenWayYards = 4000.0f;
                std::string navWhy;
                if (!player->GetMap()->Instanceable() && task.OpenTryDue(player->GetPositionX(), player->GetPositionY()))
                {
                    float length = 0.0f;
                    std::vector<NavPoint> corners;
                    std::string unsafe;
                    if (!sDungeonNav.FindWay(mapId, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), x, y, z, &corners,
                            &length, player->GetMap(), &navWhy))
                        navWhy = "; the navmesh has none either: " + navWhy;
                    else if (length > OpenWayYards)
                    {
                        navWhy = Acore::StringFormat("; the navmesh's is {:.0f} yd", length);
                        length = 0.0f;
                    }
                    else if (!NavWayIsSafe(player, rt, corners, unsafe))
                        navWhy = Acore::StringFormat("; the navmesh's ({:.0f} yd) does not do: {}", length, unsafe);
                    else
                    {
                        task.openWay = true;
                        task.navActive = false;
                        task.ownSteps = false;
                        task.ownFailStreak = 0;
                        task.stallSince = 0;
                        task.walkX = task.walkY = 0.0f;
                        LOG_INFO(ActivityLog, "activity: {} finds no path with the path finder from ({:.0f}, {:.0f}, {:.0f}) towards {} ({}): "
                            "the navmesh has a way of {:.0f} yd there ({:.0f} yd straight), and it walks that way", player->GetName(),
                            player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                            task.targetName.empty() ? std::string("its target") : task.targetName,
                            task.ownNote.empty() ? std::string("none") : task.ownNote, length, player->GetExactDist(x, y, z));
                        return;
                    }
                }
                task.noWay = true;
                LOG_INFO(ActivityLog, "activity: {} has no way from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f}): {}{}",
                    player->GetName(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), px, py, pz,
                    task.ownNote.empty() ? std::string("the path finder has none") : task.ownNote, navWhy);
            }
            // No path leads on: NewRpg's walk again, set once and left alone.
            if (BotDriver::GetRpgState(player) == RpgState::GoCamp)
                return;
        }
        BotDriver::GoTo(player, mapId, px, py, pz, false);
    };

    // Under water with a way to go to a place that is not under water: up to the surface first, once per task. From
    // the sea floor the way to a place ashore is a line into the rising slope, which the ground guard cancels (it used
    // to end under the map).
    // (Not latched while the bot is out of the water: a task that began ashore may take it down later. Sheesh mined an
    // Iron Deposit 58 yd down in Faldir's Cove on a detour of its vendor walk, and every walk on from there ran through
    // the rock of the cove: refused for a minute and a half, the vendor given up, 2026-11-26.)
    if (!task.surfaced && direct > SurfaceFirstDistance && mapId == player->GetMapId() && !player->IsInCombat() && IsDiving(player))
    {
        if (player->GetMap()->IsUnderWater(player->GetPhaseMask(), x, y, z, player->GetCollisionHeight()))
            task.surfaced = true; // the destination is down there too
        else
        {
            if (!player->isMoving() || now - task.lastAssert >= AssertGapMs)
            {
                SwimToSurface(player);
                task.lastAssert = now;
                task.walkX = task.walkY = 0.0f; // the walk is set again from the surface
            }
            return;
        }
    }

    bool const mayRoute = settings.roadsEnable && mapId == player->GetMapId() && !player->GetTransport() && !task.routeBlocked;
    // A short walk goes straight, unless the path finder has no whole path for it (the destination is on another
    // level: the flight master on Stormwind's wall, 27 yd above the gate): then the road map is asked as well.
    // Asked again from where the bot stands every 40 yd of a straight walk: the walk NewRpg makes is not the path that
    // was asked for (from Theramore's flight master the path finder has a way onto the pier; 89 yd on, beside it, the
    // next step to the ship ran 156 yd through the air, and the bot was rescued; the road map leads over the pier).
    constexpr float ShortReprobeYards = 40.0f;
    // (A destination right above or below counts too: a level-50 priest on the Cauldron's rim walked to its squad
    // leader 8 yd away on the map and 50 yd below it, beside the lift; no probe asked, no route, its straight steps
    // down refused every 30 s for two minutes, twice, 2026-11-27.)
    constexpr float ShortProbeHeight = 8.0f;
    if (mayRoute && direct <= settings.roadsMinDistance &&
        (direct > ShortProbeMin || std::fabs(player->GetPositionZ() - z) > ShortProbeHeight) &&
        (!task.shortProbed || std::fabs(task.shortX - x) + std::fabs(task.shortY - y) > 30.0f ||
         (!task.shortNoPath && Dist2d(player, task.shortFromX, task.shortFromY) > ShortReprobeYards)))
    {
        bool const hadPath = task.shortProbed && !task.shortNoPath;
        task.shortProbed = true;
        task.shortX = x;
        task.shortY = y;
        task.shortFromX = player->GetPositionX();
        task.shortFromY = player->GetPositionY();
        task.shortNoPath = !WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), x, y, z);
        // Without a whole path the road map is asked now, unless a route was planned from here already. (Not only when
        // the path was lost on the way: a route planned before a flight from far away kept the walk from the landing
        // to Theramore's ship from asking the road map at all.)
        if (task.shortNoPath && (hadPath || !task.route.found || Dist2d(player, task.routeStartX, task.routeStartY) > ShortReprobeYards))
        {
            task.routePlanned = false;
            task.nextRoutePlan = 0;
        }
    }
    bool const shortNoPath = task.shortProbed && task.shortNoPath && direct <= settings.roadsMinDistance;
    bool const wantsRoute = mayRoute && (direct > settings.roadsMinDistance || shortNoPath);
    if (wantsRoute)
    {
        bool plan = !task.routePlanned || task.route.mapId != mapId ||
            std::fabs(task.routeDestX - x) + std::fabs(task.routeDestY - y) > 30.0f;
        if (!plan && task.route.found && task.routeIndex < task.route.points.size())
        {
            RoutePoint const& at = task.route.points[task.routeIndex];
            plan = Dist2d(player, at.x, at.y) > StrayDistance && task.routeIndex > 0;
        }
        if (plan && now >= task.nextRoutePlan)
        {
            task.nextRoutePlan = now + RoutePlanGapMs;
            task.routePlanned = true;
            task.liftAt = 0; // a ride of the old route is not boarded (the planner does not plan during a ride)
            task.routeDestX = x;
            task.routeDestY = y;
            task.routeIndex = 0;
            task.routeTarget = -1;
            // Around the places the bot has set aside (where it kept dying or lost its fights), when there is a way around.
            uint8 const level = player->GetLevel();
            auto avoided = [&rt, mapId, level](float px, float py) { return rt.IsInAvoidedArea(mapId, px, py, level); };
            // And around the groups it cannot take (the risk model's answer for this bot).
            auto canTake = [&rt, player](uint8 count, uint8 crowdLevel) { return Risk::CanFight(player, rt, crowdLevel, count); };
            // Not from a node the bot twice did not get to for what stood before it (BotRuntime::NoteRefusedStart).
            RoadMap::LegCheck const walkable = LegCheckFor(player);
            float const hereX = player->GetPositionX(), hereY = player->GetPositionY();
            auto legs = [&rt, &walkable, hereX, hereY, now](RoutePoint const& from, RoutePoint const& to)
            {
                bool const firstLeg = std::fabs(from.x - hereX) + std::fabs(from.y - hereY) < 5.0f;
                return !(firstLeg && rt.IsRefusedStart(to.x, to.y, now)) && walkable(from, to);
            };
            task.route = sRoadMap.FindRoute(mapId, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), x, y, z,
                player->GetTeamId(), level, rt.refusedStarts.empty() ? walkable : RoadMap::LegCheck(legs),
                rt.avoidAreas.empty() ? RoadMap::AvoidCheck() : RoadMap::AvoidCheck(avoided), canTake,
                task.wideStart ? RoadMap::CampOutStretch : RoadMap::CrowdStartStretch);
            // (Not for a walk that has no other way: any route is better than none.)
            // A travel is not walked straight in its place unless the path finder has a straight way, hop by hop: from
            // Theramore's pier the Shimmering Flats are 2256 yd away and 30702 yd by road (around the mountains, through
            // the Barrens). The travel there "found no route and walked straight", to the end of the pier and into the
            // pirates moored beside it, five times in six minutes, before its zone was set aside. It ends at once.
            bool farAround = false;
            if (task.route.found && !shortNoPath && task.route.length > task.route.direct * MaxDetour + 2000.0f)
            {
                float chain = 0.0f;
                if (task.kind == TaskKind::Travel && !task.retreat && !ChainReaches(player, x, y, z, chain))
                {
                    farAround = true;
                    task.noWay = true;
                    task.farAround = true;
                    LOG_INFO(ActivityLog, "activity: {} has no way worth walking to ({:.0f}, {:.0f}): the road ({}) is {:.0f} yd for "
                        "{:.0f} yd straight, and the path finder has no straight way", player->GetName(), x, y, task.route.via,
                        task.route.length, task.route.direct);
                }
                task.route.found = false;
            }
            // A short walk whose straight line is safe needs no detour over a node, when the path finder has a whole
            // path for it (from Stormwind's inn to the harbor it has none: the walk ends on the cliff above the docks).
            if (task.route.found && !shortNoPath && direct < ShortWalk && task.route.length > direct * ShortWalkDetour &&
                int32(sRoadMap.DangerAlong(mapId, player->GetPositionX(), player->GetPositionY(), x, y, player->GetTeamId())) <=
                    int32(player->GetLevel()) + SafeLevels)
            {
                if (WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), x, y, z))
                {
                    task.route.found = false;
                    return WalkTo(player, rt, mapId, x, y, z);
                }
            }
            // No walk the path finder proves leads from where the bot stands to a node, it stands under the surface (a
            // cave, a hive, a building) and it came here on foot: the way it came first, then a route from there.
            // (Inside Hive'Ashi in Silithus the route began at the node above the hive; the path finder's partial path
            // towards it ended in a pocket 18 yd under the surface from which nothing led on or back, and a mage was
            // "rescued" there again and again, round 146. In the open, in a lake or on a snow field, the first stretch
            // of such a route is walked as before: the test turned two such walks into a minute's walk back.)
            constexpr float UnderSurfaceYards = 8.0f;
            auto underSurface = [player]()
            {
                float const surface = player->GetMap()->GetHeight(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(),
                    MAX_HEIGHT, true, MAX_HEIGHT);
                return surface > INVALID_HEIGHT && surface - player->GetPositionZ() > UnderSurfaceYards;
            };
            // (But a whole path of the path finder to the target itself is walked: a paladin grinding inside Hive'Zora
            // turned to a Reaver 208 yd off in the hive, the route's start was a guess, and it walked back the way it
            // came, twice in two hours, the second time 91 s among the hive's packs, round 183.)
            if (task.route.found && task.route.startGuessed && underSurface() &&
                WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), x, y, z))
            {
                task.route.found = false;
                return WalkTo(player, rt, mapId, x, y, z);
            }
            // The whole navmesh first, once per task: off a mesa the path finder does not see the ramps (Thousand Needles,
            // 2026-11-25: on the Grimtotem mesas the route's start was a guess, and the leader walked back the way it
            // came, or was "cut off" and rescued down to the canyon, after it had walked up by the navmesh's way).
            if (task.route.found && task.route.startGuessed && !player->GetMap()->Instanceable() &&
                task.OpenTryDue(player->GetPositionX(), player->GetPositionY()))
            {
                float length = 0.0f;
                std::string navWhy;
                std::vector<NavPoint> corners;
                std::string unsafe;
                if (!sDungeonNav.FindWay(mapId, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), x, y, z, &corners, &length,
                        player->GetMap(), &navWhy))
                    LOG_INFO(ActivityLog, "activity: {} finds no walk from ({:.0f}, {:.0f}, {:.0f}) to the road ({}) towards {}, and the navmesh "
                        "has none either: {}", player->GetName(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                        task.route.via, task.targetName.empty() ? std::string("its target") : task.targetName, navWhy);
                // (The navmesh's way is the shortest, and it knows no creatures: Stonehorn, level 49, on a cliff above
                // the Southsea pirates' cove in Tanaris, took it four times in a minute, each time "its way led into the
                // middle of their camp" (32 creatures), and used its hearthstone, round 257.)
                else if (length <= 4000.0f && !NavWayIsSafe(player, rt, corners, unsafe))
                    LOG_INFO(ActivityLog, "activity: {} finds no walk from ({:.0f}, {:.0f}, {:.0f}) to the road ({}) towards {}, and the navmesh's "
                        "way ({:.0f} yd) does not do: {}", player->GetName(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                        task.route.via, task.targetName.empty() ? std::string("its target") : task.targetName, length, unsafe);
                else if (length <= 4000.0f)
                {
                    task.openWay = true;
                    task.navActive = false;
                    task.route.found = false;
                    LOG_INFO(ActivityLog, "activity: {} finds no walk from where it stands to the road ({}) towards {}: the navmesh has a way "
                        "of {:.0f} yd there ({:.0f} yd straight), and it walks that way", player->GetName(), task.route.via,
                        task.targetName.empty() ? std::string("its target") : task.targetName, length, player->GetExactDist(x, y, z));
                    return WalkTo(player, rt, mapId, x, y, z);
                }
            }
            if (task.route.found && task.route.startGuessed && underSurface() &&
                StartTrailOut(player, rt, Acore::StringFormat("no walk leads from where it stands to the road ({}) towards {}",
                    task.route.via, task.targetName.empty() ? std::string("its target") : task.targetName)))
                return;
            task.routeStartAt = task.route.found ? now : 0;
            task.routeStartX = player->GetPositionX();
            task.routeStartY = player->GetPositionY();
            if (task.route.found)
                LOG_INFO(ActivityLog, "activity: {} takes the route {} ({:.0f} yd, {:.0f} yd straight; {})", player->GetName(),
                    task.route.via, task.route.length, task.route.direct, task.route.danger >= RoadMap::TownDanger ?
                        std::string("it leads through a town of the other side") :
                        Acore::StringFormat("strongest hostile creatures on it level {}", uint32(task.route.danger)));
            else if (!farAround && !task.retreat && (direct > settings.roadsMinDistance || shortNoPath))
            {
                // No route and no whole way of the path finder hop by hop: the walk does not start (the owner: no walk goes
                // blindly straight). (A warlock in the Searing Gorge "found no route for 2264 yd" to a vendor in the
                // Burning Steppes, walked straight, came onto a scaffold of the Cauldron and stood there.)
                float chain = 0.0f;
                if (!ChainReaches(player, x, y, z, chain))
                {
                    // An island has no way on the ground: the water first, once per task. Kodahorn (34) was given
                    // the hand-in of "Claim Rackmore's Treasure!" at Rackmore's Chest on Ranazjar Isle, 680-850 yd
                    // off Desolace's coast; its travel ended "stuck" at once, twice, and the errand was given up for
                    // half an hour, again and again (round 257): the swim is armed only by a refused step, and a
                    // walk that never starts refuses none.
                    if (mapId == player->GetMapId() && !task.swimLooked)
                    {
                        task.swimArmed = true;
                        if (StepSwim(player, task, x, y, z))
                            return;
                    }
                    task.noWay = true;
                    LOG_INFO(ActivityLog, "activity: {} finds no way to walk to ({:.0f}, {:.0f}): no route of the road map for {:.0f} yd, "
                        "and the path finder has no whole way there", player->GetName(), x, y, direct);
                }
                else
                {
                    LOG_INFO(ActivityLog, "activity: {} finds no route for {:.0f} yd to ({:.0f}, {:.0f}) and walks the path finder's way",
                        player->GetName(), direct, x, y);
                    task.ownSteps = true;
                    task.ownFailStreak = 0;
                    task.ownStepAt = 0;
                }
            }
            else if (!farAround)
            {
                LOG_INFO(ActivityLog, "activity: {} finds no route for {:.0f} yd to ({:.0f}, {:.0f}) and walks straight", player->GetName(),
                    direct, x, y);
                if (!task.straightSince && !task.noWay)
                {
                    task.straightSince = now ? now : 1;
                    task.straightX = player->GetPositionX();
                    task.straightY = player->GetPositionY();
                }
            }
        }
    }
    // A route given up for a step with no way on the ground (OrderService: "leaves its route") leaves the rest of the
    // walk without the road map, and its destination still wants a way: not NewRpg's walk towards it, which goes for
    // the nearest point it can get to. (From the floor of Un'Goro the route to Silithus began at Valor's Rest, 200 yd
    // up the passage the path finder has no way through; NewRpg walked a hunter 1100 yd the other way, into the Golakka
    // basin under Silithus's cliffs, whose ground the map counts to Silithus: the travel "arrived", and every way out of
    // the basin passed elite Stegodons, until its hearthstone, round 158.) Without a whole way of the path finder hop by
    // hop the walk has none, and the task's stuck rule ends it.
    if (task.routeBlocked && !task.noWay && !task.retreat && mapId == player->GetMapId() && !player->GetTransport() &&
        direct > settings.roadsMinDistance &&
        (!task.blockedProbed || std::fabs(task.blockedX - x) + std::fabs(task.blockedY - y) > 30.0f))
    {
        task.blockedProbed = true;
        task.blockedX = x;
        task.blockedY = y;
        float chain = 0.0f;
        if (!ChainReaches(player, x, y, z, chain))
        {
            task.noWay = true;
            LOG_INFO(ActivityLog, "activity: {} has no way on the ground to ({:.0f}, {:.0f}) once it left its route: the path finder "
                "has no whole way there either ({:.0f} yd)", player->GetName(), x, y, direct);
            BotDriver::Rest(player);
            return;
        }
        task.ownSteps = true;
        task.ownFailStreak = 0;
        task.ownStepAt = 0;
    }
    // Not through a town of the other side (RoadMap::InHostileTown), whatever the task and the bot's level. The tasks
    // with a walk gate of their own (pickups, hand-ins, vendors, trainers, objectives, interactions, travels and the
    // walk to a dock) refuse such a way there, with their reasons; this is for the rest: a grind spot behind
    // Hammerfall was walked to over the node in the town's middle. The walk counts as one without a way, and the
    // task's own rule for that ends it. A bot inside such a town may walk out.
    // (The travels to an anvil or forge and to a waiting squad member have no gate of their own: a level-35 Horde miner
    // walked to the forge of Goldshire on one, round 244.)
    bool const gated = task.kind == TaskKind::Pickup || task.kind == TaskKind::TurnIn || task.kind == TaskKind::Vendor ||
        task.kind == TaskKind::Train || task.kind == TaskKind::Objective || task.kind == TaskKind::Interact ||
        task.kind == TaskKind::Voyage || (task.kind == TaskKind::Travel && !task.retreat && !task.hubFocus && !task.fetchSquad);
    if (!gated && mapId == player->GetMapId() && !task.noWay && !player->GetTransport() &&
        (!task.townChecked || task.townRoute != task.route.found || std::fabs(task.townX - x) + std::fabs(task.townY - y) > 30.0f))
    {
        task.townChecked = true;
        task.townRoute = task.route.found;
        task.townX = x;
        task.townY = y;
        TeamId const team = player->GetTeamId();
        bool const inTown = sRoadMap.InHostileTown(mapId, player->GetPositionX(), player->GetPositionY(), team);
        // The road map's way leads through the town and a straight way the path finder has passes it by: that one. (A
        // level-28 Horde bot at Raynewood Retreat in Ashenvale was sent 351 yd out of a camp to the spirit healer of
        // Nightsong Woods; the path finder walks straight there, the road map's way of 1491 yd led past the Ashenvale
        // Sentinels 75 yd beside it, the walk was refused, and it went home by hearthstone, round 244.)
        constexpr float StraightMaxDetour = 1.6f;
        float chain = 0.0f;
        if (!inTown && task.route.found && task.route.danger >= RoadMap::TownDanger &&
            sRoadMap.DangerAlong(mapId, player->GetPositionX(), player->GetPositionY(), x, y, team) < RoadMap::TownDanger &&
            ChainReaches(player, x, y, z, chain) && chain <= task.route.direct * StraightMaxDetour + 100.0f)
        {
            LOG_INFO(ActivityLog, "activity: {} takes the straight way to ({:.0f}, {:.0f}) ({}): the road map's way ({}) leads through "
                "a town of the other side; the path finder has one of {:.0f} yd ({:.0f} yd straight) past it", player->GetName(), x, y,
                ToString(task.kind), task.route.via, chain, task.route.direct);
            task.route = RoadRoute();
            task.routeBlocked = true; // the rest of the task walks without the road map
            task.walkX = task.walkY = 0.0f;
            task.townRoute = false;
        }
        else if (!inTown && (task.route.found ? task.route.danger :
                sRoadMap.DangerAlong(mapId, player->GetPositionX(), player->GetPositionY(), x, y, team)) >= RoadMap::TownDanger)
        {
            LOG_INFO(ActivityLog, "activity: {} does not walk to ({:.0f}, {:.0f}) ({}): the way{} leads through a town of the other "
                "side", player->GetName(), x, y, ToString(task.kind), task.route.found ? " (" + task.route.via + ")" : "");
            task.route = RoadRoute();
            task.noWay = true;
            BotDriver::Rest(player);
            return;
        }
    }
    // A straight walk for want of a route that does not get going (NewRpg has no path either and stands).
    if (task.straightSince && now - task.straightSince > StraightStartMs)
    {
        // (Not the rest the planner's own steps keep NewRpg in: with it the minute never ran out.)
        if (player->IsInCombat() || (BotDriver::GetRpgState(player) == RpgState::Rest && !task.ownSteps))
            task.straightSince = now; // a fight or a rest took the minute: once more
        else
        {
            if (Dist2d(player, task.straightX, task.straightY) < RouteStartDistance)
                task.noWay = true;
            task.straightSince = 0;
        }
    }
    else if (task.routePlanned && direct <= settings.roadsMinDistance && !shortNoPath)
        task.route.found = false; // the last stretch is a plain walk
    // (Not the route of a short walk that has no whole path: it is the only way there. It was thrown away in the tick
    // it was planned in, and the bot walked the straight line the ground guard refuses: a level-19 character stood
    // inside the Exodar, 59 yd beside and 141 yd below the flight master outside, for three quarters of an hour.)

    if (wantsRoute && task.route.found && !task.route.points.empty())
    {
        std::vector<RoutePoint> const& points = task.route.points;
        std::size_t const last = points.size() - 1;

        // The next ride on a lift of the route: its boarding stop is not passed until the ride took the bot to the
        // other stop (the two stand one above the other, sometimes less than PassHeight apart).
        std::size_t liftStop = last;
        uint32 liftEntry = 0;
        for (RoadRoute::Lift const& lift : task.route.lifts)
            if (lift.at >= task.routeIndex && lift.at < liftStop && lift.at + 1 <= last)
            {
                liftStop = lift.at;
                liftEntry = lift.entry;
            }
        // The furthest point the bot has reached, looking a stretch ahead of the last one.
        std::size_t const scanEnd = std::min<std::size_t>({ last, task.routeIndex + ScanPoints, liftStop });
        // (A point on another level is not reached: a route out of a building passes above or below itself.)
        for (std::size_t i = task.routeIndex; i <= scanEnd; ++i)
            if (Dist2d(player, points[i].x, points[i].y) <= PassDistance && std::fabs(points[i].z - player->GetPositionZ()) <= PassHeight)
                task.routeIndex = uint32(i);

        // At the boarding stop of a lift: the planner boards and rides (StepLift); NewRpg rests meanwhile. (The stop is
        // the platform's own position there, which is not its floor: the lifts of Thunder Bluff have it 8 yd above.)
        constexpr float LiftBoardYards = 8.0f, LiftBoardHeight = 12.0f, LandingYards = 3.0f;
        bool const atLanding = liftEntry && task.routeIndex == liftStop &&
            LiftLanding(player, task, points[liftStop].x, points[liftStop].y, points[liftStop].z) &&
            Dist2d(player, task.liftLandX, task.liftLandY) <= LandingYards;
        if (task.route.found && liftEntry && !task.liftAt && task.routeIndex == liftStop &&
            (Dist2d(player, points[liftStop].x, points[liftStop].y) <= LiftBoardYards || atLanding) &&
            std::fabs(points[liftStop].z - player->GetPositionZ()) <= LiftBoardHeight)
        {
            task.liftAt = uint32(liftStop) + 1;
            task.liftEntry = liftEntry;
            task.liftSince = 0;
            task.liftRiding = false;
            task.liftGuid.Clear();
            task.liftToX = points[liftStop + 1].x;
            task.liftToY = points[liftStop + 1].y;
            task.liftToZ = points[liftStop + 1].z;
            BotDriver::Rest(player);
        }
        if (task.liftAt && StepLift(player, rt))
            return;

        // A route the bot does not get going on (nothing moved since it was planned): the straight walk, as before
        // routes existed, is tried instead.
        if (task.routeStartAt && now - task.routeStartAt > RouteStartMs)
        {
            if (Dist2d(player, task.routeStartX, task.routeStartY) < RouteStartDistance)
            {
                LOG_INFO(ActivityLog, "activity: {} has not moved {} s after taking its route: it walks straight instead", player->GetName(),
                    RouteStartMs / IN_MILLISECONDS);
                task.route.found = false;
            }
            task.routeStartAt = 0;
        }

        // Past the last node: the rest is a plain walk to the destination.
        if (task.route.found && task.routeIndex >= last && Dist2d(player, points[last].x, points[last].y) <= PassDistance &&
            std::fabs(points[last].z - player->GetPositionZ()) <= PassHeight)
            task.route.found = false;

        if (task.route.found)
        {
            // The point about AheadDistance along the route from where the bot is (the points of a link lie 5 to 60 yd
            // apart: a fixed number of points ahead was a walk of up to 280 yd in one step).
            // A bot that is not on the route yet (the walk to its first node) goes to that point itself: the step to
            // a point farther along is not the walk the route was checked for (from the Wild Shore the node at the
            // graveyard above Booty Bay is a walk; a point 80 yd on, down in the tunnel, is 14 yd of rock away).
            std::size_t target = task.routeIndex;
            bool const onRoute = Dist2d(player, points[target].x, points[target].y) <= OnRouteDistance;
            // (Not past the boarding stop of the next lift: the bot walks to the stop and waits there.)
            for (float along = 0.0f; onRoute && target < liftStop && along < AheadDistance; ++target)
                along += std::sqrt((points[target + 1].x - points[target].x) * (points[target + 1].x - points[target].x) +
                    (points[target + 1].y - points[target].y) * (points[target + 1].y - points[target].y));
            bool moveOn = task.routeTarget < 0 || std::size_t(task.routeTarget) > last;
            if (!moveOn && target > std::size_t(task.routeTarget))
            {
                RoutePoint const& current = points[std::size_t(task.routeTarget)];
                moveOn = task.routeIndex >= std::size_t(task.routeTarget) || Dist2d(player, current.x, current.y) <= RetargetDistance;
            }
            bool const lost = BotDriver::GetRpgState(player) != RpgState::GoCamp && now - task.lastAssert >= AssertGapMs;
            if (moveOn || lost || task.ownSteps)
            {
                RoutePoint point = points[moveOn ? target : std::size_t(std::max<int32>(task.routeTarget, 0))];
                if (moveOn)
                    task.routeTarget = int32(target);
                // (The boarding stop of a lift: its landing, LiftLanding.)
                std::size_t const toward = std::size_t(std::max<int32>(task.routeTarget, 0));
                if (liftEntry && toward == liftStop && LiftLanding(player, task, point.x, point.y, point.z))
                    point = RoutePoint{ task.liftLandX, task.liftLandY, task.liftLandZ };
                // A point of the road map under or over the ground the path finder walks on goes onto that ground: the
                // road up to Thunder Bluff's Hunter Rise lies 3 yd under the ramp, the core's path to such a point ended
                // short of it, the ground guard refused the climb ("no way leads back down"), and an undead warlock
                // left its route and stood 14 minutes at the ramp's foot, round 260. (The nearest ground: a point on a
                // road under a bridge keeps the road.)
                else
                {
                    constexpr float PointAcross = 2.0f;   // sideways
                    constexpr float PointUpDown = 5.0f;   // up or down
                    constexpr float PointOffYards = 1.0f; // nearer than this: the point is on the ground
                    float gx = 0.0f, gy = 0.0f, gz = 0.0f;
                    if (NavmeshGroundNear(player, point.x, point.y, point.z, PointAcross, PointUpDown, gx, gy, gz) &&
                        std::fabs(gz - point.z) > PointOffYards)
                        point = RoutePoint{ gx, gy, gz };
                }
                go(point.x, point.y, point.z);
                task.lastAssert = now;
                task.walkX = point.x;
                task.walkY = point.y;
            }
            return;
        }
    }

    // Straight to the destination. Also when NewRpg still walks to a point of a route that was given up meanwhile.
    bool const otherTarget = std::fabs(task.walkX - x) + std::fabs(task.walkY - y) > 5.0f;
    if (otherTarget || task.ownSteps || (BotDriver::GetRpgState(player) != RpgState::GoCamp && now - task.lastAssert >= AssertGapMs))
    {
        go(x, y, z);
        task.lastAssert = now;
        task.walkX = x;
        task.walkY = y;
    }
}
