/*
 * RoadMap implementation. See RoadMap.h.
 */

#include "RoadMap.h"
#include "RiskModel.h"
#include "GuildAdventureSettings.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "World.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Perf.h"
#include "PlayerbotsDatabase.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WaypointMgr.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <cmath>
#include <queue>
#include <sstream>

using namespace GuildAdventure;

namespace
{
    // Hostile creatures this close to a path count as "on the way": in full within the creature's reach, and one
    // level less for every DangerFalloff yards beyond, for DangerFalloffLevels levels. The reach is how near a walker
    // has to come to be attacked: the core's aggro range for a creature of the walker's level (AggroYards times the
    // server's Rate.Creature.Aggro; a level more or less changes it by a yard), half of what the creature wanders, and
    // a few yards because nobody walks a line to the yard.
    // (It was 25 yd for every creature, with a falloff to 45 yd. On a server with an aggro rate of 0.5 a standing
    // creature reaches 10 yd: from Menethil Harbor every quest NPC of the Wetlands outside the town lay behind
    // "level-26 creatures on the way" for a level-24 character, and bots of 24 to 27 found no zone on their continent
    // with three quests they could get to: live round 51.)
    constexpr float AggroYards = 20.0f;
    constexpr float AggroRateMin = 0.25f, AggroRateMax = 1.5f;
    constexpr float ReachSlack = 4.0f;
    constexpr float WanderShare = 0.5f;
    constexpr float MaxWander = 20.0f;
    constexpr float DangerFalloff = 4.0f;
    constexpr float DangerFalloffLevels = 4.0f;
    // A creature with another one next to it does not come alone (the core's assistance, 10 yd). Since 2026-11-14 (the
    // maintainers: the risk model's verdict for two, not a flat bump) a pair is a group of two that the walk gate and
    // the route search judge with the walker's own fights (`GuildAdventure.Walks.GroupFrom`, 2); the old bump, each of
    // a pair two levels higher on the danger map, is `GuildAdventure.Roads.PairLevels` (0 now). Before:
    // each of a pair counts PairLevels higher. With a margin of three levels for a walk (Walks.MaxLevelsAbove, the owner's
    // ruling of 2026-11-14: a way is refused or walked around only for elites, creatures four or more levels above the
    // walker and groups it cannot take) a single creature three levels above the walker is passed, and a pair two
    // levels above it is not: two foes two levels up are a group a guild bot does not take. (The first test run with a
    // margin of two: a level-25 character's errand led past a level-27 and a level-26 gnoll that stand together, and
    // it died on the run from the two.)
    constexpr float PairYards = 12.0f;
    int32 PairLevels() { return GuildAdventure::GetSettings().roadsPairLevels; }
    // Cell size of the creature grid; at least the longest reach with its falloff (30 + 4 + 10 + 16 yd), so a 3x3
    // neighbourhood covers it.
    constexpr float CellSize = 64.0f;
    // Towns of the other side (RoadMap::InHostileTown).
    constexpr float TownCellYards = 24.0f;   // the grid the closed ground is kept in
    constexpr float TownFolkYards = 70.0f;   // its people stand this near to one another
    constexpr uint32 TownMinFolk = 3;        // and are at least this many
    constexpr float TownYards = 60.0f;       // the ground this far around each of them is closed
    // Creatures this close to one point of a way stand "on" it and come together; from CrowdMin of them a link costs
    // more for anyone they would still attack (not more than CrowdLevelsBelow under the walker).
    // (18 yd, was 14: the three Defias at the Alexston farm that killed a level-20 mage stand 8 yd apart, and the way
    // of the road map passes 13 yd from their middle: it saw two of them. A bot does not walk the line to the yard.)
    constexpr float CrowdRange = 18.0f;
    // A group counts for a walker from three creatures that are not more than CrowdLevelsBelow under it: "strong
    // camps" (the owner). Weaker ones a walker passes (their reach shrinks with every level, and a road leads past many);
    // counting them from twelve levels below sent walkers around murlocs half their level.
    uint8 CrowdMin() { return uint8(GuildAdventure::GetSettings().walksGroupFrom); }
    constexpr int32 CrowdLevelsBelow = 3;
    // A creature on patrol is on the danger map along its path, a point every so many yards. On a long round it is
    // seldom where the walker crosses its path: it counts a level less for every PatrolRound yards of its path, up to
    // PatrolMaxDiscount (a courier that walks through a whole zone does not close the zone).
    constexpr float PatrolStep = 20.0f;
    constexpr float PatrolRound = 500.0f;
    constexpr uint32 PatrolMaxDiscount = 6;
    // Elites count this many levels higher, bosses more. Nine (four until 2026-11-21, three until 2026-11-14): every
    // elite down to five levels below the walker reaches the four levels from which a way is walked around or refused
    // (the owner: [quote left out]; with
    // four, the elites of Tyr's Hand, levels 55 to 57, were no reason for a level-60 warrior's route to go around the
    // place, and its level-58 squad member died there to three of them, round 143).
    constexpr uint8 EliteLevels = 9;
    constexpr uint8 BossLevels = 10;
    // Nodes this close to the start or the destination are tried as the first or last node of a route.
    constexpr float NodeRadius = 800.0f;
    constexpr std::size_t MaxEndNodes = 8;
    // The walk to the first and from the last node is no known path: it counts this much longer.
    constexpr float OffRoadFactor = 1.3f;
    constexpr float OffRoadNear = 150.0f;
    // Two points of a link's path farther apart than this: the path has a hole.
    constexpr float MaxPointGap = 60.0f;
    // A node this close to the start or the destination needs no check of the walk between them.
    constexpr float LegTrusted = 8.0f;
    // How much more a node may lie above or below the walker than it lies away from it, as an end of a route.
    constexpr float SteepNodeYards = 10.0f;
    constexpr float OffRoadFarFactor = 3.0f;
    // Swimming is slow.
    constexpr float SwimFactor = 2.0f;
    // A link with this much sea in one piece is left out; one with at least the lower number counts it as a swim.
    constexpr float MaxSeaRun = 250.0f;
    constexpr float MinSeaRun = 40.0f;
    // A danger line is sampled this often.
    constexpr float DangerStep = 20.0f;
    // Every n-th point of a link's path is looked at for danger (the points lie about five yards apart).
    constexpr uint32 DangerPointStep = 3;

    bool IsOpenWorldMap(uint32 mapId)
    {
        return mapId == 0 || mapId == 1 || mapId == 530 || mapId == 571;
    }

    uint64 CellKey(uint32 mapId, float x, float y)
    {
        int32 const cx = int32(std::floor(x / CellSize)) + 2048;
        int32 const cy = int32(std::floor(y / CellSize)) + 2048;
        return (uint64(mapId) << 40) | (uint64(uint32(cx)) << 20) | uint64(uint32(cy));
    }

    uint64 TownKey(uint32 mapId, int32 cx, int32 cy)
    {
        return (uint64(mapId) << 40) | (uint64(uint32(cx + 8192)) << 20) | uint64(uint32(cy + 8192));
    }

    // How much longer a link counts for a walker `delta` levels below its strongest hostile creature: from
    // GuildAdventure.Roads.CostFromLevelsAbove (4) on, steeply. Below that a single creature costs nothing, and the
    // walker passes it as a player would (the owner, 2026-11-14: [quote left out]); elites count four levels higher and pairs two
    // higher, and groups of three cost by the crowd check. (From one level above until 2026-11-14, twice the length
    // at one, six times at two: the bots walked long ways around creatures a player passes.)
    float DangerFactor(int32 delta)
    {
        int32 const from = GuildAdventure::GetSettings().roadsCostFromLevelsAbove;
        if (delta < from || delta <= 0)
            return 1.0f;
        int32 const over = delta - from;
        if (over == 0)
            return 12.0f;
        if (over <= 2)
            return 20.0f;
        if (over <= 6)
            return 30.0f;
        return 60.0f;
    }

    float Dist2d(float x1, float y1, float x2, float y2)
    {
        return std::sqrt((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2));
    }

    // A creature that only swims attacks nobody who is not in the water (the core: Unit::isInAccessiblePlaceFor). A
    // Coral Shark (level 46 elite) patrols 19 to 42 yd below the pier of Theramore: on the danger map it stood "on the
    // way" from the flight master to the ship, and no character below level 41 was let onto the pier (2026-11-13: the
    // guild leader at 33 and three bots at 25, every one of their ways across the sea from there refused).
    // With a height the rule is: the sea lies at height 0 on every continent, and a lake's surface is not far above
    // what swims in it. A way without a height (a straight line the planner asks about) is looked up in the map's
    // terrain: the walker is in the water where the ground lies deeper under the surface than a character stands,
    // with what is built over the water (a pier, a low bridge) as ground. So a task that leads into the water still
    // meets the shark (the guild leader died to it under water on "Thresher Oil" at level 36).
    constexpr float SeaCreatureBelow = 0.5f;   // a swimmer below this height lives in the sea
    constexpr float LakeSurfaceAbove = 6.0f;   // a lake's surface above what swims in it, at most
    constexpr float WaterSlack = 1.5f;         // a point this far above the surface is out of the water
    constexpr float SwimDepth = 1.5f;          // water this deep is swum in
    constexpr float BuiltOverWater = 20.0f;    // a deck or bridge is looked for up to this far above the surface
    constexpr float DeckHeight = 3.0f;         // the way to a ship's node counts as this high above the sea at least
    constexpr float DeckNear = 45.0f;          // a walk that ends this near to a ship's node ends at the ship
    bool InDeepWater(uint32 mapId, float x, float y)
    {
        Map* map = sMapMgr->FindBaseMap(mapId);
        if (!map)
            return false;
        LiquidData const liquid = map->GetLiquidData(PHASEMASK_NORMAL, x, y, 1000.0f, DEFAULT_COLLISION_HEIGHT, {});
        if (liquid.Status == LIQUID_MAP_NO_WATER)
            return false;
        float const ground = std::max(liquid.DepthLevel, map->GetHeight(x, y, liquid.Level + BuiltOverWater, true, BuiltOverWater + 10.0f));
        return ground < liquid.Level - SwimDepth;
    }

    // Whether a creature that only swims counts for a point within its range. `deep` caches the terrain's answer for
    // the point (-1: not asked yet).
    bool SwimmerCounts(uint32 mapId, float hostileZ, float x, float y, float z, int8& deep)
    {
        if (z > RoadMap::NoHeight)
        {
            float const surface = hostileZ < SeaCreatureBelow ? 0.0f : hostileZ + LakeSurfaceAbove;
            return z <= surface + WaterSlack;
        }
        if (deep < 0)
            deep = InDeepWater(mapId, x, y) ? 1 : 0;
        return deep == 1;
    }
}

RoadMap& RoadMap::Instance()
{
    static RoadMap instance;
    return instance;
}

void RoadMap::IndexHostiles()
{
    _hostiles.clear();
    _patrolPoints = 0;
    _patrolWays.clear();
    _campPatrols.clear();
    FactionTemplateEntry const* alliance = sFactionTemplateStore.LookupEntry(1); // human player
    FactionTemplateEntry const* horde = sFactionTemplateStore.LookupEntry(2);    // orc player
    if (!alliance || !horde)
        return;

    float const aggro = AggroYards * std::clamp(sWorld->getRate(RATE_CREATURE_AGGRO), AggroRateMin, AggroRateMax);
    uint32 swimmers = 0;
    struct Folk
    {
        uint32 mapId;
        float x, y;
    };
    std::vector<Folk> folk[2]; // by the team they are hostile to: the other side's people
    // Who only stands there during a holiday is nobody's town: the fire keeper, revelers and scorchling of a Midsummer
    // fire are three "people" of one side within a few yards, and the two sides' fires of Gadgetzan and Everlook stand
    // in neutral towns (at Hammerfall they widened the closed ground by 90 yd to the west).
    std::unordered_set<uint32> seasonal;
    if (QueryResult result = WorldDatabase.Query("SELECT guid FROM game_event_creature WHERE eventEntry > 0"))
    {
        do
            seasonal.insert(result->Fetch()[0].Get<uint32>());
        while (result->NextRow());
    }
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (!IsOpenWorldMap(data.mapid) || !(data.phaseMask & PHASEMASK_NORMAL))
            continue;
        CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(data.id);
        // The other side's people, civilians included: who offers a service or stands guard and is hostile to one
        // team only. Where three of them stand together is one of its towns (below).
        if (cinfo && (cinfo->npcflag || (cinfo->flags_extra & CREATURE_FLAG_EXTRA_GUARD)) &&
            !(cinfo->flags_extra & CREATURE_FLAG_EXTRA_TRIGGER) && cinfo->type != CREATURE_TYPE_CRITTER && !seasonal.count(spawnId))
            if (FactionTemplateEntry const* side = sFactionTemplateStore.LookupEntry(cinfo->faction))
            {
                bool const toAlliance = side->IsHostileTo(*alliance), toHorde = side->IsHostileTo(*horde);
                if (toAlliance != toHorde)
                    folk[toAlliance ? 0 : 1].push_back({ data.mapid, data.posX, data.posY });
            }
        // Who only stands there during a holiday is no danger the rest of the year (the Flame Eaters of the Midsummer fires,
        // level 42-60): WorldIndex leaves such spawns out of everything else already. (The look-out still sees what stands
        // there while the holiday runs.)
        if (seasonal.count(spawnId))
            continue;
        // Civilians attack nobody: the flight master of the other side in a neutral town (Gringer in Booty Bay, level
        // 65 elite, made every way into the town "too dangerous" for the Alliance), festival folk.
        if (!cinfo || cinfo->type == CREATURE_TYPE_CRITTER || (cinfo->flags_extra & (CREATURE_FLAG_EXTRA_TRIGGER | CREATURE_FLAG_EXTRA_CIVILIAN)) ||
            (cinfo->unit_flags & (UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_IMMUNE_TO_PC)))
            continue;
        // Rares are seldom there, and nobody plans a way around a creature that may be: Mugglefin (level 23, counted
        // three higher like an elite) kept a level-21 bot from the corpse it had come to Lake Falathim for. Not a rare
        // elite: it is one spawn that is up until somebody kills it, and nobody does on this server (King Mosh, a level-60
        // devilsaur on a round through Un'Goro Crater, killed a level-60 shaman at the plants of its quest, round 143).
        if (cinfo->rank == CREATURE_ELITE_RARE)
            continue;
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(cinfo->faction);
        if (!faction)
            continue;

        Hostile hostile;
        hostile.teams = (faction->IsHostileTo(*alliance) ? 1 : 0) | (faction->IsHostileTo(*horde) ? 2 : 0);
        if (!hostile.teams)
            continue;
        uint32 level = cinfo->maxlevel;
        if (cinfo->rank == CREATURE_ELITE_WORLDBOSS)
            level += BossLevels;
        else if (cinfo->rank == CREATURE_ELITE_ELITE || cinfo->rank == CREATURE_ELITE_RAREELITE)
            level += EliteLevels;
        hostile.level = uint8(std::min<uint32>(level, 255));
        hostile.water = !cinfo->Movement.IsGroundAllowed() && cinfo->Movement.IsSwimAllowed() && !cinfo->Movement.IsFlightAllowed();
        if (hostile.water)
            ++swimmers;
        hostile.x = data.posX;
        hostile.y = data.posY;
        hostile.z = data.posZ;
        hostile.wander = data.movementType == RANDOM_MOTION_TYPE ? std::clamp(data.wander_distance, 0.0f, MaxWander) * WanderShare : 0.0f;
        hostile.reach = aggro + ReachSlack + hostile.wander;
        hostile.faction = cinfo->faction;
        _hostiles[CellKey(data.mapid, data.posX, data.posY)].push_back(hostile);

        // A creature on patrol is a danger wherever it walks, not only where it spawns (Emogg the Crusher walks from
        // the Mo'grosh camp down to the road and killed two bots there).
        if (data.movementType != WAYPOINT_MOTION_TYPE)
            continue;
        CreatureAddon const* addon = sObjectMgr->GetCreatureAddon(spawnId);
        WaypointPath const* path = addon && addon->path_id ? sWaypointMgr->GetPath(addon->path_id) : nullptr;
        if (!path)
            continue;
        float round = 0.0f;
        for (std::size_t i = 1; i < path->Nodes.size(); ++i)
            round += Dist2d(path->Nodes[i - 1].X, path->Nodes[i - 1].Y, path->Nodes[i].X, path->Nodes[i].Y);
        uint32 const discount = std::min<uint32>(PatrolMaxDiscount, uint32(round / PatrolRound));
        hostile.level = uint8(hostile.level > discount ? hostile.level - discount : 1);
        hostile.patrol = true;
        hostile.reach = aggro + ReachSlack;
        if (!hostile.water)
        {
            PatrolWay way;
            way.mapId = data.mapid;
            way.teams = hostile.teams;
            way.reach = hostile.reach;
            way.points.push_back({ data.posX, data.posY, data.posZ });
            for (WaypointNode const& node : path->Nodes)
                way.points.push_back({ node.X, node.Y, node.Z });
            _patrolWays.push_back(std::move(way));
        }
        float lastX = data.posX, lastY = data.posY, lastZ = data.posZ;
        for (WaypointNode const& node : path->Nodes)
        {
            float const segment = Dist2d(lastX, lastY, node.X, node.Y);
            if (segment < PatrolStep)
                continue;
            uint32 const steps = uint32(segment / PatrolStep);
            for (uint32 step = 1; step <= steps; ++step)
            {
                hostile.x = lastX + (node.X - lastX) * float(step) / float(steps);
                hostile.y = lastY + (node.Y - lastY) * float(step) / float(steps);
                hostile.z = lastZ + (node.Z - lastZ) * float(step) / float(steps);
                _hostiles[CellKey(data.mapid, hostile.x, hostile.y)].push_back(hostile);
                ++_patrolPoints;
            }
            lastX = node.X;
            lastY = node.Y;
            lastZ = node.Z;
        }
    }

    LOG_INFO("module", "mod-guild-adventure: road map: {} hostile creatures only swim and count for ways in the water only", swimmers);

    // Towns of the other side. Two bots died in one live window after a rescue had put them "back on the ground" at
    // the nearest road node: the Horde's flight master in Hammerfall (21 level-50 guardians) and the inn of Shadowprey
    // Village (19 of level 65). The guards' own reach on the danger map leaves gaps inside a town, and its people are
    // civilians the danger map does not know.
    uint32 townFolk[2] = { 0, 0 };
    for (int side = 0; side < 2; ++side)
    {
        _townCells[side].clear();
        std::unordered_map<uint64, std::vector<std::size_t>> byCell;
        for (std::size_t i = 0; i < folk[side].size(); ++i)
            byCell[CellKey(folk[side][i].mapId, folk[side][i].x, folk[side][i].y)].push_back(i);
        int const cells = int(std::ceil(TownFolkYards / CellSize));
        int const reach = int(std::ceil(TownYards / TownCellYards)) + 1;
        for (Folk const& one : folk[side])
        {
            uint32 together = 0;
            for (int dx = -cells; dx <= cells && together < TownMinFolk; ++dx)
                for (int dy = -cells; dy <= cells && together < TownMinFolk; ++dy)
                {
                    auto const cell = byCell.find(CellKey(one.mapId, one.x + float(dx) * CellSize, one.y + float(dy) * CellSize));
                    if (cell == byCell.end())
                        continue;
                    for (std::size_t other : cell->second)
                    {
                        float const ex = folk[side][other].x - one.x, ey = folk[side][other].y - one.y;
                        if (ex * ex + ey * ey <= TownFolkYards * TownFolkYards)
                            ++together; // itself included
                    }
                }
            if (together < TownMinFolk)
                continue;
            ++townFolk[side];
            int32 const cx = int32(std::floor(one.x / TownCellYards)), cy = int32(std::floor(one.y / TownCellYards));
            for (int dx = -reach; dx <= reach; ++dx)
                for (int dy = -reach; dy <= reach; ++dy)
                {
                    float const ex = (float(cx + dx) + 0.5f) * TownCellYards - one.x, ey = (float(cy + dy) + 0.5f) * TownCellYards - one.y;
                    if (ex * ex + ey * ey <= (TownYards + TownCellYards * 0.5f) * (TownYards + TownCellYards * 0.5f))
                        _townCells[side].insert(TownKey(one.mapId, cx + dx, cy + dy));
                }
        }
    }
    LOG_INFO("module", "mod-guild-adventure: road map: towns of the other side are closed ground: {} of the Horde's people in "
        "towns ({} cells closed for the Alliance), {} of the Alliance's ({} cells closed for the Horde)", townFolk[0],
        _townCells[0].size(), townFolk[1], _townCells[1].size());

    // Who stands with another (spawn places; patrols walk alone).
    for (auto& [key, cell] : _hostiles)
    {
        uint32 const mapId = uint32(key >> 40);
        for (Hostile& hostile : cell)
        {
            if (hostile.patrol)
                continue;
            for (int dx = -1; dx <= 1 && !hostile.paired; ++dx)
                for (int dy = -1; dy <= 1 && !hostile.paired; ++dy)
                {
                    auto const other = _hostiles.find(CellKey(mapId, hostile.x + float(dx) * CellSize, hostile.y + float(dy) * CellSize));
                    if (other == _hostiles.end())
                        continue;
                    for (Hostile const& neighbour : other->second)
                    {
                        if (&neighbour == &hostile || neighbour.patrol || !(neighbour.teams & hostile.teams))
                            continue;
                        float const ex = neighbour.x - hostile.x, ey = neighbour.y - hostile.y, ez = neighbour.z - hostile.z;
                        if (ex * ex + ey * ey + ez * ez <= PairYards * PairYards)
                        {
                            hostile.paired = true;
                            break;
                        }
                    }
                }
        }
    }
}

uint8 RoadMap::WeakestAround(uint32 mapId, float x, float y, float radius, TeamId team, uint32& count) const
{
    uint8 const bit = team == TEAM_ALLIANCE ? 1 : 2;
    int const reach = int(std::ceil(radius / CellSize));
    uint8 weakest = 0;
    count = 0;
    for (int dx = -reach; dx <= reach; ++dx)
        for (int dy = -reach; dy <= reach; ++dy)
        {
            auto cell = _hostiles.find(CellKey(mapId, x + float(dx) * CellSize, y + float(dy) * CellSize));
            if (cell == _hostiles.end())
                continue;
            for (Hostile const& hostile : cell->second)
            {
                if (hostile.patrol || hostile.water || !(hostile.teams & bit))
                    continue;
                float const ex = hostile.x - x, ey = hostile.y - y;
                if (ex * ex + ey * ey > radius * radius)
                    continue;
                ++count;
                if (!weakest || hostile.level < weakest)
                    weakest = hostile.level;
            }
        }
    return weakest;
}

uint8 RoadMap::CrowdAt(uint32 mapId, float x, float y, float z, TeamId team, uint8& level) const
{
    uint8 const bit = team == TEAM_ALLIANCE ? 1 : 2;
    uint32 count = 0;
    int8 deep = -1;
    level = 0;
    // The group comes when one of it does: someone at the point has to be within the reach of one of them. (With the
    // server's aggro rate at 0.5 a standing creature reaches 14 yd with the slack; the group around it, CrowdRange,
    // joins through the core's assistance.)
    bool touched = false;
    std::vector<Hostile const*> members; // those hostile to one side only, for the chain below
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
        {
            auto cell = _hostiles.find(CellKey(mapId, x + float(dx) * CellSize, y + float(dy) * CellSize));
            if (cell == _hostiles.end())
                continue;
            for (Hostile const& hostile : cell->second)
            {
                if (hostile.patrol || !(hostile.teams & bit))
                    continue;
                float const ex = hostile.x - x, ey = hostile.y - y;
                float const ez = z > NoHeight ? hostile.z - z : 0.0f;
                float const d2 = ex * ex + ey * ey + ez * ez;
                // (A creature that walks about meets the others that far from its spawn: the Scorpid Terrors of the
                // Shimmering Flats stand 22 to 40 yd apart and wander 30 yd, so they come two at a time; no group was
                // counted on the way through them, and a level-31 druid died to two of level 34 there, 2026-11-25.)
                float const range = CrowdRange + hostile.wander;
                if (d2 > range * range || (hostile.water && !SwimmerCounts(mapId, hostile.z, x, y, z, deep)))
                    continue;
                ++count;
                level = std::max(level, hostile.level);
                touched = touched || d2 <= hostile.reach * hostile.reach;
                if (hostile.teams == bit)
                    members.push_back(&hostile);
            }
        }
    if (!touched)
    {
        level = 0;
        return 0;
    }
    // The soldiers of one side call one another: a creature in a fight calls those of its kind within 10 yd, and again
    // every few seconds from wherever it is (Creature::CallAssistance, CreatureFamilyAssistancePeriod), so one of a post
    // brings the post. Seventeen Theramore Sentries stand around Sentry Point, a few yards apart; the way past it touched
    // three of them, and a level-40 druid walking by had them on it (2026-11-26). Only creatures hostile to one side (the
    // other side's soldiers and guards, who stand in posts); camps of creatures hostile to everyone are left as they are.
    if (!members.empty())
    {
        constexpr float ChainYards = 12.0f; // the core's assistance radius with a little slack
        constexpr float ChainHeight = 8.0f;
        constexpr std::size_t ChainMax = 40;
        auto friends = [](Hostile const& a, Hostile const& b)
        {
            if (a.faction == b.faction)
                return true;
            FactionTemplateEntry const* fa = sFactionTemplateStore.LookupEntry(a.faction);
            FactionTemplateEntry const* fb = sFactionTemplateStore.LookupEntry(b.faction);
            return fa && fb && fa->IsFriendlyTo(*fb);
        };
        std::vector<Hostile const*> chain = members;
        for (std::size_t i = 0; i < chain.size() && chain.size() < ChainMax; ++i)
        {
            Hostile const& one = *chain[i];
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                {
                    auto cell = _hostiles.find(CellKey(mapId, one.x + float(dx) * CellSize, one.y + float(dy) * CellSize));
                    if (cell == _hostiles.end())
                        continue;
                    for (Hostile const& other : cell->second)
                    {
                        if (other.patrol || other.water || other.teams != bit || chain.size() >= ChainMax)
                            continue;
                        float const ex = other.x - one.x, ey = other.y - one.y;
                        if (ex * ex + ey * ey > ChainYards * ChainYards || std::fabs(other.z - one.z) > ChainHeight)
                            continue;
                        if (std::find(chain.begin(), chain.end(), &other) != chain.end() || !friends(one, other))
                            continue;
                        chain.push_back(&other);
                        ++count;
                        level = std::max(level, other.level);
                    }
                }
        }
    }
    return uint8(std::min<uint32>(count, 255));
}

bool RoadMap::GroupCounts(uint8 count, uint8 crowdLevel, int32 walkerLevel)
{
    constexpr uint8 FarBelowFrom = 4;
    constexpr int32 FarBelowLevels = 8; // Escape::GroupLevelsBelow
    if (count < CrowdMin())
        return false;
    return int32(crowdLevel) >= walkerLevel - CrowdLevelsBelow ||
        (count >= FarBelowFrom && int32(crowdLevel) + FarBelowLevels >= walkerLevel);
}

uint8 RoadMap::CrowdAlong(uint32 mapId, float fromX, float fromY, float toX, float toY, TeamId team, uint8& level, float fromZ,
    float toZ, float skipStart) const
{
    bool const heights = fromZ > NoHeight && toZ > NoHeight;
    float const length = Dist2d(fromX, fromY, toX, toY);
    uint32 const steps = std::max<uint32>(1, uint32(length / (CrowdRange / 2.0f)));
    uint8 crowd = 0;
    level = 0;
    for (uint32 step = 0; step <= steps; ++step)
    {
        float const t = float(step) / float(steps);
        if (skipStart > 0.0f && t * length < skipStart)
            continue;
        uint8 here = 0;
        uint8 const count = CrowdAt(mapId, fromX + (toX - fromX) * t, fromY + (toY - fromY) * t,
            heights ? fromZ + (toZ - fromZ) * t : NoHeight, team, here);
        if (count > crowd)
        {
            crowd = count;
            level = here;
        }
    }
    return crowd;
}

bool RoadMap::InHostileTown(uint32 mapId, float x, float y, TeamId team) const
{
    std::unordered_set<uint64> const& cells = _townCells[team == TEAM_ALLIANCE ? 0 : 1];
    return !cells.empty() &&
        cells.count(TownKey(mapId, int32(std::floor(x / TownCellYards)), int32(std::floor(y / TownCellYards)))) != 0;
}

std::string RoadMap::DangerName(uint8 danger)
{
    return danger >= TownDanger ? std::string("a town of the other side") : Acore::StringFormat("level-{} creatures", uint32(danger));
}

uint8 RoadMap::StrongestNear(uint32 mapId, float x, float y, float z, TeamId team, float radius) const
{
    uint8 const bit = team == TEAM_ALLIANCE ? 1 : 2;
    int const cells = int(std::ceil(radius / CellSize));
    uint8 best = 0;
    for (int dx = -cells; dx <= cells; ++dx)
        for (int dy = -cells; dy <= cells; ++dy)
        {
            auto const cell = _hostiles.find(CellKey(mapId, x + float(dx) * CellSize, y + float(dy) * CellSize));
            if (cell == _hostiles.end())
                continue;
            for (Hostile const& hostile : cell->second)
                if ((hostile.teams & bit) && !hostile.water && hostile.level > best && std::fabs(hostile.z - z) <= 30.0f &&
                    Dist2d(hostile.x, hostile.y, x, y) <= radius)
                    best = hostile.level;
        }
    return best;
}

uint8 RoadMap::GuardAt(uint32 mapId, float x, float y, float z, TeamId team) const
{
    if (InHostileTown(mapId, x, y, team))
        return TownDanger;
    uint8 const bit = team == TEAM_ALLIANCE ? 1 : 2;
    uint8 guard = 0;
    int8 deep = -1;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
        {
            auto cell = _hostiles.find(CellKey(mapId, x + float(dx) * CellSize, y + float(dy) * CellSize));
            if (cell == _hostiles.end())
                continue;
            for (Hostile const& hostile : cell->second)
            {
                if (hostile.patrol || !(hostile.teams & bit) || hostile.level <= guard)
                    continue;
                float const ex = hostile.x - x, ey = hostile.y - y, ez = hostile.z - z;
                if (ex * ex + ey * ey + ez * ez > hostile.reach * hostile.reach ||
                    (hostile.water && !SwimmerCounts(mapId, hostile.z, x, y, z, deep)))
                    continue;
                guard = hostile.level;
            }
        }
    return guard;
}

bool RoadMap::IsNearTransportNode(uint32 mapId, float x, float y, float yards) const
{
    for (Node const& node : _nodes)
        if (node.transport && node.mapId == mapId && Dist2d(x, y, node.x, node.y) <= yards)
            return true;
    return false;
}

uint8 RoadMap::NodeGuard(uint32 mapId, float x, float y, TeamId team) const
{
    if (team != TEAM_ALLIANCE && team != TEAM_HORDE)
        return 0;
    for (Node const& node : _nodes)
        if (node.mapId == mapId && Dist2d(x, y, node.x, node.y) <= 5.0f)
            return node.guard[team];
    return 0;
}

uint8 RoadMap::DangerAt(uint32 mapId, float x, float y, TeamId team, float z, float waterZ) const
{
    if (InHostileTown(mapId, x, y, team))
        return TownDanger;
    uint8 const bit = team == TEAM_ALLIANCE ? 1 : 2;
    uint8 danger = 0;
    int8 deep = -1;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
        {
            auto cell = _hostiles.find(CellKey(mapId, x + float(dx) * CellSize, y + float(dy) * CellSize));
            if (cell == _hostiles.end())
                continue;
            for (Hostile const& hostile : cell->second)
            {
                int32 level = int32(hostile.level) + (hostile.paired ? PairLevels() : 0);
                if (!(hostile.teams & bit) || level <= int32(danger))
                    continue;
                float const ex = hostile.x - x, ey = hostile.y - y;
                float const ez = z > NoHeight ? hostile.z - z : 0.0f;
                float const d2 = ex * ex + ey * ey + ez * ez;
                float const range = hostile.reach + DangerFalloff * DangerFalloffLevels;
                if (d2 > range * range || (hostile.water && !SwimmerCounts(mapId, hostile.z, x, y, z > NoHeight ? z : waterZ, deep)))
                    continue;
                if (d2 > hostile.reach * hostile.reach)
                    level -= int32(std::ceil((std::sqrt(d2) - hostile.reach) / DangerFalloff));
                if (level > int32(danger))
                    danger = uint8(std::min<int32>(level, 255));
            }
        }
    return danger;
}

uint8 RoadMap::DangerAlong(uint32 mapId, float fromX, float fromY, float toX, float toY, TeamId team, float stopBefore, float fromZ,
    float toZ, bool swimmersByHeight) const
{
    bool const heights = fromZ > NoHeight && toZ > NoHeight;
    if (stopBefore > 0.0f)
    {
        float const whole = Dist2d(fromX, fromY, toX, toY);
        if (whole <= stopBefore)
            return 0;
        toX = fromX + (toX - fromX) * (whole - stopBefore) / whole;
        toY = fromY + (toY - fromY) * (whole - stopBefore) / whole;
        if (heights)
            toZ = fromZ + (toZ - fromZ) * (whole - stopBefore) / whole;
    }
    float const length = Dist2d(fromX, fromY, toX, toY);
    uint32 const steps = std::max<uint32>(1, uint32(length / DangerStep));
    uint8 danger = 0;
    for (uint32 step = 0; step <= steps; ++step)
    {
        float const t = float(step) / float(steps);
        // (The line's own height is a guess between its ends; twice the range of a point on a link's path.)
        float const z = heights ? fromZ + (toZ - fromZ) * t : NoHeight;
        danger = std::max(danger, DangerAt(mapId, fromX + (toX - fromX) * t, fromY + (toY - fromY) * t, team,
            swimmersByHeight ? NoHeight : z, swimmersByHeight ? z : NoHeight));
    }
    return danger;
}

void RoadMap::Load()
{
    uint32 const start = getMSTime();
    _nodes.clear();
    _links.clear();
    _points.clear();
    IndexHostiles();

    std::unordered_map<uint32, uint32> nodeIndex; // node id -> index
    if (QueryResult result = PlayerbotsDatabase.Query("SELECT id, name, map_id, x, y, z FROM playerbots_travelnode"))
    {
        do
        {
            Field* fields = result->Fetch();
            Node node;
            node.id = fields[0].Get<uint32>();
            node.name = fields[1].Get<std::string>();
            // Nodes at instance portals ("Stormwind Stockade entrance" and "... exit": down the stairs, where the navmesh
            // of the world ends) and at the far side of teleporters are nodes of playerbots' teleport links, not places
            // to walk over.
            node.blocked = node.name.ends_with("entrance") || node.name.ends_with("exit") || node.name.find("portal") != std::string::npos;
            node.transport = node.name.starts_with("Ship (") || node.name.starts_with("Zeppelin (");
            node.mapId = fields[2].Get<uint32>();
            node.x = fields[3].Get<float>();
            node.y = fields[4].Get<float>();
            node.z = fields[5].Get<float>();
            if (!IsOpenWorldMap(node.mapId))
                continue;
            nodeIndex[node.id] = uint32(_nodes.size());
            _nodes.push_back(std::move(node));
        } while (result->NextRow());
    }

    // What stands on each node (GuardCounts).
    uint32 guarded = 0;
    for (Node& node : _nodes)
    {
        node.guard[TEAM_ALLIANCE] = GuardAt(node.mapId, node.x, node.y, node.z, TEAM_ALLIANCE);
        node.guard[TEAM_HORDE] = GuardAt(node.mapId, node.x, node.y, node.z, TEAM_HORDE);
        if (node.guard[TEAM_ALLIANCE] || node.guard[TEAM_HORDE])
            ++guarded;
    }
    LOG_INFO("module", "mod-guild-adventure: road map: a hostile creature stands on {} of {} nodes (no places to send or put a bot to)",
        guarded, _nodes.size());
    for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
    {
        std::string names;
        uint32 count = 0;
        for (Node const& node : _nodes)
            if (!node.blocked && InHostileTown(node.mapId, node.x, node.y, team))
            {
                if (++count <= 400)
                    names += (names.empty() ? "" : ", ") + node.name;
            }
        LOG_INFO("module", "mod-guild-adventure: road map: {} nodes lie in towns of the other side for the {}: {}", count,
            team == TEAM_ALLIANCE ? "Alliance" : "Horde", names);
    }

    // The camps nodes lie in and links lead through, as the risk model sees them (Risk::CampAtPoint: several
    // creatures of a place close together and around the point; beasts make none). mod-playerbots recorded named
    // places, and many named places are camps: "Redridge Mountains Stonewatch Keep" in the Blackrock camp, "Badlands
    // The Makers Terrace" in the Shadowforge dig, the Venture Co. Operations Center, the Mosshide Fen.
    constexpr float CampStepYards = 12.0f;
    // A road this far beside the middle of a camp leads through it: the camp's creatures reach it, its patrols cross it.
    // (The road from Booty Bay to the Nekmani Wellspring runs 30 to 40 yd past a Bloodsail camp of eight; at the road
    // itself the camp check saw no camp, and a warrior of 44 had five of them on it within seconds and died.)
    constexpr float CampSideYards = 20.0f;
    uint32 campNodes = 0, campLinks = 0;
    std::chrono::steady_clock::duration campTime{};
    auto noteCamp = [](Risk::CampAt const& camp, uint8& walker, uint8& count, uint8& campLevel)
    {
        if (!camp.inside || camp.walkerLevel <= walker)
            return false;
        walker = camp.walkerLevel;
        count = uint8(std::min<uint16>(camp.count, 255));
        campLevel = camp.level;
        return true;
    };

    // A creature on patrol whose way runs through the middle of a camp carries the camp along its way: where the way
    // leads out of it, onto a road or beside one, the camp's creatures follow what the patrol pulls (a Bloodsail mage
    // walks from the middle of its camp 25 yd beyond the camp's tents, a level-44 hunter met it there and had five of
    // the camp on it). Its points every CampStepYards, with the camp it walks through, up to CampCarryYards beyond the
    // camp's farthest creature (a courier whose round leads through a camp and on for a mile carries it no farther).
    constexpr float CampCarryYards = 40.0f;
    uint32 campPatrols = 0, campPatrolPoints = 0;
    {
        auto const campFrom = std::chrono::steady_clock::now();
        for (PatrolWay const& way : _patrolWays)
            for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
            {
                if (!(way.teams & (team == TEAM_ALLIANCE ? 1 : 2)))
                    continue;
                Risk::CampAt best;
                for (RoutePoint const& point : way.points)
                    if (Risk::CampAt const camp = Risk::CampAtPoint(way.mapId, point.x, point.y, team, point.z);
                        camp.inside && camp.walkerLevel > best.walkerLevel)
                        best = camp;
                if (!best.inside)
                    continue;
                ++campPatrols;
                for (std::size_t i = 0; i < way.points.size(); ++i)
                {
                    RoutePoint const& point = way.points[i];
                    RoutePoint const& next = way.points[std::min(i + 1, way.points.size() - 1)];
                    int const steps = std::max(1, int(Dist2d(point.x, point.y, next.x, next.y) / CampStepYards));
                    for (int s = 0; s < steps; ++s)
                    {
                        float const t = float(s) / float(steps);
                        CampPatrol patrol;
                        patrol.x = point.x + (next.x - point.x) * t;
                        patrol.y = point.y + (next.y - point.y) * t;
                        patrol.z = point.z + (next.z - point.z) * t;
                        if (Dist2d(patrol.x, patrol.y, best.x, best.y) > best.radius + CampCarryYards)
                            continue;
                        patrol.reach = way.reach;
                        patrol.team = uint8(team);
                        patrol.walker = best.walkerLevel;
                        patrol.count = uint8(std::min<uint16>(best.count, 255));
                        patrol.level = best.level;
                        patrol.campX = best.x;
                        patrol.campY = best.y;
                        _campPatrols[CellKey(way.mapId, patrol.x, patrol.y)].push_back(patrol);
                        ++campPatrolPoints;
                    }
                }
            }
        campTime += std::chrono::steady_clock::now() - campFrom;
    }
    // The camp a patrol carries to a point, as Risk::CampAt (the strongest; its middle in x, y).
    auto const campByPatrol = [this](uint32 mapId, float x, float y, float z, TeamId team)
    {
        Risk::CampAt camp;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
            {
                auto const cell = _campPatrols.find(CellKey(mapId, x + float(dx) * CellSize, y + float(dy) * CellSize));
                if (cell == _campPatrols.end())
                    continue;
                for (CampPatrol const& patrol : cell->second)
                    if (patrol.team == uint8(team) && patrol.walker > camp.walkerLevel && std::fabs(patrol.z - z) < 15.0f &&
                        Dist2d(patrol.x, patrol.y, x, y) <= patrol.reach)
                    {
                        camp.inside = true;
                        camp.walkerLevel = patrol.walker;
                        camp.count = patrol.count;
                        camp.level = patrol.level;
                        camp.x = patrol.campX;
                        camp.y = patrol.campY;
                    }
            }
        return camp;
    };

    // Walking links the graph has but no character can walk (world table ga_road_unlink, tools/gen_road_links.py): their
    // walked path jumps up or down where the navmesh has no walk of reasonable length (the links of the Royal Quarter of
    // Undercity drop 90 yd to the surface of Tirisfal; the walk between their nodes is in ga_road_link when there is one).
    std::unordered_set<uint64> unlinked;
    if (QueryResult result = WorldDatabase.Query("SELECT from_node, to_node FROM ga_road_unlink"))
    {
        do
        {
            Field* fields = result->Fetch();
            unlinked.insert((uint64(fields[0].Get<uint32>()) << 32) | fields[1].Get<uint32>());
        } while (result->NextRow());
    }

    // Nodes a bot could not walk to earlier (ga_road_block).
    uint32 remembered = 0;
    std::vector<std::size_t> fromTable;
    if (QueryResult result = WorldDatabase.Query("SELECT node_id FROM ga_road_block"))
    {
        do
        {
            auto itr = nodeIndex.find(result->Fetch()[0].Get<uint32>());
            if (itr != nodeIndex.end() && !_nodes[itr->second].blocked)
            {
                _nodes[itr->second].blocked = true;
                fromTable.push_back(itr->second);
                ++remembered;
            }
        } while (result->NextRow());
    }
    // Blocks of a lift's stops (nodes one above the other at one spot) were made by distance on the map alone until
    // 2026-11-14: one refused step to the Searing Gorge's rim took the stop on the Cauldron's floor out too, its only way
    // out, and every bot down there was "cut off". They are given back; a stop that is no way on the ground is blocked
    // again by height (BlockNodesNear).
    uint32 lifted = 0;
    for (std::size_t index : fromTable)
    {
        Node& node = _nodes[index];
        bool stack = false;
        for (Node const& other : _nodes)
            if (&other != &node && other.mapId == node.mapId && Dist2d(other.x, other.y, node.x, node.y) <= 3.0f &&
                std::fabs(other.z - node.z) > 8.0f)
            {
                stack = true;
                break;
            }
        if (!stack)
            continue;
        node.blocked = false;
        --remembered;
        ++lifted;
        WorldDatabase.Execute("DELETE FROM ga_road_block WHERE node_id = {}", node.id);
    }
    if (lifted)
        LOG_INFO("module", "mod-guild-adventure: road map: {} blocked stops of lifts given back (blocked by distance alone)", lifted);
    // Blocks next to a link that is left out: a bot that followed the link's jump had its step refused there, and the
    // block took the nodes around the step out (Ashmire on the drop from Cold Hearth Manor to the Royal Quarter of
    // Undercity, 2026-11-24: "out of the road map: Varimathras, Lady Sylvanas Windrunner", the only walking way out of
    // the city for a level-11 Forsaken, the other one leading through the Scarlet camp). With the link gone the cause is
    // gone; a node that is no way on the ground is blocked again when a bot is refused at it.
    uint32 freed = 0;
    if (!unlinked.empty())
    {
        std::vector<std::size_t> jumpEnds;
        for (uint64 key : unlinked)
            for (uint32 id : { uint32(key >> 32), uint32(key & 0xFFFFFFFF) })
                if (auto itr = nodeIndex.find(id); itr != nodeIndex.end())
                    jumpEnds.push_back(itr->second);
        constexpr float JumpEndYards = 60.0f, JumpEndHeight = 15.0f;
        for (std::size_t index : fromTable)
        {
            Node& node = _nodes[index];
            if (!node.blocked)
                continue;
            bool const nearEnd = std::any_of(jumpEnds.begin(), jumpEnds.end(), [&](std::size_t end)
            {
                Node const& other = _nodes[end];
                return other.mapId == node.mapId && Dist2d(other.x, other.y, node.x, node.y) <= JumpEndYards &&
                    std::fabs(other.z - node.z) <= JumpEndHeight;
            });
            if (!nearEnd)
                continue;
            node.blocked = false;
            --remembered;
            ++freed;
            WorldDatabase.Execute("DELETE FROM ga_road_block WHERE node_id = {}", node.id);
        }
    }
    if (freed)
        LOG_INFO("module", "mod-guild-adventure: road map: {} blocked nodes beside links that are left out given back", freed);

    // The upper stops of lifts: two nodes one above the other that a ride with an object joins (the links mod-playerbots
    // rides; they are no walks and are not loaded).
    uint32 liftTops = 0;
    if (QueryResult result = PlayerbotsDatabase.Query("SELECT node_id, to_node_id FROM playerbots_travelnode_link WHERE object <> 0"))
    {
        do
        {
            Field* fields = result->Fetch();
            auto from = nodeIndex.find(fields[0].Get<uint32>());
            auto to = nodeIndex.find(fields[1].Get<uint32>());
            if (from == nodeIndex.end() || to == nodeIndex.end())
                continue;
            Node& a = _nodes[from->second];
            Node& b = _nodes[to->second];
            if (a.mapId != b.mapId || Dist2d(a.x, a.y, b.x, b.y) > 3.0f || std::fabs(a.z - b.z) <= 5.0f)
                continue;
            Node& upper = a.z > b.z ? a : b;
            if (!upper.liftTop)
            {
                upper.liftTop = true;
                ++liftTops;
            }
        } while (result->NextRow());
    }
    if (liftTops)
        LOG_INFO("module", "mod-guild-adventure: road map: {} upper stops of lifts are no ends of routes and no places to put a bot", liftTops);

    // Rides on lifts: the transport links (type 3) of the graph whose object is a static transport (an elevator) and
    // whose two stops stand one above the other. Thunder Bluff and Freewind Post stand on mesas only lifts lead up to,
    // and a Tauren recruit at the foot of Thunder Bluff had its hand-in at Hunter Rise refused ("no route of the road
    // map for 191 yd"); NewRpg does not ride them either. A ride costs its height and the wait for the platform; the
    // order planner boards and leaves the platform itself (OrderService::StepLift). (Not the doors of Undercity's lifts,
    // which the graph also joins by their object: they move a few yards.)
    constexpr float LiftStopYards = 10.0f, LiftMinRise = 12.0f, LiftWaitYards = 60.0f;
    uint32 lifts = 0;
    if (QueryResult result = PlayerbotsDatabase.Query("SELECT node_id, to_node_id, object FROM playerbots_travelnode_link "
        "WHERE type = 3 AND object <> 0"))
    {
        do
        {
            Field* fields = result->Fetch();
            if (unlinked.count((uint64(fields[0].Get<uint32>()) << 32) | fields[1].Get<uint32>()))
                continue;
            auto from = nodeIndex.find(fields[0].Get<uint32>());
            auto to = nodeIndex.find(fields[1].Get<uint32>());
            GameObjectTemplate const* lift = sObjectMgr->GetGameObjectTemplate(fields[2].Get<uint32>());
            if (from == nodeIndex.end() || to == nodeIndex.end() || !lift || lift->type != GAMEOBJECT_TYPE_TRANSPORT)
                continue;
            Node const& a = _nodes[from->second];
            Node const& b = _nodes[to->second];
            if (a.mapId != b.mapId || Dist2d(a.x, a.y, b.x, b.y) > LiftStopYards || std::fabs(a.z - b.z) < LiftMinRise)
                continue;
            _nodes[from->second].liftStop = true;
            _nodes[to->second].liftStop = true;
            Link link;
            link.from = from->second;
            link.to = to->second;
            link.distance = std::fabs(a.z - b.z) + LiftWaitYards;
            link.lift = lift->entry;
            link.firstPoint = uint32(_points.size());
            link.pointCount = 2;
            _points.push_back({ a.x, a.y, a.z });
            _points.push_back({ b.x, b.y, b.z });
            _links.push_back(link);
            ++lifts;
        } while (result->NextRow());
    }
    if (lifts)
        LOG_INFO("module", "mod-guild-adventure: road map: {} rides on lifts between stops one above the other", lifts);

    // Walking links (type 1) between two nodes of the same open-world map.
    std::unordered_map<uint64, uint32> linkIndex; // (from id, to id) -> index
    uint32 leftOut = 0;
    if (QueryResult result = PlayerbotsDatabase.Query("SELECT node_id, to_node_id, distance, swim_distance FROM "
        "playerbots_travelnode_link WHERE type = 1"))
    {
        do
        {
            Field* fields = result->Fetch();
            if (unlinked.count((uint64(fields[0].Get<uint32>()) << 32) | fields[1].Get<uint32>()))
            {
                ++leftOut;
                continue;
            }
            auto from = nodeIndex.find(fields[0].Get<uint32>());
            auto to = nodeIndex.find(fields[1].Get<uint32>());
            if (from == nodeIndex.end() || to == nodeIndex.end() || _nodes[from->second].mapId != _nodes[to->second].mapId)
                continue;
            Link link;
            link.from = from->second;
            link.to = to->second;
            link.distance = fields[2].Get<float>();
            link.swim = fields[3].Get<float>();
            linkIndex[(uint64(fields[0].Get<uint32>()) << 32) | fields[1].Get<uint32>()] = uint32(_links.size());
            _links.push_back(link);
        } while (result->NextRow());
    }

    // The walked path of each link; the rows of a link follow each other in order.
    if (QueryResult result = PlayerbotsDatabase.Query("SELECT node_id, to_node_id, x, y, z FROM playerbots_travelnode_path "
        "ORDER BY node_id, to_node_id, nr"))
    {
        uint64 currentKey = 0;
        Link* current = nullptr;
        do
        {
            Field* fields = result->Fetch();
            uint64 const key = (uint64(fields[0].Get<uint32>()) << 32) | fields[1].Get<uint32>();
            if (key != currentKey || !current)
            {
                currentKey = key;
                auto itr = linkIndex.find(key);
                current = itr != linkIndex.end() ? &_links[itr->second] : nullptr;
                if (current)
                    current->firstPoint = uint32(_points.size());
            }
            if (!current)
                continue;
            _points.push_back({ fields[2].Get<float>(), fields[3].Get<float>(), fields[4].Get<float>() });
            ++current->pointCount;
        } while (result->NextRow());
    }

    // Walking links the graph lacks, found on the navmesh (world table ga_road_link, tools/gen_road_links.py): parts of
    // the graph that no walking link joins to the rest of their map. The start mesa of the Tauren has links among its
    // seven nodes and none down to the plains, so no route led off it, and the path finder cannot see the way down
    // either while the plains' grid is empty: two recruits of a test guild stood on the mesa's rim with every walk to
    // Bloodhoof Village refused until the guild left the village's quest givers alone for a day (2026-11-24).
    // Each row is a walk in both directions.
    uint32 navmeshLinks = 0;
    if (QueryResult result = WorldDatabase.Query("SELECT from_node, to_node, points FROM ga_road_link"))
    {
        do
        {
            Field* fields = result->Fetch();
            auto from = nodeIndex.find(fields[0].Get<uint32>());
            auto to = nodeIndex.find(fields[1].Get<uint32>());
            if (from == nodeIndex.end() || to == nodeIndex.end() || _nodes[from->second].mapId != _nodes[to->second].mapId)
                continue;
            std::vector<RoutePoint> walk;
            std::istringstream points(fields[2].Get<std::string>());
            std::string point;
            while (std::getline(points, point, ';'))
            {
                RoutePoint p;
                if (std::sscanf(point.c_str(), "%f %f %f", &p.x, &p.y, &p.z) == 3)
                    walk.push_back(p);
            }
            if (walk.size() < 2)
                continue;
            float length = 0.0f;
            for (std::size_t i = 1; i < walk.size(); ++i)
                length += Dist2d(walk[i - 1].x, walk[i - 1].y, walk[i].x, walk[i].y);
            for (bool back : { false, true })
            {
                Link link;
                link.from = back ? to->second : from->second;
                link.to = back ? from->second : to->second;
                link.distance = length;
                link.firstPoint = uint32(_points.size());
                link.pointCount = uint32(walk.size());
                if (back)
                    _points.insert(_points.end(), walk.rbegin(), walk.rend());
                else
                    _points.insert(_points.end(), walk.begin(), walk.end());
                _links.push_back(link);
            }
            ++navmeshLinks;
        } while (result->NextRow());
    }
    if (navmeshLinks)
        LOG_INFO("module", "mod-guild-adventure: road map: {} walks the travel node graph lacks, from the navmesh (ga_road_link)",
            navmeshLinks);
    if (leftOut)
        LOG_INFO("module", "mod-guild-adventure: road map: {} links of the travel node graph left out: their walked path jumps "
            "where no character can walk (ga_road_unlink)", leftOut);

    // Links without a path are of no use; the rest get their danger and join their node.
    uint32 usable = 0, broken = 0, sea = 0, swims = 0;
    for (uint32 i = 0; i < _links.size(); ++i)
    {
        Link& link = _links[i];
        if (link.pointCount < 2)
            continue;
        // A link over the open sea: mod-playerbots walked its links on a navmesh that has the water's surface, and
        // wrote no swim distance for most (the link from Stormwind's harbour pier to the Jansen Stead in Westfall is
        // 950 yd of sea; a level-22 druid took it and died of fatigue 180 yd out). The sea of the continents lies at
        // height 0, and a walked path on it is flat: the longest run of points at that height that stay within a hand
        // of each other is a swim. A short one is one (the Echo Isles, Fray Island); a long one is no way at all.
        {
            constexpr float SeaLow = -0.6f, SeaHigh = 1.4f, SeaFlat = 0.45f;
            float run = 0.0f, best = 0.0f, low = 0.0f, high = 0.0f;
            bool in = false;
            for (uint32 p = 0; p < link.pointCount; ++p)
            {
                RoutePoint const& point = _points[link.firstPoint + p];
                if (point.z < SeaLow || point.z > SeaHigh)
                {
                    in = false;
                    continue;
                }
                if (!in || std::max(high, point.z) - std::min(low, point.z) > SeaFlat)
                {
                    in = true;
                    low = high = point.z;
                    run = 0.0f;
                    continue;
                }
                low = std::min(low, point.z);
                high = std::max(high, point.z);
                RoutePoint const& before = _points[link.firstPoint + p - 1];
                run += Dist2d(before.x, before.y, point.x, point.y);
                best = std::max(best, run);
            }
            if (best > MaxSeaRun)
            {
                ++sea;
                continue;
            }
            if (best > link.swim + MinSeaRun)
            {
                link.swim = best;
                ++swims;
            }
        }
        // A walked path has its points a few yards apart. One with a hole (from a node straight to a point 280 yd on,
        // seen above Loch Modan) is no path: nobody knows what lies in the hole.
        bool holes = false;
        for (uint32 p = 1; p < link.pointCount && !holes; ++p)
        {
            RoutePoint const& a = _points[link.firstPoint + p - 1];
            RoutePoint const& b = _points[link.firstPoint + p];
            holes = Dist2d(a.x, a.y, b.x, b.y) > MaxPointGap;
        }
        if (holes)
        {
            ++broken;
            continue;
        }
        uint32 const mapId = _nodes[link.from].mapId;
        // (A link to a ship's node ends in the water beside the pier, at the water's height: its points count as on
        // the deck, so what only swims below does not. The Coral Sharks under Theramore's pier closed the link from
        // the flight master to The Lady Mehley for every bot below level 45, and a bot that had sailed there stood.)
        bool const toDeck = _nodes[link.from].transport || _nodes[link.to].transport;
        for (uint32 p = 0; p < link.pointCount; p += DangerPointStep)
        {
            RoutePoint point = _points[link.firstPoint + p];
            if (toDeck)
                point.z = std::max(point.z, DeckHeight);
            link.danger[TEAM_ALLIANCE] = std::max(link.danger[TEAM_ALLIANCE], DangerAt(mapId, point.x, point.y, TEAM_ALLIANCE, point.z));
            link.danger[TEAM_HORDE] = std::max(link.danger[TEAM_HORDE], DangerAt(mapId, point.x, point.y, TEAM_HORDE, point.z));
            for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
            {
                uint8 level = 0;
                uint8 const count = CrowdAt(mapId, point.x, point.y, point.z, team, level);
                if (count > link.crowd[team])
                {
                    link.crowd[team] = count;
                    link.crowdLevel[team] = level;
                }
            }
        }
        // The camps the path leads through the middle of, or close beside (the points lie 5 to 60 yd apart: every
        // CampStepYards, and CampSideYards to either side).
        auto const campFrom = std::chrono::steady_clock::now();
        for (uint32 p = 0; p < link.pointCount; ++p)
        {
            RoutePoint const& point = _points[link.firstPoint + p];
            RoutePoint const& next = _points[link.firstPoint + std::min(p + 1, link.pointCount - 1)];
            float const run = Dist2d(point.x, point.y, next.x, next.y);
            int const steps = std::max(1, int(run / CampStepYards));
            // Across the way (the last point: the way it came).
            float sideX = 0.0f, sideY = 0.0f;
            if (run > 0.1f)
            {
                sideX = -(next.y - point.y) / run;
                sideY = (next.x - point.x) / run;
            }
            else if (p > 0)
            {
                RoutePoint const& before = _points[link.firstPoint + p - 1];
                float const back = Dist2d(before.x, before.y, point.x, point.y);
                if (back > 0.1f)
                {
                    sideX = -(point.y - before.y) / back;
                    sideY = (point.x - before.x) / back;
                }
            }
            for (int s = 0; s < steps; ++s)
            {
                float const t = float(s) / float(steps);
                float const px = point.x + (next.x - point.x) * t;
                float const py = point.y + (next.y - point.y) * t;
                float const pz = point.z + (next.z - point.z) * t;
                for (float side : { 0.0f, CampSideYards, -CampSideYards })
                {
                    if (side != 0.0f && sideX == 0.0f && sideY == 0.0f)
                        continue;
                    float const cx = px + sideX * side;
                    float const cy = py + sideY * side;
                    for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
                        if (noteCamp(Risk::CampAtPoint(mapId, cx, cy, team, pz), link.campWalker[team], link.campCount[team],
                            link.campLevel[team]))
                        {
                            link.campX[team] = cx;
                            link.campY[team] = cy;
                        }
                }
                // Where a camp's patrol walks (its middle stands for where the route meets the camp).
                for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
                    if (Risk::CampAt const camp = campByPatrol(mapId, px, py, pz, team);
                        noteCamp(camp, link.campWalker[team], link.campCount[team], link.campLevel[team]))
                    {
                        link.campX[team] = camp.x;
                        link.campY[team] = camp.y;
                    }
            }
        }
        campTime += std::chrono::steady_clock::now() - campFrom;
        if (link.campWalker[TEAM_ALLIANCE] || link.campWalker[TEAM_HORDE])
            ++campLinks;
        _nodes[link.from].links.push_back(i);
        ++usable;
    }
    for (Node& node : _nodes)
    {
        for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
        {
            noteCamp(Risk::CampAtPoint(node.mapId, node.x, node.y, team, node.z), node.campWalker[team], node.campCount[team],
                node.campLevel[team]);
            noteCamp(campByPatrol(node.mapId, node.x, node.y, node.z, team), node.campWalker[team], node.campCount[team],
                node.campLevel[team]);
        }
        if (node.campWalker[TEAM_ALLIANCE] || node.campWalker[TEAM_HORDE])
            ++campNodes;
    }
    LOG_INFO("module", "mod-guild-adventure: road map: {} nodes lie in the middle of a camp and {} links lead through one (a walker "
        "that cannot take the camp gets no route over them; {} creatures on patrol carry their camp along {} points of their ways; "
        "{} ms)", campNodes, campLinks, campPatrols, campPatrolPoints,
        std::chrono::duration_cast<std::chrono::milliseconds>(campTime).count());

    // Islands: a few nodes whose walking links lead only among themselves and that float far above the ground of their
    // continent: Acherus above the Eastern Plaguelands, whose seven nodes (Death's Breach and its halls, 200 yards up)
    // link only to each other and by flight to Light's Hope. No place to put a bot and no end of a route: a level-60
    // mage that was cut off below it was put on its deck and stood there with no whole way down to anything, and the
    // next login put it there again (round 200). Small groups that stand on the ground keep their nodes: Camp Narache on
    // its mesa, Rut'theran by the sea, Freewind Post (a lift), each reached by a way the travel data lacks.
    {
        constexpr uint32 IslandNodes = 8;     // a group of fewer nodes than this
        constexpr float IslandAbove = 40.0f;  // every node at least this far above the terrain under it
        std::vector<uint32> parent(_nodes.size());
        for (uint32 i = 0; i < parent.size(); ++i)
            parent[i] = i;
        auto root = [&parent](uint32 i)
        {
            while (parent[i] != i)
                i = parent[i] = parent[parent[i]];
            return i;
        };
        for (Node const& node : _nodes)
            for (uint32 index : node.links)
                parent[root(_links[index].from)] = root(_links[index].to);
        std::unordered_map<uint32, std::vector<uint32>> groups;
        for (uint32 i = 0; i < _nodes.size(); ++i)
            if (!_nodes[i].links.empty())
                groups[root(i)].push_back(i);
        for (auto const& [first, members] : groups)
        {
            if (members.size() >= IslandNodes)
                continue;
            uint32 const mapId = _nodes[members.front()].mapId;
            MapEntry const* entry = sMapStore.LookupEntry(mapId);
            if (!entry || entry->Instanceable())
                continue;
            Map* map = sMapMgr->CreateBaseMap(mapId);
            if (!map)
                continue;
            float lowest = 0.0f;
            bool floating = true;
            for (uint32 i : members)
            {
                float const ground = map->GetGridHeight(_nodes[i].x, _nodes[i].y);
                if (ground <= INVALID_HEIGHT || _nodes[i].z - ground < IslandAbove)
                {
                    floating = false;
                    break;
                }
                float const above = _nodes[i].z - ground;
                lowest = lowest > 0.0f ? std::min(lowest, above) : above;
            }
            if (!floating)
                continue;
            for (uint32 i : members)
                _nodes[i].island = true;
            Node const& node = _nodes[members.front()];
            LOG_INFO("module", "mod-guild-adventure: road map: {} nodes around {} ({:.0f}, {:.0f}) map {} float {:.0f} yards and more "
                "above the ground with no walk out (no places to put a bot, no ends of routes)", members.size(), node.name, node.x,
                node.y, node.mapId, lowest);
        }
    }

    LOG_INFO("module", "mod-guild-adventure: road map: {} nodes ({} left out because a bot could not walk to them), {} walking "
        "links with {} path points, {} links with holes in their path and {} across the open sea left out, {} more count as "
        "swims, {} points of patrols on the danger map ({} ms)",
        _nodes.size(), remembered, usable, _points.size(), broken, sea, swims, _patrolPoints, GetMSTimeDiffToNow(start));
}

bool RoadMap::OnDungeonGround(Node const& node) const
{
    if (node.dungeon >= 0)
        return node.dungeon != 0;
    // The names of the instance maps, lower case (once).
    static std::vector<std::string> const dungeons = []
    {
        std::vector<std::string> names;
        for (MapEntry const* map : sMapStore)
            if (map->IsDungeon() && map->name[0] && std::strlen(map->name[0]) >= 6)
            {
                std::string name = map->name[0];
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                names.push_back(std::move(name));
            }
        return names;
    }();
    uint32 zone = 0, area = 0;
    sMapMgr->GetZoneAndAreaId(PHASEMASK_NORMAL, zone, area, node.mapId, node.x, node.y, node.z);
    bool dungeon = false;
    for (uint32 id : { zone, area })
        if (AreaTableEntry const* entry = id ? sAreaTableStore.LookupEntry(id) : nullptr)
        {
            std::string name = entry->area_name[0] ? entry->area_name[0] : "";
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            for (std::string const& instance : dungeons)
                dungeon = dungeon || name.find(instance) != std::string::npos;
        }
    node.dungeon = dungeon ? 1 : 0;
    return dungeon;
}

bool RoadMap::FindNearestNode(uint32 mapId, float x, float y, float minDistance, float range, RoutePoint& spot, std::string& name,
    TeamId team, uint8 level) const
{
    float best = range;
    bool found = false;
    bool const guards = level && (team == TEAM_ALLIANCE || team == TEAM_HORDE);
    for (Node const& node : _nodes)
    {
        // Not the far side of a teleporter (the sanctum of Stormwind's wizard tower, a room without a way out on foot),
        // not a dungeon entrance.
        if (node.mapId != mapId || node.links.empty() || node.blocked || node.liftTop || node.island)
            continue;
        // Nor, for a walker, a node a creature it would have to fight stands on, or one in the middle of a camp.
        if (guards && (GuardCounts(node.guard[team], level) || node.campWalker[team] >= level))
            continue;
        float const distance = Dist2d(x, y, node.x, node.y);
        if (distance < minDistance || distance > best)
            continue;
        // Nor a node on a dungeon's ground (the tunnels before its portal).
        if (OnDungeonGround(node))
            continue;
        best = distance;
        found = true;
        spot = { node.x, node.y, node.z };
        name = node.name;
    }
    return found;
}

bool RoadMap::IsNodeAmongGroups(uint32 mapId, float x, float y, TeamId team, uint8 level, CrowdCheck const& canTake) const
{
    for (Node const& node : _nodes)
    {
        if (node.mapId != mapId || node.links.empty() || Dist2d(x, y, node.x, node.y) > 5.0f)
            continue;
        for (uint32 linkId : node.links)
        {
            Link const& link = _links[linkId];
            uint8 const count = link.crowd[team], crowdLevel = link.crowdLevel[team];
            if (!GroupCounts(count, crowdLevel, int32(level)) || (canTake && canTake(count, crowdLevel)))
                return false; // a link the walker can take
        }
        return true;
    }
    return false;
}

std::string RoadMap::BlockNodesNear(uint32 mapId, float x, float y, float z, float radius, float heightYards)
{
    std::string names;
    for (Node& node : _nodes)
    {
        // (Not a lift's stop: a step to it is refused while the platform is at the other one, over the empty shaft. One
        // such step at Freewind Post took both lower stops of its lifts out, and the guild leader on the mesa had no way
        // down for the rest of the session, 2026-11-25.)
        if (node.mapId != mapId || node.blocked || node.liftStop || Dist2d(x, y, node.x, node.y) > radius ||
            std::fabs(node.z - z) > heightYards)
            continue;
        node.blocked = true;
        names += (names.empty() ? "" : ", ") + node.name;
        // Remembered across restarts (ga_road_block).
        std::string name = node.name;
        WorldDatabase.EscapeString(name);
        WorldDatabase.Execute("REPLACE INTO ga_road_block (node_id, name, blocked_at) VALUES ({}, '{}', UNIX_TIMESTAMP())", node.id, name);
    }
    return names;
}

RoadRoute RoadMap::FindRoute(uint32 mapId, float fromX, float fromY, float fromZ, float toX, float toY, float toZ, TeamId team,
    uint8 level, LegCheck const& legWalkable, AvoidCheck const& avoided, CrowdCheck const& canTake, float startStretch) const
{
    // The planner's counters (Perf.h, docs/performance.md PF-0).
    Perf::Counters& counters = Perf::Get();
    counters.routeSearches.fetch_add(1, std::memory_order_relaxed);
    Perf::ScopedUs timer(counters.routeUs);

    // The stretch at the start whose groups do not count is for a walker that stands among them: one that stands
    // outside every group's reach has a short one only. (A level-14 tauren on the shore of Lordamere Lake took a route
    // into the middle of the Vile Fin murlocs' camp 30 yd from it again and again: the camp lay inside the 120 yd of the
    // start, it kept away from it every ten seconds for a minute and a half and its voyage ended stuck, 2026-11-25.)
    if (startStretch == CrowdStartStretch && team <= TEAM_HORDE)
    {
        uint8 hereLevel = 0;
        if (!CrowdAt(mapId, fromX, fromY, fromZ, team, hereLevel))
            startStretch = OutsideStartStretch;
    }

    // Through a place the walker keeps away from: this many times the length.
    constexpr float AvoidedFactor = 30.0f;
    // Past a group the walker cannot take: this many times the length (two characters died on travel routes through
    // groups of three, six and five levels below them: the route knew no other cost for them than half a length each).
    constexpr float CrowdRefusedFactor = 20.0f;
    std::unordered_map<uint16, bool> takes; // (count, level) -> the walker's answer
    auto refuses = [&](uint8 count, uint8 crowdLevel)
    {
        if (!canTake || !GroupCounts(count, crowdLevel, int32(level)))
            return false;
        uint16 const key = uint16(uint16(count) << 8 | crowdLevel);
        auto known = takes.find(key);
        if (known == takes.end())
            known = takes.emplace(key, canTake(count, crowdLevel)).first;
        return !known->second;
    };
    auto linkAvoided = [&](Link const& link)
    {
        if (!avoided)
            return false;
        for (uint32 p = 0; p < link.pointCount; p += DangerPointStep)
            if (avoided(_points[link.firstPoint + p].x, _points[link.firstPoint + p].y))
                return true;
        return false;
    };
    auto lineAvoided = [&](float x1, float y1, float x2, float y2)
    {
        if (!avoided)
            return false;
        float const length = Dist2d(x1, y1, x2, y2);
        int const steps = std::max(1, int(length / 20.0f));
        for (int i = 0; i <= steps; ++i)
            if (avoided(x1 + (x2 - x1) * float(i) / float(steps), y1 + (y2 - y1) * float(i) / float(steps)))
                return true;
        return false;
    };

    RoadRoute route;
    route.mapId = mapId;
    route.direct = Dist2d(fromX, fromY, toX, toY);
    if (_nodes.empty() || team > TEAM_HORDE)
        return route;

    // A camp (Risk::CampAtPoint) the walker has to keep out of: it holds for the walker's level, and the walker cannot
    // take three of its creatures at once (the crowd check's answer). A node in the middle of one is no node of the
    // route, a link through one costs as a link past a group the walker cannot take does, and a route that still leads
    // through one says so (RoadRoute::camp). Not the camp the walker stands in (within `startStretch` of it: it has to
    // get out), nor one the destination lies in (within EndStretch of it: whoever goes there judged the place).
    constexpr uint8 CampFoesAtOnce = 3;
    constexpr float CampEdgeYards = 35.0f;
    auto campRefuses = [&](uint8 walker, uint8 campLevel) { return walker && level <= walker && refuses(CampFoesAtOnce, campLevel); };
    bool ownCamp = false, destCamp = false;
    if (canTake)
    {
        Risk::CampAt const here = Risk::CampAtPoint(mapId, fromX, fromY, team, fromZ);
        ownCamp = here.inside && campRefuses(here.walkerLevel, here.level);
        // (Or at its edge: a camp on a shore is "in the middle" only inland of where a character stands at the water.
        // A warrior that had taken a quest at the Bloodsail camp's seaward side was refused its hand-in in Booty Bay,
        // 289 yd away: the way out led through the camp it stood in.)
        for (int direction = 0; direction < 8 && !ownCamp; ++direction)
        {
            float const angle = float(direction) * 0.785398f;
            Risk::CampAt const beside = Risk::CampAtPoint(mapId, fromX + CampEdgeYards * std::cos(angle),
                fromY + CampEdgeYards * std::sin(angle), team, fromZ);
            ownCamp = beside.inside && campRefuses(beside.walkerLevel, beside.level);
        }
        destCamp = Risk::CampAtPoint(mapId, toX, toY, team, toZ).inside;
    }
    auto campExempt = [&](float x, float y)
    {
        return (ownCamp && Dist2d(fromX, fromY, x, y) <= startStretch) || (destCamp && Dist2d(toX, toY, x, y) <= EndStretch);
    };
    auto nodeInCamp = [&](Node const& node)
    {
        return campRefuses(node.campWalker[team], node.campLevel[team]) && !campExempt(node.x, node.y);
    };
    auto linkInCamp = [&](Link const& link)
    {
        return campRefuses(link.campWalker[team], link.campLevel[team]) && !campExempt(link.campX[team], link.campY[team]);
    };
    // (The walks off the graph, to the first node and from the last one.)
    auto legCamp = [&](float x1, float y1, float x2, float y2, float skipStart, float stopBefore, std::pair<uint8, uint8>& found)
    {
        Risk::CampAt camp;
        if (!canTake || !Risk::LegEntersCamp(mapId, x1, y1, x2, y2, team, level, &camp, nullptr, skipStart, stopBefore) ||
            !campRefuses(camp.walkerLevel, camp.level))
            return false;
        found = { uint8(std::min<uint16>(camp.count, 255)), camp.level };
        return true;
    };

    // The nodes nearest to the start and to the destination.
    auto nearest = [&](float x, float y, float z, bool start, bool checked = true)
    {
        std::vector<std::pair<float, uint32>> found;
        for (uint32 i = 0; i < _nodes.size(); ++i)
        {
            Node const& node = _nodes[i];
            if (node.mapId != mapId || node.links.empty() || node.blocked || node.liftTop || node.island || nodeInCamp(node))
                continue;
            float const distance = Dist2d(x, y, node.x, node.y);
            // (A ship's or zeppelin's node only for a walk to or from that ship: it stands on the deck, over the water or
            // at the top of a tower, and a route to a quest's creatures 423 yd beyond Grom'gol ended at the Iron Eagle's
            // node 50 yd up beside the tower, the walk from it judged through the air, the step up refused below it.)
            if (node.transport && distance > DeckNear)
                continue;
            // (Not a node far above or below it, steeper than any slope a walker takes: the stops of a lift stand one
            // above the other, and from the Barrens' rim beside the Great Lift a mage's route began at the stop 129 yd
            // below it, the walk down the shaft refused, a walk back, the same route again and a rescue, round 183.)
            if (distance <= NodeRadius && std::fabs(node.z - z) <= distance + SteepNodeYards)
                found.emplace_back(distance, i);
        }
        std::sort(found.begin(), found.end());
        // (Not a node on a dungeon's ground as an end of a route, unless nothing else is near.)
        std::vector<std::pair<float, uint32>> outside;
        for (std::size_t i = 0; i < found.size() && i < 3 * MaxEndNodes; ++i)
            if (!OnDungeonGround(_nodes[found[i].second]))
                outside.push_back(found[i]);
        if (!outside.empty())
            found = std::move(outside);
        if (!legWalkable || !checked)
        {
            if (found.size() > MaxEndNodes)
                found.resize(MaxEndNodes);
            return found;
        }
        // The walk between the spot and the node must be one the path finder has: the flight master on Stormwind's
        // wall is 37 yd from the gate below it, and no walk.
        std::vector<std::pair<float, uint32>> usable;
        std::size_t asked = 0;
        for (auto const& [distance, index] : found)
        {
            if (usable.size() >= MaxEndNodes || asked >= 2 * MaxEndNodes)
                break;
            Node const& node = _nodes[index];
            RoutePoint const spot{ x, y, z };
            RoutePoint const at{ node.x, node.y, node.z };
            ++asked;
            if ((distance <= LegTrusted && std::fabs(node.z - z) <= LegTrusted) || (start ? legWalkable(spot, at) : legWalkable(at, spot)))
                usable.emplace_back(distance, index);
        }
        return usable;
    };
    std::vector<std::pair<float, uint32>> starts = nearest(fromX, fromY, fromZ, true);
    std::vector<std::pair<float, uint32>> ends = nearest(toX, toY, toZ, false);
    // No node with a proven walk from the start or to the destination (the walker stands where the path finder has no
    // path from: beside a vendor on a platform; the destination is a waterfall up a river): the route uses the nearest
    // nodes anyway, and that stretch is the path finder's. Without a route the whole way would be (a hunter 1131 yd
    // from its quest spot "found no route and walked straight", 575 yd through a hill, which the ground check stopped).
    bool startGuessed = false;
    if (starts.empty() && legWalkable)
    {
        starts = nearest(fromX, fromY, fromZ, true, false);
        startGuessed = true;
    }
    if (ends.empty() && legWalkable)
        ends = nearest(toX, toY, toZ, false, false);
    if (starts.empty() || ends.empty())
        return route;

    auto legCost = [&](float x1, float y1, float z1, float x2, float y2, float z2, uint8& danger, bool deck = false)
    {
        // (To or from a ship's node, for a walker or a destination above the water or right at the node, where the
        // ship is boarded: over the pier, not through the water the node lies in. A destination in the water farther
        // from it is a swim, and what swims there counts.)
        // Any other walk off the graph: what only swims counts where the line between the two heights is in the water,
        // not where the terrain under it is (the planks of a pier are no terrain: from the end of Theramore's pier
        // every route began "past level-43 creatures", the sharks below, and the town's flight master was out of reach).
        danger = deck ? DangerAlong(mapId, x1, y1, x2, y2, team, 0.0f, DeckHeight, DeckHeight) :
            DangerAlong(mapId, x1, y1, x2, y2, team, 0.0f, z1, z2, true);
        // A long walk off the graph is a guess (the straight line from Stormwind's inn to the harbor ends on the cliff
        // above it): beyond the first stretch it costs much more than links, so a route takes the node next to the
        // destination when there is one.
        float const distance = Dist2d(x1, y1, x2, y2);
        float const weighted = std::min(distance, OffRoadNear) * OffRoadFactor + std::max(0.0f, distance - OffRoadNear) * OffRoadFarFactor;
        return weighted * DangerFactor(int32(danger) - int32(level)) * (lineAvoided(x1, y1, x2, y2) ? AvoidedFactor : 1.0f);
    };

    // Dijkstra from the start over the nodes; the destination is reached from any of the end nodes.
    constexpr float Unreached = 1.0e12f;
    // (Kept per thread and filled again, not allocated for every search: PF-2. A search started from inside another
    // one's callbacks gets arrays of its own.)
    thread_local std::vector<float> sharedCost;
    thread_local std::vector<int32> sharedViaLink;
    thread_local uint32 searchDepth = 0;
    struct DepthScope
    {
        DepthScope() { ++searchDepth; }
        ~DepthScope() { --searchDepth; }
    } const depthScope;
    std::vector<float> ownCost;
    std::vector<int32> ownViaLink;
    std::vector<float>& cost = searchDepth == 1 ? sharedCost : ownCost;
    std::vector<int32>& viaLink = searchDepth == 1 ? sharedViaLink : ownViaLink;
    cost.assign(_nodes.size(), Unreached);
    viaLink.assign(_nodes.size(), -1);
    std::unordered_map<uint32, uint8> startDanger, endDanger;
    std::unordered_map<uint32, float> endCost;
    // The groups on the walks off the graph: many nodes are named places, and many named places are camps (the farm
    // at the start of a route, the mine: both deaths of live round 29 were on the walk to a route's first node). Not
    // the group the walker stands in, nor what lives right at the destination (whoever goes there judges that).
    std::unordered_map<uint32, std::pair<uint8, uint8>> startCrowd, endCrowd; // node -> group, its level
    std::unordered_map<uint32, std::pair<uint8, uint8>> startCamp, endCamp;   // node -> a camp on that walk
    using Entry = std::pair<float, uint32>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    for (auto const& [distance, node] : starts)
    {
        uint8 danger = 0, crowdLevel = 0;
        cost[node] = legCost(fromX, fromY, fromZ, _nodes[node].x, _nodes[node].y, _nodes[node].z, danger,
            _nodes[node].transport && (fromZ > WaterSlack || distance <= DeckNear));
        uint8 const crowd = CrowdAlong(mapId, fromX, fromY, _nodes[node].x, _nodes[node].y, team, crowdLevel, NoHeight, NoHeight,
            startStretch);
        std::pair<uint8, uint8> camp;
        bool const camped = legCamp(fromX, fromY, _nodes[node].x, _nodes[node].y, ownCamp ? startStretch : 0.0f, 0.0f, camp);
        if (camped)
            startCamp[node] = camp;
        if (camped || refuses(crowd, crowdLevel))
            cost[node] *= CrowdRefusedFactor;
        startCrowd[node] = { crowd, crowdLevel };
        startDanger[node] = danger;
        open.emplace(cost[node], node);
    }
    for (auto const& [distance, node] : ends)
    {
        uint8 danger = 0, crowdLevel = 0;
        endCost[node] = legCost(_nodes[node].x, _nodes[node].y, _nodes[node].z, toX, toY, toZ, danger,
            _nodes[node].transport && (toZ > WaterSlack || distance <= DeckNear));
        uint8 const crowd = CrowdAlong(mapId, toX, toY, _nodes[node].x, _nodes[node].y, team, crowdLevel, NoHeight, NoHeight, EndStretch);
        std::pair<uint8, uint8> camp;
        bool const camped = legCamp(_nodes[node].x, _nodes[node].y, toX, toY, 0.0f, destCamp ? EndStretch : 0.0f, camp);
        if (camped)
            endCamp[node] = camp;
        if (camped || refuses(crowd, crowdLevel))
            endCost[node] *= CrowdRefusedFactor;
        endCrowd[node] = { crowd, crowdLevel };
        endDanger[node] = danger;
    }

    float bestTotal = Unreached;
    int32 bestEnd = -1;
    while (!open.empty())
    {
        auto const [nodeCost, node] = open.top();
        open.pop();
        if (nodeCost > cost[node])
            continue;
        if (nodeCost >= bestTotal)
            break;
        auto end = endCost.find(node);
        if (end != endCost.end() && nodeCost + end->second < bestTotal)
        {
            bestTotal = nodeCost + end->second;
            bestEnd = int32(node);
        }
        for (uint32 linkId : _nodes[node].links)
        {
            Link const& link = _links[linkId];
            if (_nodes[link.to].blocked || nodeInCamp(_nodes[link.to]))
                continue;
            // (A group standing on the link costs only when the walker cannot take it. Any group of three used to cost
            // half the link's length more for each: three level-7 murlocs at Crystal Lake sent a level-12 character
            // from Westbrook Garrison to the Eastvale Logging Camp in an arc over Northshire instead of along the road.)
            float const crowded = (linkInCamp(link) || refuses(link.crowd[team], link.crowdLevel[team])) ? CrowdRefusedFactor : 1.0f;
            float const next = nodeCost + (link.distance + link.swim * (SwimFactor - 1.0f)) *
                DangerFactor(int32(link.danger[team]) - int32(level)) * crowded * (linkAvoided(link) ? AvoidedFactor : 1.0f);
            if (next < cost[link.to])
            {
                cost[link.to] = next;
                viaLink[link.to] = int32(linkId);
                open.emplace(next, link.to);
            }
        }
    }
    if (bestEnd < 0)
        return route;

    // Back from the last node to the first one.
    std::vector<uint32> chain;
    uint32 node = uint32(bestEnd);
    while (viaLink[node] >= 0)
    {
        chain.push_back(uint32(viaLink[node]));
        node = _links[viaLink[node]].from;
    }
    std::reverse(chain.begin(), chain.end());
    uint32 const first = node;

    route.found = true;
    route.startGuessed = startGuessed;
    route.danger = std::max(startDanger[first], endDanger[uint32(bestEnd)]);
    route.dangerBefore = std::max(startDanger[first], (_nodes[uint32(bestEnd)].transport && (toZ > WaterSlack ||
        Dist2d(_nodes[uint32(bestEnd)].x, _nodes[uint32(bestEnd)].y, toX, toY) <= DeckNear)) ?
        DangerAlong(mapId, _nodes[uint32(bestEnd)].x, _nodes[uint32(bestEnd)].y, toX, toY, team, EndStretch, DeckHeight, DeckHeight) :
        DangerAlong(mapId, _nodes[uint32(bestEnd)].x, _nodes[uint32(bestEnd)].y, toX, toY, team, EndStretch,
            _nodes[uint32(bestEnd)].z, toZ, true));
    route.length = Dist2d(fromX, fromY, _nodes[first].x, _nodes[first].y) +
        Dist2d(_nodes[uint32(bestEnd)].x, _nodes[uint32(bestEnd)].y, toX, toY);
    route.nodes = uint32(chain.size()) + 1;
    auto take = [&](uint8 count, uint8 crowdLevel, float distance)
    {
        if (count > route.crowd && (!canTake || refuses(count, crowdLevel)))
        {
            route.crowd = count;
            route.crowdLevel = crowdLevel;
            route.crowdDistance = distance;
        }
    };
    take(startCrowd[first].first, startCrowd[first].second, Dist2d(fromX, fromY, _nodes[first].x, _nodes[first].y));
    take(endCrowd[uint32(bestEnd)].first, endCrowd[uint32(bestEnd)].second, Dist2d(fromX, fromY, toX, toY));
    // A camp on the way comes before any group (the nearest one to the start).
    auto takeCamp = [&](uint8 count, uint8 campLevel, float distance)
    {
        if (route.camp && distance >= route.crowdDistance)
            return;
        route.camp = true;
        route.crowd = std::max<uint8>(count, CampFoesAtOnce);
        route.crowdLevel = campLevel;
        route.crowdDistance = distance;
    };
    if (auto camp = startCamp.find(first); camp != startCamp.end())
        takeCamp(camp->second.first, camp->second.second, Dist2d(fromX, fromY, _nodes[first].x, _nodes[first].y));
    if (auto camp = endCamp.find(uint32(bestEnd)); camp != endCamp.end())
        takeCamp(camp->second.first, camp->second.second, Dist2d(fromX, fromY, toX, toY));
    route.via = _nodes[first].name;
    route.points.push_back({ _nodes[first].x, _nodes[first].y, _nodes[first].z });
    for (uint32 linkId : chain)
    {
        Link const& link = _links[linkId];
        route.length += link.distance;
        route.danger = std::max(route.danger, link.danger[team]);
        route.dangerBefore = std::max(route.dangerBefore, link.danger[team]);
        // The biggest group on it (with a check: that the walker cannot take). A link that begins where the walker
        // stands counts from CrowdStartStretch on: the group around it is no reason to stay among them.
        uint8 crowd = link.crowd[team], crowdLevel = link.crowdLevel[team];
        float crowdFrom = Dist2d(fromX, fromY, _nodes[link.from].x, _nodes[link.from].y);
        if (crowd && crowdFrom < startStretch)
        {
            crowd = crowdLevel = 0;
            for (uint32 p = 0; p < link.pointCount; p += DangerPointStep)
            {
                RoutePoint const& point = _points[link.firstPoint + p];
                if (Dist2d(fromX, fromY, point.x, point.y) < startStretch)
                    continue;
                uint8 here = 0;
                uint8 const count = CrowdAt(mapId, point.x, point.y, point.z, team, here);
                if (count > crowd)
                {
                    crowd = count;
                    crowdLevel = here;
                    crowdFrom = Dist2d(fromX, fromY, point.x, point.y);
                }
            }
        }
        if (!route.camp)
            take(crowd, crowdLevel, crowdFrom);
        if (linkInCamp(link))
            takeCamp(link.campCount[team], link.campLevel[team], Dist2d(fromX, fromY, link.campX[team], link.campY[team]));
        if (link.lift)
            route.lifts.push_back({ uint32(route.points.size()), link.lift });
        for (uint32 p = 0; p < link.pointCount; ++p)
            route.points.push_back(_points[link.firstPoint + p]);
        if (route.nodes <= 12)
            route.via += (link.lift ? " > (lift) " : " > ") + _nodes[link.to].name;
    }
    if (route.nodes > 12)
        route.via += " > ... > " + _nodes[uint32(bestEnd)].name;
    return route;
}
