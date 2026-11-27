/*
 * RiskModel implementation. See RiskModel.h.
 */

#include "RiskModel.h"
#include "BotSettings.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "FightEscape.h"
#include "GameTime.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildBotHolder.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "OrderService.h"
#include "Pet.h"
#include "Player.h"
#include "QuestDef.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Squads.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

// The road map of the travel work (RoadMap.h) knows the creatures along a straight line. Walks over 350 yards are
// routed along roads there; for the shorter, straight ones the spot choice below asks it.
#if __has_include("RoadMap.h")
#include "RoadMap.h"
#define GA_RISK_ROAD_MAP 1
#endif

using namespace GuildAdventure;

namespace
{
    // Squad characters within this distance of the planning leader count for the danger limit of a spot.
    constexpr float SquadRiskYards = 120.0f;
    // Mobs this close to a spot join the fight sooner or later: their own aggro radius, the call for help of their
    // neighbours, a patrol, the respawn behind the bot's back.
    constexpr float PackRadius = 22.0f;
    // The fight a spot means. A hostile creature joins it for sure when it is of the target's faction and its
    // spawn lies within the core's assistance radius of the spot (the call for help), or when its spawn lies
    // within its own aggro radius of the spot (the core's: 20 yards, a yard more or less per level it is above or
    // below the bot, times Rate.Creature.Aggro), each plus a small margin. Up to WanderYards farther off it may
    // walk into the fight or not (spawn data does not say how far a creature walks about): it counts half.
    // (Before, everything within the aggro radius plus 6 yards counted in full, and the creatures of a camp
    // farther off counted with their share of the cost but as whole attackers: a single creature with six others
    // 31 to 45 yards away came out at five times its cost.)
    constexpr float AssistMargin = 2.0f;
    constexpr float AggroMargin = 3.0f;
    constexpr float WanderYards = 6.0f;
    constexpr float MaybeShare = 0.25f;
    // The camp around a spot counts for at most this many creatures per ring (within 30 yards a third each, up to
    // 45 a seventh each): the shares stand for creatures that may come into the fight while it lasts, and a fight
    // is over before a dozen could. In a valley with fourteen troggs within 45 yards a level-17 paladin's fight
    // against one of them, five levels below it, came out at 202 % of what it has.
    constexpr float CampCount = 3.0f;
    // A rare is seldom there (hours between its lives, and one of several places holds it): it counts with this
    // share of what its place would count, and not among the foes or the strongest. When it is there the pull
    // check and the look-out for strong roamers see it. (A level-20 priest was kept from a gnoll camp by a rare
    // that the spawn data shows at two places within 23 yards.)
    constexpr float RareShare = 0.15f;
    constexpr float MaxPullYards = 45.0f;
    constexpr float PullHeight = 12.0f;
    // A pet takes this share of a fight's cost off its master.
    constexpr float PetShare = 0.6f;
    // Hostile creatures beyond the pull distance but this near still come into a fight that lasts (they wander, a
    // patrol passes, the bot is pushed back): they count with this share of their cost. A level-18 mage in poor gear
    // died seven times in one ogre camp whose spawns stand 20 yards apart, each "alone" by the pull distance.
    constexpr float CampYards = 30.0f;
    constexpr float CampShare = 0.2f;
    // ... and what stands farther off, up to MaxPullYards, with this share: the spots where bots died three times in a
    // row at one gnoll camp had nothing within 30 yards and three creatures between 31 and 39.
    constexpr float FarCampShare = 0.1f;
    // Deaths by place, kept across restarts: deaths within this time at one place count together; from the second
    // the place is set aside for PlaceAreaMs, twice as long with every further death, at most this long.
    constexpr uint32 PlaceMemorySeconds = 6 * HOUR;
    // The guild's death places: another guild character's death within this long counts for a place that kills the guild
    // (ga_bot_death_place keeps a row this long).
    constexpr uint32 GuildPlaceSeconds = 2 * DAY;
    constexpr uint32 MaxPlaceAreaMs = 8 * HOUR * IN_MILLISECONDS;
    // One point of danger is worth this many yards of walking when spots are compared.
    constexpr float YardsPerDangerPoint = 60.0f;
    // ... and a quest level above the bot this many in the ranking of the objectives.
    constexpr float YardsPerQuestLevel = 120.0f;
    // A safer spawn may lie this much farther away than the nearest one.
    constexpr float SaferSpotExtraDistance = 250.0f;
    // The nearest spawn is taken as it is below this danger (no search).
    constexpr float CalmDanger = 1.0f;
    // At most this many candidates get the terrain lookups of a closer look.
    constexpr std::size_t MaxSpotCandidates = 12;
    constexpr float CaveDanger = 1.5f;
    // Deaths: how long a death is remembered, how close two deaths are "the same place", how long an area stays set
    // aside (an area set aside for its foes' level lasts until the bot has the level, at most FoeAreaMs).
    constexpr uint32 DeathMemoryMs = 30 * MINUTE * IN_MILLISECONDS;
    constexpr float SamePlaceRadius = 80.0f;
    constexpr uint32 PlaceAreaMs = 30 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 FoeAreaMs = 2 * HOUR * IN_MILLISECONDS;
    // Rescue: the foes count as far above the bot from this many levels, the zone when it starts this many above.
    constexpr uint8 RescueFoeLevels = 4;
    constexpr uint8 RescueZoneLevels = 2;
    // Approaches up to this length are walked in a straight line (the planner routes longer ones along roads), and
    // creatures this many levels above the bot on that line rule a spot out.
    [[maybe_unused]] constexpr float StraightApproachYards = 350.0f;
    [[maybe_unused]] constexpr uint8 OutmatchedLevels = 3;
    // Cell size of the mob grid.
    constexpr float CellSize = 32.0f;

    // Mob spawns of a map in a grid, built on first use and again when WorldIndex was reloaded (its vector moved or
    // changed its size).
    struct MobGrid
    {
        MobSpawn const* data = nullptr;
        std::size_t size = 0;
        std::unordered_map<uint64, std::vector<uint32>> cells;
    };
    std::unordered_map<uint32, MobGrid> grids;

    uint64 CellKey(float x, float y)
    {
        int32 const cx = int32(std::floor(x / CellSize));
        int32 const cy = int32(std::floor(y / CellSize));
        return (uint64(uint32(cx)) << 32) | uint32(cy);
    }

    std::unordered_map<uint32, MobGrid> strongGrids;
    std::unordered_map<uint32, MobGrid> patrolGrids;

    MobGrid const& GridFor(std::unordered_map<uint32, MobGrid>& store, std::vector<MobSpawn> const& mobs, uint32 mapId)
    {
        MobGrid& grid = store[mapId];
        if (grid.data != mobs.data() || grid.size != mobs.size())
        {
            grid.cells.clear();
            grid.data = mobs.data();
            grid.size = mobs.size();
            for (uint32 i = 0; i < mobs.size(); ++i)
                grid.cells[CellKey(mobs[i].spawn.x, mobs[i].spawn.y)].push_back(i);
        }
        return grid;
    }

    MobGrid const& GridOf(uint32 mapId)
    {
        return GridFor(grids, sWorldIndex.GetMobSpawns(mapId), mapId);
    }

    // Every mob spawn of a map within `radius` yards (in the plane) of a point: the ordinary ones and the strong
    // ones (elites, rares, bosses; MobSpawn::elite).
    template <typename Visit>
    void ForEachMobNear(uint32 mapId, float x, float y, float radius, Visit&& visit)
    {
        int32 const reach = int32(std::ceil(radius / CellSize));
        int32 const cx = int32(std::floor(x / CellSize));
        int32 const cy = int32(std::floor(y / CellSize));
        for (MobGrid const* grid : { &GridOf(mapId), &GridFor(strongGrids, sWorldIndex.GetStrongSpawns(mapId), mapId) })
            for (int32 gx = cx - reach; gx <= cx + reach; ++gx)
                for (int32 gy = cy - reach; gy <= cy + reach; ++gy)
                {
                    auto cell = grid->cells.find((uint64(uint32(gx)) << 32) | uint32(gy));
                    if (cell == grid->cells.end())
                        continue;
                    for (uint32 index : cell->second)
                    {
                        MobSpawn const& mob = grid->data[index];
                        float const dx = mob.spawn.x - x;
                        float const dy = mob.spawn.y - y;
                        if (dx * dx + dy * dy > radius * radius)
                            continue;
                        visit(mob);
                        // What its script summons (a warlock's succubus or imp) stands beside it and fights with it,
                        // at its level and on its side, with no spawn of its own (WorldIndex::MinionOf).
                        if (uint32 const minion = sWorldIndex.MinionOf(mob.spawn.entry))
                        {
                            MobSpawn pet = mob;
                            pet.spawn.spawnId = 0;
                            pet.spawn.entry = minion;
                            pet.elite = false;
                            pet.rare = mob.rare; // (a rare's minion is as seldom there)
                            pet.flees = false;
                            visit(pet);
                        }
                    }
                }
    }

    // The patrols whose path comes within `radius` of a point: each creature once, with the nearest point of its
    // path and that distance.
    struct PatrolNear
    {
        MobSpawn const* mob;
        float distance;
    };
    std::vector<PatrolNear> PatrolsNear(uint32 mapId, float x, float y, float z, float radius, float height)
    {
        std::vector<PatrolNear> found;
        MobGrid const& grid = GridFor(patrolGrids, sWorldIndex.GetPatrolPoints(mapId), mapId);
        int32 const reach = int32(std::ceil(radius / CellSize));
        int32 const cx = int32(std::floor(x / CellSize));
        int32 const cy = int32(std::floor(y / CellSize));
        for (int32 gx = cx - reach; gx <= cx + reach; ++gx)
            for (int32 gy = cy - reach; gy <= cy + reach; ++gy)
            {
                auto cell = grid.cells.find((uint64(uint32(gx)) << 32) | uint32(gy));
                if (cell == grid.cells.end())
                    continue;
                for (uint32 index : cell->second)
                {
                    MobSpawn const& mob = grid.data[index];
                    float const dx = mob.spawn.x - x;
                    float const dy = mob.spawn.y - y;
                    float const distance = std::sqrt(dx * dx + dy * dy);
                    if (distance > radius || std::fabs(mob.spawn.z - z) > height)
                        continue;
                    auto same = std::find_if(found.begin(), found.end(), [&](PatrolNear const& other)
                    {
                        return other.mob->spawn.spawnId == mob.spawn.spawnId;
                    });
                    if (same == found.end())
                        found.push_back({ &mob, distance });
                    else if (distance < same->distance)
                        *same = { &mob, distance };
                }
            }
        return found;
    }

    // An elite hits and lasts like a creature some levels higher: for the level rules it counts this much more, and
    // its fight costs this many times a normal creature's. At least: an elite counts by its health and damage as the
    // look-outs count it (Escape::ThreatLevelOfKind) when that is more. The Nerubian Overseer of the Eastern
    // Plaguelands, a rare elite with six times the health and four times the damage, counted as an elite of level 63
    // costing a level-60 shaman 37 % of its strength; it cost 120 % and killed it (round 172). By its health and
    // damage it is level 72.
    constexpr uint8 EliteLevels = 3;
    constexpr float EliteCostFactor = 2.5f;
    uint8 EffectiveLevel(MobSpawn const& mob)
    {
        uint32 level = uint32(mob.maxLevel);
        if (mob.elite)
            level = std::max(level + EliteLevels, Escape::ThreatLevelOfKind(sObjectMgr->GetCreatureTemplate(mob.spawn.entry), mob.maxLevel));
        return uint8(std::min<uint32>(level, 255));
    }

    // Indoors (a cave, a mine, a building), remembered per spawn: the lookup reads the terrain.
    std::unordered_map<uint64, bool> indoorCache;

    bool IsIndoors(Player const* player, SpawnPoint const& spot)
    {
        if (!spot.spawnId || spot.mapId != player->GetMapId())
            return false;
        uint64 const key = (uint64(spot.spawnId) << 1) | uint64(spot.gameObject ? 1 : 0);
        auto itr = indoorCache.find(key);
        if (itr != indoorCache.end())
            return itr->second;

        PositionFullTerrainStatus status;
        player->GetMap()->GetFullTerrainStatusForPosition(PHASEMASK_NORMAL, spot.x, spot.y, spot.z, DEFAULT_COLLISION_HEIGHT, status);
        bool const indoors = !status.outdoors;
        indoorCache[key] = indoors;
        return indoors;
    }

    // The highest foe level the bot takes on for now: its own plus the setting, one more with a pet at its side.
    uint8 MaxFoeLevel(Player const* player)
    {
        uint32 level = player->GetLevel() + uint32(sBotSettings.Get(player, Setting::RiskMaxFoeLevelAbove));
        if (player->GetPet())
            ++level;
        return uint8(std::min<uint32>(level, 255));
    }

    float Dist2d(float ax, float ay, float bx, float by)
    {
        float const dx = ax - bx;
        float const dy = ay - by;
        return std::sqrt(dx * dx + dy * dy);
    }
}

namespace
{
    // A creature that attacks the bot by itself (red, not yellow): only those join a fight they were not pulled into.
    bool IsAggressive(MobSpawn const& mob, Player const* player)
    {
        FactionTemplateEntry const* mine = player->GetFactionTemplateEntry();
        FactionTemplateEntry const* theirs = sFactionTemplateStore.LookupEntry(mob.factionTemplate);
        return mine && theirs && theirs->IsHostileTo(*mine);
    }

    // A pet takes part of a fight. What a bot has learned from its own fights it learned with its pet (a warlock whose
    // fights cost 80 % was sent to bears as if they cost 48 %), so the share only stands in until then.
    float PetFactor(Player const* player, BotRuntime const& rt)
    {
        constexpr uint32 LearnedAfterFights = 4;
        return player->GetPet() && rt.fightsLearned < LearnedAfterFights ? PetShare : 1.0f;
    }

    // The foes of a fight, one after the other: the weakest dies first, everyone still standing hits meanwhile (the
    // estimate of FightEscape, on spawn data). weight: how surely the creature is in the fight; one that may come
    // counts with its share as a foe to kill and as one that hits meanwhile.
    struct Part
    {
        float cost;
        float weight;
        bool grouped = true; // within Escape::GroupLevelsBelow of the bot: counts for Escape::CrowdTail
    };

    // How surely a hostile spawn `distance` yards from a spot is in the fight there, for a bot of `level`:
    // 1 = it joins, MaybeShare = it may walk into it, CampShare / FarCampShare = the camp around.
    float JoinWeight(MobSpawn const& mob, float distance, int32 level, uint32 targetFaction)
    {
        // Its own aggro radius, and the target's call for help when it is of the target's faction.
        float pull = std::clamp(20.0f + float(int32(mob.maxLevel) - level), 5.0f, 45.0f) * sWorld->getRate(RATE_CREATURE_AGGRO) + AggroMargin;
        float const assistYards = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        if (targetFaction && mob.factionTemplate == targetFaction && assistYards > 0.0f)
            pull = std::max(pull, assistYards + AssistMargin);
        pull = std::min(pull, MaxPullYards);
        if (distance <= pull)
            return 1.0f;
        if (distance <= pull + WanderYards)
            return MaybeShare;
        return distance <= CampYards ? CampShare : FarCampShare;
    }

    // A patrol is at one place of its round only part of the time: on a round up to this long it counts like a
    // creature that may walk in, on a longer one less.
    constexpr float PatrolRoundFull = 400.0f;
    constexpr int32 TooStrongLevels = 4;
    constexpr float TooStrongFarShare = 0.6f;

    // How surely a hostile creature near a spot is in the fight there. JoinWeight, with two corrections:
    //  - a patrol's path counts as "may walk in" at most (and less on a long round);
    //  - a creature the bot cannot beat alone counts in full when it stands, or walks, within CampYards. A share of
    //    a creature's cost stands for the fights it joins; a share of a creature that kills the bot is a share of
    //    its deaths (a level-25 mage was sent to lumber piles 23 yards from level-28 scouts, "near, counts a third",
    //    and died there).
    float SpotWeight(Player const* player, BotRuntime const& rt, MobSpawn const& mob, float distance, int32 level, uint32 targetFaction,
        float limit, bool* tooStrong = nullptr)
    {
        float weight = JoinWeight(mob, distance, level, targetFaction);
        if (mob.patrol)
            weight = std::min(weight, MaybeShare) * std::min(1.0f, PatrolRoundFull / std::max(mob.round, 1.0f));
        if (mob.rare)
            weight *= RareShare;
        float const alone = Escape::FoeCostAlone(player, rt, uint8(level), int32(mob.maxLevel) - level, mob.spawn.entry) *
            (mob.elite ? EliteCostFactor : 1.0f);
        // (Four levels above the bot is too strong whatever the estimate says: the cost per level is capped at five
        // levels, and a bot whose fights are cheap had "48 %" for a boar seven levels above it that killed it.)
        // However long its round: a spot on the road of a creature that kills the bot is a spot it dies at when the
        // creature comes by, and a grind or a quest's spawns keep it there for minutes. The Nerubian Overseer walks a
        // round of 1135 yd through the Eastern Plaguelands; it counted 9 % at grind spots three yards from its road, and
        // a level-60 shaman died there (round 172). Packs on long rounds counted as too strong since round 161.
        bool const strong = !mob.rare && (alone > limit || int32(EffectiveLevel(mob)) >= level + TooStrongLevels);
        if (tooStrong)
            *tooStrong = strong;
        // (Farther off one such creature is a risk worth taking, two are not.)
        if (strong)
            return distance <= CampYards ? 1.0f : std::max(weight, TooStrongFarShare);
        return weight;
    }

    // What a creature near a spot costs the bot when it joins. A creature TooStrongLevels above the bot costs at
    // least all it has: the estimate's cost per level is capped, and "the fight would cost about 78 %" stood for a
    // target with a boar seven levels above the bot beside it.
    float JoinerCost(Player const* player, BotRuntime const& rt, MobSpawn const& mob, int32 level)
    {
        float const cost = Escape::FoeCostAlone(player, rt, uint8(level), int32(mob.maxLevel) - level, mob.spawn.entry) *
            (mob.elite ? EliteCostFactor : 1.0f);
        return !mob.rare && int32(EffectiveLevel(mob)) >= level + TooStrongLevels ? std::max(cost, 1.0f) : cost;
    }

    uint32 FactionOf(SpawnPoint const& spot)
    {
        if (spot.gameObject)
            return 0;
        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(spot.entry);
        return info ? info->faction : 0;
    }

    float NeedOf(std::vector<Part>& parts, float share)
    {
        std::sort(parts.begin(), parts.end(), [](Part const& a, Part const& b) { return a.cost * a.weight < b.cost * b.weight; });
        float standing = 0.0f;
        float groupedStanding = 0.0f;
        for (Part const& part : parts)
        {
            standing += part.weight;
            if (part.grouped)
                groupedStanding += part.weight;
        }
        // (Four or more at once scatter widely: Escape::CrowdTail; creatures more than Escape::GroupLevelsBelow below
        // the bot do not.)
        float const tail = Escape::CrowdTail(groupedStanding);
        float need = 0.0f;
        for (Part const& part : parts)
        {
            // (The others still standing count a share of a foe each: Escape::OthersShareOf, what the bot's class learned.)
            need += part.cost * part.weight * (part.weight + share * (standing - part.weight));
            standing -= part.weight;
        }
        return need * tail;
    }

    // The members of a patrol's formation that follow it without a path of their own (creature_formations), hostile to
    // the player and no rares, as spawns of their own at the patrol's point.
    std::vector<MobSpawn> FormationFollowers(MobSpawn const& leader, Player const* player)
    {
        std::vector<MobSpawn> followers;
        if (!leader.spawn.spawnId)
            return followers;
        auto const members = sFormationMgr->CreatureGroupMembers.find(leader.spawn.spawnId);
        if (members == sFormationMgr->CreatureGroupMembers.end())
            return followers;
        for (ObjectGuid::LowType member : members->second)
        {
            if (member == leader.spawn.spawnId)
                continue;
            CreatureData const* data = sObjectMgr->GetCreatureData(member);
            CreatureTemplate const* info = data ? sObjectMgr->GetCreatureTemplate(data->id) : nullptr;
            if (!info || info->rank == CREATURE_ELITE_RARE)
                continue;
            if (data->movementType == WAYPOINT_MOTION_TYPE)
                continue; // a path of its own: a patrol of its own
            MobSpawn follower = leader;
            follower.spawn.spawnId = member;
            follower.spawn.entry = data->id;
            follower.minLevel = uint8(info->minlevel);
            follower.maxLevel = uint8(info->maxlevel);
            follower.factionTemplate = info->faction;
            follower.elite = info->rank == CREATURE_ELITE_ELITE || info->rank == CREATURE_ELITE_RAREELITE || info->rank == CREATURE_ELITE_WORLDBOSS;
            follower.rare = false;
            follower.flees = false;
            if (IsAggressive(follower, player))
                followers.push_back(follower);
        }
        return followers;
    }

    // Patrols that walk together: the Crimson Courier of the Eastern Plaguelands (elite, level 60) and its four
    // bodyguards (elites, 57 and 58) each have a path of their own, all as long, and pass a spot side by side. On a
    // round that long (more than PatrolRoundFull) each was counted a few percent, so the grind spots on their road
    // passed: two level-60 bots ground there, the pack came by, and both died within seven seconds, round 161. For each
    // patrol: the others whose round is as long (within a tenth) and whose nearest point lies within PackYards of its
    // own; when all of them together are a fight the bot cannot take, each counts as too strong for it (the pack's
    // size, else 0). A patrol's formation walks with it too: Steelsnap, a level-30 hyena of Thousand Needles, walks a
    // round of 94 points with two Pesterhide Snarlers that have no path of their own (creature_formations); a level-30
    // shaman rested on that round with one creature near and none at its place, the three came by and killed it,
    // round 244.
    std::vector<uint8> PatrolPacks(Player const* player, BotRuntime const& rt, std::vector<PatrolNear> const& patrols, int32 level,
        float limit)
    {
        constexpr float PackYards = 20.0f;
        constexpr float SameRound = 0.1f;
        std::vector<uint8> packs(patrols.size(), 0);
        for (std::size_t i = 0; i < patrols.size(); ++i)
        {
            MobSpawn const& a = *patrols[i].mob;
            if (a.rare || !IsAggressive(a, player))
                continue;
            std::vector<Part> pack;
            std::vector<ObjectGuid::LowType> inPack;
            for (PatrolNear const& other : patrols)
            {
                MobSpawn const& b = *other.mob;
                if (b.rare || !IsAggressive(b, player) || std::fabs(b.round - a.round) > a.round * SameRound ||
                    Dist2d(a.spawn.x, a.spawn.y, b.spawn.x, b.spawn.y) > PackYards)
                    continue;
                pack.push_back({ JoinerCost(player, rt, b, level), 1.0f, true });
                inPack.push_back(b.spawn.spawnId);
            }
            for (MobSpawn const& follower : FormationFollowers(a, player))
                if (std::find(inPack.begin(), inPack.end(), follower.spawn.spawnId) == inPack.end())
                    pack.push_back({ JoinerCost(player, rt, follower, level), 1.0f, true });
            if (pack.size() >= 2 && NeedOf(pack, Escape::OthersShareOf(player)) > limit)
                packs[i] = uint8(std::min<std::size_t>(pack.size(), 255));
        }
        return packs;
    }
}

namespace
{
    // The fight a spot means for a bot of `level` (its own level now, or a later one for SpotVerdict::retryLevel).
    struct SpotFight
    {
        float need = 0.0f;    // share of the bot's full strength
        uint32 foes = 0;      // the target and what joins in full
        uint32 serious = 0;   // of those: about the bot's level or above
        uint8 strongest = 0;  // the highest (effective) level of what stands within PackRadius
        float company = 0.0f; // how many foes beside the target are to be expected (the weights of the others)
        bool target = false;  // a creature of the spot is the target
        bool eliteTarget = false;
    };

    SpotFight FightAt(Player const* player, BotRuntime const& rt, SpawnPoint const& spot, uint8 botLevel)
    {
        SpotFight fight;
        int32 const level = botLevel;
        std::vector<Part> parts;
        uint32 const targetFaction = FactionOf(spot);
        // A target that runs for help comes back with the nearest of its kind within the core's 30 yards.
        bool const targetFlees = !spot.gameObject && sWorldIndex.FleesForHelp(spot.entry);
        // A target that calls for help brings every creature of its side within its call (WorldIndex::CallForHelpYards).
        float const callYards = spot.gameObject ? 0.0f : sWorldIndex.CallForHelpYards(spot.entry);
        float const fetchYards = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS);
        std::size_t helper = 0; // index into parts, 0 = none (parts[0] is the target when there is one)
        float helperDistance = 0.0f;
        float const limit = sBotSettings.GetFloat(player, Setting::RiskMaxFightCostPercent);
        std::vector<std::pair<ObjectGuid::LowType, std::size_t>> counted; // spawn id -> its part
        // Where each part's spawn stands and whether it is of the target's faction (the helper's neighbours come with it).
        struct Where
        {
            float x;
            float y;
            bool kin;
        };
        std::vector<Where> where;
        // What the target's and the sure joiners' deaths bring (WorldIndex::DeathSummonOf): counted after the rings.
        std::vector<std::pair<uint32, uint8>> deathSummons;
        if (uint8 const foe = Risk::ThreatLevel(player, spot))
        {
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(spot.entry);
            bool const elite = info && info->rank != CREATURE_ELITE_NORMAL && info->rank != CREATURE_ELITE_RARE;
            parts.push_back({ Escape::FoeCost(rt, botLevel, int32(foe) - level, spot.entry) * (elite ? EliteCostFactor : 1.0f), 1.0f });
            fight.target = true;
            fight.eliteTarget = elite;
            if (auto const summon = sWorldIndex.DeathSummonOf(spot.entry); summon.first)
                deathSummons.push_back(summon);
            // (And what it summons in the fight: counted as what its death brings, after the rings.)
            if (auto const summon = sWorldIndex.CombatSummonOf(spot.entry); summon.first && sWorldIndex.MinionOf(spot.entry) != summon.first)
                deathSummons.push_back(summon);
            ++fight.foes;
            if (int32(foe) + (elite ? EliteLevels : 0) + 1 >= level)
                ++fight.serious;
        }

        ForEachMobNear(spot.mapId, spot.x, spot.y, MaxPullYards, [&](MobSpawn const& mob)
        {
            if (!spot.gameObject && mob.spawn.spawnId == spot.spawnId)
                return;
            float const distance = Dist2d(mob.spawn.x, mob.spawn.y, spot.x, spot.y);
            if (std::fabs(mob.spawn.z - spot.z) > PullHeight || !IsAggressive(mob, player))
                return;
            if (distance <= PackRadius && !mob.rare)
                fight.strongest = std::max(fight.strongest, EffectiveLevel(mob));
            float weight = SpotWeight(player, rt, mob, distance, level, targetFaction, limit);
            if (callYards > 0.0f && targetFaction && mob.factionTemplate == targetFaction && !mob.rare && distance <= callYards + 2.0f)
                weight = std::max(weight, 1.0f);
            bool const joins = weight >= 1.0f;
            // (What comes with the target is not held by a pet: FoeCostAlone.)
            parts.push_back({ JoinerCost(player, rt, mob, level), weight, int32(mob.maxLevel) + Escape::GroupLevelsBelow >= level });
            where.resize(parts.size(), Where{ spot.x, spot.y, false });
            where.back() = { mob.spawn.x, mob.spawn.y, targetFaction && mob.factionTemplate == targetFaction && !mob.rare };
            counted.push_back({ mob.spawn.spawnId, parts.size() - 1 });
            // (The core counts both creatures' sizes: about 3 yd more than its radius, FightEscape's JudgePull.)
            CreatureData const* helperData = targetFlees && !joins ? sObjectMgr->GetCreatureData(mob.spawn.spawnId) : nullptr;
            float const helperWalk = helperData ? std::min(helperData->wander_distance, 10.0f) : 0.0f;
            if (targetFlees && !joins && !mob.rare && targetFaction && mob.factionTemplate == targetFaction &&
                distance <= fetchYards + 3.0f + helperWalk && (!helper || distance < helperDistance))
            {
                helper = parts.size() - 1;
                helperDistance = distance;
            }
            if (joins)
            {
                ++fight.foes;
                if (int32(EffectiveLevel(mob)) + 1 >= level)
                    ++fight.serious;
                if (auto const summon = sWorldIndex.DeathSummonOf(mob.spawn.entry); summon.first)
                    deathSummons.push_back(summon);
                if (auto const summon = sWorldIndex.CombatSummonOf(mob.spawn.entry);
                    summon.first && sWorldIndex.MinionOf(mob.spawn.entry) != summon.first)
                    deathSummons.push_back(summon);
            }
        });

        // Patrols: the nearest point of each one's path. A creature already counted where it spawns keeps the higher
        // of the two weights. One of a pack the bot cannot take counts as too strong for it, however long its round.
        std::vector<PatrolNear> const patrols = PatrolsNear(spot.mapId, spot.x, spot.y, spot.z, MaxPullYards, PullHeight);
        std::vector<uint8> const packs = PatrolPacks(player, rt, patrols, level, limit);
        for (std::size_t p = 0; p < patrols.size(); ++p)
        {
            PatrolNear const& patrol = patrols[p];
            MobSpawn const& mob = *patrol.mob;
            if ((!spot.gameObject && mob.spawn.spawnId == spot.spawnId) || !IsAggressive(mob, player))
                continue;
            float weight = SpotWeight(player, rt, mob, patrol.distance, level, targetFaction, limit);
            if (packs[p])
                weight = patrol.distance <= CampYards ? 1.0f : std::max(weight, TooStrongFarShare);
            if (patrol.distance <= PackRadius && !mob.rare)
                fight.strongest = std::max(fight.strongest, EffectiveLevel(mob));
            // (A pack of a formation: its followers come with it, as heavy as it.)
            if (packs[p])
                for (MobSpawn const& follower : FormationFollowers(mob, player))
                    if (std::none_of(counted.begin(), counted.end(), [&](std::pair<ObjectGuid::LowType, std::size_t> const& entry)
                            { return entry.first == follower.spawn.spawnId; }))
                    {
                        parts.push_back({ JoinerCost(player, rt, follower, level), weight, true });
                        where.resize(parts.size(), Where{ spot.x, spot.y, false });
                        counted.emplace_back(follower.spawn.spawnId, parts.size() - 1);
                        if (weight >= 1.0f)
                        {
                            ++fight.foes;
                            if (int32(EffectiveLevel(follower)) + 1 >= level)
                                ++fight.serious;
                        }
                    }
            auto same = std::find_if(counted.begin(), counted.end(), [&](std::pair<ObjectGuid::LowType, std::size_t> const& entry)
            {
                return entry.first == mob.spawn.spawnId;
            });
            bool joinsNew = weight >= 1.0f;
            if (same != counted.end())
            {
                if (parts[same->second].weight >= weight)
                    continue;
                joinsNew = joinsNew && parts[same->second].weight < 1.0f;
                parts[same->second].weight = weight;
            }
            else
            {
                parts.push_back({ JoinerCost(player, rt, mob, level), weight, int32(mob.maxLevel) + Escape::GroupLevelsBelow >= level });
                where.resize(parts.size(), Where{ spot.x, spot.y, false });
            }
            if (joinsNew)
            {
                ++fight.foes;
                if (int32(EffectiveLevel(mob)) + 1 >= level)
                    ++fight.serious;
            }
        }

        if (helper && fight.foes)
        {
            // (A target that runs early in every fight surely fetches: Escape::JudgePull, round 229.)
            float const share = sWorldIndex.FleesForHelpEarly(spot.entry) ? 1.0f : Escape::FetchedShare(player);
            parts[helper].weight = std::max(parts[helper].weight, share);
            // What stands with the helper comes with it (two more at most).
            float const assistYards = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS) + AssistMargin;
            uint32 more = 0;
            for (std::size_t i = 1; i < parts.size() && i < where.size() && more < 2; ++i)
                if (i != helper && where[i].kin && parts[i].weight < share &&
                    Dist2d(where[i].x, where[i].y, where[helper].x, where[helper].y) <= assistYards)
                {
                    parts[i].weight = share;
                    ++more;
                }
        }
        // (After the helper: a creature that counts more than its ring's share is no longer of the ring.)
        for (float const share : { CampShare, FarCampShare })
        {
            float total = 0.0f;
            for (Part const& part : parts)
                if (part.weight == share)
                    total += part.weight;
            if (total > share * CampCount)
                for (Part& part : parts)
                    if (part.weight == share)
                        part.weight *= share * CampCount / total;
        }
        // What their deaths bring: one more foe each, after the one it comes out of (a Tortured Druid's Hive'Ashi Drone,
        // round 231). Not counted as one that joins at once (fight.foes), but as company: such a target is no single one.
        for (auto const& [summon, count] : deathSummons)
            if (CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(summon))
            {
                int32 const summonLevel = int32(std::max(info->minlevel, info->maxlevel));
                bool const elite = info->rank != CREATURE_ELITE_NORMAL && info->rank != CREATURE_ELITE_RARE;
                float const cost = Escape::FoeCostAlone(player, rt, botLevel, summonLevel - level, summon) * (elite ? EliteCostFactor : 1.0f);
                for (uint8 n = 0; n < count; ++n)
                    parts.push_back({ cost, 1.0f, summonLevel + Escape::GroupLevelsBelow >= level });
            }
        for (Part const& part : parts)
            fight.company += part.weight;
        if (fight.target)
            fight.company -= 1.0f;
        float need = NeedOf(parts, Escape::OthersShareOf(player));
        need *= PetFactor(player, rt);
        fight.need = need;
        return fight;
    }
}

float Risk::SpotFightCost(Player const* player, BotRuntime const& rt, SpawnPoint const& spot)
{
    return FightAt(player, rt, spot, player->GetLevel()).need;
}

Risk::SpotVerdict Risk::JudgeSpot(Player const* player, BotRuntime const& rt, SpawnPoint const& spot, uint32 extraTogether)
{
    SpotVerdict verdict;
    if (!sBotSettings.IsOn(player, Setting::RiskEnable))
        return verdict;

    uint8 const level = player->GetLevel();
    uint8 const foe = ThreatLevel(player, spot);
    verdict.strongest = foe;
    if (rt.IsInAvoidedArea(spot.mapId, spot.x, spot.y, level))
    {
        verdict.usable = false;
        verdict.reason = "the place is set aside";
        return verdict;
    }
    // A strong creature on its way was seen there, or walks there in a moment (FightEscape.h: a patrol, Stitches).
    if (Escape::RoamerNear(spot.mapId, spot.x, spot.y, level))
    {
        verdict.usable = false;
        verdict.reason = "a strong creature is walking there right now";
        return verdict;
    }

    uint32 const together = sSquadService.CountTogether(player, SquadRiskYards) + extraTogether;
    float const limit = sBotSettings.GetFloat(player, Setting::RiskMaxFightCostPercent) * float(together);
    uint32 const maxFoes = uint32(sBotSettings.Get(player, Setting::EscapeMaxFoes)) * together;
    int32 const capAbove = int32(MaxFoeLevel(player)) - int32(level);
    // (Whether the spot lies in the middle of a camp the bot cannot take: no single-target exemption there.)
    bool const inCamp = !spot.gameObject && InCampItCannotTake(player, rt, spot.mapId, spot.x, spot.y, spot.z);
    auto why = [&](uint8 atLevel, SpotFight const& fight) -> char const*
    {
        int32 const cap = int32(atLevel) + capAbove;
        if (int32(foe) > cap)
            return "its target is above the level cap";
        // (A creature ThreatLevels above the bot, MaxFoeLevelAbove + 1 by default: the same line as the target's cap.)
        if (int32(fight.strongest) > cap)
            return "a creature far above the bot stands there";
        if (maxFoes && fight.foes >= 2 && fight.serious >= maxFoes)
            return "too many foes would join";
        // A single ordinary target with less than half a foe more to be expected beside it, outside the middle of a
        // camp the bot cannot take, is no refusal (the owner, 2026-11-14: a detour or a refusal only for elites, for
        // creatures four or more levels above the bot and for groups it cannot take); the target's level cap stays.
        // (Up to a whole foe more it let a paladin of level 52 work the Jaedenar Cultists of "Forces of Jaedenar" one
        // spawn after the other into the middle of their camp, where every way out had creatures on it, and it died
        // against four of them: in a camp the shares of the creatures around stay small, the camp is still a group.)
        if (fight.need > limit && !(fight.target && !fight.eliteTarget && fight.company < 0.5f && !inCamp))
            return "the fight would cost too much";
        return nullptr;
    };

    SpotFight const fight = FightAt(player, rt, spot, level);
    verdict.cost = fight.need;
    verdict.foes = fight.foes;
    verdict.strongest = std::max(foe, fight.strongest);
    // A place to walk up to (a quest giver, an ender, a vendor, an object to hand a quest in at: nothing to pull) is
    // judged by what stands around it too, not only by who would join a fight at the place itself: the bot has to
    // get there. Three or more creatures that would attack it within GuardYards, more than it can take together
    // (four at most are counted): the place is guarded. A priest rode 355 seconds to Dalinda Malem in Thunder Axe
    // Fortress (eight Burning Blade within 31 yards of her, one of them six yards away, "would work there": one or two
    // would join a fight at her side) and turned round at the gate; a warrior died at the Bloodsail Correspondence
    // (six pirates within 30 yards). Hubs have no such spawns. An object to use counts as such a place too (a
    // shaman of level 42 opening The Emperor's Tomb of "Kurzen's Mystery" in the Bloodscalp ruins, "11%: it would
    // work there", had five trolls on it in the next minute and died; four or more within 30 yards there, none within
    // three levels of it).
    if (spot.gameObject)
    {
        constexpr float GuardYards = 30.0f;
        constexpr uint32 MinGuards = 3;
        constexpr uint32 GuardsAtOnce = 4;
        uint8 strongest = 0;
        uint32 const guards = GuardsNear(player, spot.mapId, spot.x, spot.y, spot.z, GuardYards, &strongest);
        if (guards >= MinGuards && !CanFight(player, rt, strongest, std::min(guards, GuardsAtOnce)))
        {
            verdict.usable = false;
            verdict.reason = "the place is guarded";
            verdict.foes = uint8(std::min<uint32>(guards, 255));
            verdict.strongest = std::max(verdict.strongest, strongest);
            return verdict;
        }
    }
    if (char const* reason = why(level, fight))
    {
        verdict.usable = false;
        verdict.reason = reason;
        constexpr uint8 LookAhead = 8;
        for (uint32 next = uint32(level) + 1; next <= uint32(level) + LookAhead && next <= 255; ++next)
            if (!why(uint8(next), FightAt(player, rt, spot, uint8(next))))
            {
                verdict.retryLevel = uint8(next);
                break;
            }
    }
    return verdict;
}

Risk::SummonVerdict Risk::JudgeSummon(Player const* player, BotRuntime const& rt, uint32 spellId)
{
    SummonVerdict verdict;
    if (!sBotSettings.IsOn(player, Setting::RiskEnable))
        return verdict;
    // The creatures the spell summons (and a spell it triggers), hostile to the bot and not under its control, each with
    // the time it comes at (an event's script summons waves).
    std::vector<CreatureTemplate const*> summons;
    std::vector<uint32> comesAt; // seconds after the use
    FactionTemplateEntry const* mine = player->GetFactionTemplateEntry();
    auto hostile = [mine](uint32 faction)
    {
        FactionTemplateEntry const* theirs = sFactionTemplateStore.LookupEntry(faction);
        return theirs && mine && theirs->IsHostileTo(*mine);
    };
    auto collect = [&](uint32 id, auto& self, int depth) -> void
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
        if (!info)
            return;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& effect = info->Effects[i];
            if (effect.Effect == SPELL_EFFECT_TRIGGER_SPELL && effect.TriggerSpell && depth == 0)
                self(effect.TriggerSpell, self, depth + 1);
            // An event the spell sends whose script summons creatures: "King of the Foulweald" (Karang's Banner, event
            // 6721) brings 17 furbolgs of level 23-25 in waves over three minutes and Chief Murgut of 26 at the end; two
            // bots of 22 and 23 used it and died within 100 s of each other, 2026-11-24.
            if (effect.Effect == SPELL_EFFECT_SEND_EVENT && effect.MiscValue > 0)
            {
                auto const event = sEventScripts.find(uint32(effect.MiscValue));
                if (event != sEventScripts.end())
                    for (auto const& [delay, script] : event->second)
                    {
                        if (script.command != SCRIPT_COMMAND_TEMP_SUMMON_CREATURE)
                            continue;
                        CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(script.TempSummonCreature.CreatureEntry);
                        if (creature && hostile(creature->faction))
                        {
                            summons.push_back(creature);
                            comesAt.push_back(delay);
                        }
                    }
                continue;
            }
            // An object the spell puts down whose trap casts a spell (a banner, a totem): what that spell summons.
            if (effect.Effect == SPELL_EFFECT_SUMMON_OBJECT_WILD && effect.MiscValue > 0 && depth < 2)
            {
                if (GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(uint32(effect.MiscValue)))
                    if (object->type == GAMEOBJECT_TYPE_TRAP && object->trap.spellId)
                        self(object->trap.spellId, self, depth + 1);
                continue;
            }
            if (effect.Effect != SPELL_EFFECT_SUMMON || effect.MiscValue <= 0)
                continue;
            CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(uint32(effect.MiscValue));
            if (!creature)
                continue;
            uint32 faction = creature->faction;
            if (SummonPropertiesEntry const* properties = sSummonPropertiesStore.LookupEntry(uint32(effect.MiscValueB)))
            {
                if (properties->Category != SUMMON_CATEGORY_WILD)
                    continue; // a guardian, a pet, a totem, a vehicle: the bot's own
                if (properties->Faction)
                    faction = properties->Faction;
            }
            if (!hostile(faction))
                continue;
            summons.push_back(creature);
            comesAt.push_back(0);
        }
    };
    collect(spellId, collect, 0);
    if (summons.empty())
        return verdict;
    // What the summons call in by their script comes too (the Deepmoss Eggs' trap brings two Deepmoss Hatchlings of
    // level 14, and each of them calls a Deepmoss Matriarch of 22 at 30 % health: a level-19 warrior opened two eggs and
    // died with four on it, 2026-11-24).
    for (std::size_t i = 0, count = summons.size(); i < count; ++i)
        if (uint32 const minion = sWorldIndex.MinionOf(summons[i]->Entry))
            if (CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(minion))
            {
                summons.push_back(creature);
                comesAt.push_back(comesAt[i]);
            }

    uint8 const level = player->GetLevel();
    int32 const capAbove = int32(MaxFoeLevel(player)) - int32(level);
    float const limit = sBotSettings.GetFloat(player, Setting::RiskMaxFightCostPercent) *
        float(sSquadService.CountTogether(player, SquadRiskYards));
    // Too much for a bot of `at`: above the cap, too strong, or a fight that costs more than the limit (the summons
    // come together, so their costs add up).
    // Waves: what comes within WaveSeconds of another is fought together (the bot does not finish one wave before the
    // next), so the fight is the costliest such window, not every summon at once.
    constexpr uint32 WaveSeconds = 40;
    auto tooMuch = [&](uint8 at) -> bool
    {
        std::vector<float> cost(summons.size(), 0.0f);
        for (std::size_t i = 0; i < summons.size(); ++i)
        {
            CreatureTemplate const* creature = summons[i];
            bool const elite = creature->rank == CREATURE_ELITE_ELITE || creature->rank == CREATURE_ELITE_RAREELITE ||
                creature->rank == CREATURE_ELITE_WORLDBOSS;
            int32 const worth = int32(creature->maxlevel) + (elite ? EliteLevels : 0);
            if (int32(creature->maxlevel) > int32(at) + capAbove || worth >= int32(at) + TooStrongLevels)
                return true;
            cost[i] = Escape::FoeCostAlone(player, rt, at, int32(creature->maxlevel) - int32(at), creature->Entry) *
                (elite ? EliteCostFactor : 1.0f);
        }
        float need = 0.0f;
        for (std::size_t i = 0; i < summons.size(); ++i)
        {
            float window = 0.0f;
            for (std::size_t j = 0; j < summons.size(); ++j)
                if (comesAt[j] >= comesAt[i] && comesAt[j] < comesAt[i] + WaveSeconds)
                    window += cost[j];
            need = std::max(need, window);
        }
        return need > limit;
    };
    if (!tooMuch(level))
        return verdict;
    verdict.usable = false;
    std::vector<std::pair<CreatureTemplate const*, uint32>> kinds;
    for (CreatureTemplate const* creature : summons)
    {
        auto known = std::find_if(kinds.begin(), kinds.end(), [creature](auto const& kind) { return kind.first == creature; });
        if (known == kinds.end())
            kinds.push_back({ creature, 1 });
        else
            ++known->second;
    }
    for (auto const& [creature, count] : kinds)
        verdict.what += Acore::StringFormat("{}{}{} (level {})", verdict.what.empty() ? "" : ", ", count > 1 ? Acore::StringFormat("{} x ", count) :
            std::string(), creature->Name, uint32(creature->maxlevel));
    if (std::any_of(comesAt.begin(), comesAt.end(), [](uint32 at) { return at > 0; }))
        verdict.what += Acore::StringFormat(" in waves over {} s", *std::max_element(comesAt.begin(), comesAt.end()));
    constexpr uint8 LookAhead = 8;
    for (uint32 next = uint32(level) + 1; next <= uint32(level) + LookAhead && next <= 255; ++next)
        if (!tooMuch(uint8(next)))
        {
            verdict.retryLevel = uint8(next);
            break;
        }
    return verdict;
}

std::string Risk::DescribeSpot(Player const* player, BotRuntime const& rt, SpawnPoint const& spot)
{
    int32 const level = player->GetLevel();
    std::string joiners;
    uint32 count = 0;
    uint32 const targetFaction = FactionOf(spot);
    float const limit = sBotSettings.GetFloat(player, Setting::RiskMaxFightCostPercent);
    ForEachMobNear(spot.mapId, spot.x, spot.y, MaxPullYards, [&](MobSpawn const& mob)
    {
        float const distance = Dist2d(mob.spawn.x, mob.spawn.y, spot.x, spot.y);
        // (The target itself is not listed; its minion, which stands where it stands, is: JudgeSpot counts it.)
        if (std::fabs(mob.spawn.z - spot.z) > PullHeight || (distance < 0.5f && mob.spawn.spawnId) || !IsAggressive(mob, player))
            return;
        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(mob.spawn.entry);
        float const weight = JoinWeight(mob, distance, level, targetFaction);
        bool tooStrong = false;
        SpotWeight(player, rt, mob, distance, level, targetFaction, limit, &tooStrong);
        if (++count <= 8)
            joiners += Acore::StringFormat("{}{} {}{}{} at {:.0f} yd{}", joiners.empty() ? "" : ", ", info ? info->Name : "?",
                uint32(mob.maxLevel), mob.elite ? " elite" : "", mob.rare ? " rare" : "", distance,
                mob.rare ? " (seldom there, counts little)" : (tooStrong ? (distance <= CampYards ?
                    " (too strong for it alone, counts in full)" : " (too strong for it alone, counts 60%)") :
                    (weight >= 1.0f ? "" : (weight >= MaybeShare ? " (may walk in, counts a quarter)" :
                        (weight >= CampShare ? " (near, counts a fifth)" : " (farther off, counts a tenth)")))));
    });
    std::vector<PatrolNear> const patrols = PatrolsNear(spot.mapId, spot.x, spot.y, spot.z, MaxPullYards, PullHeight);
    std::vector<uint8> const packs = PatrolPacks(player, rt, patrols, level, limit);
    for (std::size_t p = 0; p < patrols.size(); ++p)
    {
        PatrolNear const& patrol = patrols[p];
        MobSpawn const& mob = *patrol.mob;
        if ((!spot.gameObject && mob.spawn.spawnId == spot.spawnId) || !IsAggressive(mob, player))
            continue;
        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(mob.spawn.entry);
        bool tooStrong = false;
        float weight = SpotWeight(player, rt, mob, patrol.distance, level, targetFaction, limit, &tooStrong);
        if (packs[p])
            weight = patrol.distance <= CampYards ? 1.0f : std::max(weight, TooStrongFarShare);
        if (++count <= 8)
            joiners += Acore::StringFormat("{}{} {}{} on patrol, its way {:.0f} yd from here (a round of {:.0f} yd; {})",
                joiners.empty() ? "" : ", ", info ? info->Name : "?", uint32(mob.maxLevel), mob.elite ? " elite" : "", patrol.distance,
                mob.round, packs[p] ? Acore::StringFormat("one of {} that walk together, too strong for it together, counts {:.0f}%",
                    uint32(packs[p]), weight * 100.0f) :
                    tooStrong ? Acore::StringFormat("too strong for it alone, counts {:.0f}%", weight * 100.0f) :
                    Acore::StringFormat("counts {:.0f}%", weight * 100.0f));
    }
    SpotVerdict const verdict = JudgeSpot(player, rt, spot);
    uint8 const foe = ThreatLevel(player, spot);
    if (auto const [summon, summons] = spot.gameObject ? std::pair<uint32, uint8>{ 0, 0 } : sWorldIndex.DeathSummonOf(spot.entry); summon)
        if (CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(summon))
            joiners += Acore::StringFormat("{}the target brings {}{} {} at its death (counts in full)", joiners.empty() ? "" : "; ",
                summons > 1 ? Acore::StringFormat("{} x ", uint32(summons)) : std::string(), info->Name, uint32(info->maxlevel));
    if (auto const [summon, summons] = spot.gameObject ? std::pair<uint32, uint8>{ 0, 0 } : sWorldIndex.CombatSummonOf(spot.entry); summon)
        if (CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(summon))
            joiners += Acore::StringFormat("{}the target summons {}{} {} in the fight (counts in full)", joiners.empty() ? "" : "; ",
                summons > 1 ? Acore::StringFormat("{} x ", uint32(summons)) : std::string(), info->Name, uint32(info->maxlevel));
    if (float const callYards = spot.gameObject ? 0.0f : sWorldIndex.CallForHelpYards(spot.entry); callYards > 0.0f)
        joiners += Acore::StringFormat("{}the target calls every creature of its side within {:.0f} yd for help in the fight (they count in full)",
            joiners.empty() ? "" : "; ", callYards);
    if (!spot.gameObject && sWorldIndex.FleesForHelp(spot.entry))
        joiners += Acore::StringFormat("{}the target runs for help {}: the nearest of its kind within 30 yd counts {:.0f}%, "
            "and so do up to two that stand with that one",
            joiners.empty() ? "" : "; ", sWorldIndex.FleesForHelpEarly(spot.entry) ? "early in a fight" : "when it is nearly dead",
            (sWorldIndex.FleesForHelpEarly(spot.entry) ? 1.0f : Escape::FetchedShare(player)) * 100.0f);
    // The camp the point lies in the middle of, if any (CampAtPoint).
    CampAt const camp = CampAtPoint(spot.mapId, spot.x, spot.y, player->GetTeamId(), spot.z);
    std::string const campText = !camp.inside ? std::string("; the point is in the middle of no camp") :
        Acore::StringFormat("; the point is in the middle of a camp ({} spawns of level {}-{}, mostly {}, around ({:.0f}, {:.0f}) within "
            "{:.0f} yd; a camp for walkers up to level {}): {}", uint32(camp.count), uint32(camp.minLevel), uint32(camp.maxLevel),
            uint32(camp.level), camp.x, camp.y, camp.radius, uint32(camp.walkerLevel),
            InCampItCannotTake(player, rt, spot.mapId, spot.x, spot.y, spot.z) ?
                (level > camp.walkerLevel ? "its creatures are far below it, but what it learned of their kind says it cannot take "
                    "them, and it keeps out" : "it cannot take it and keeps out") :
                (level > camp.walkerLevel ? "its creatures are too far below it to count" : "it can take it"));
    // The water there: by the feet (WaterAt) and by its depth (what a rest asks: Escape::TooWetToRest).
    float const depth = Escape::WaterDepth(player, spot.x, spot.y, spot.z);
    std::string const waterText = depth <= 0.0f && !Escape::WaterAt(player, spot.x, spot.y, spot.z) ? std::string() :
        Acore::StringFormat(", {} ({:.1f} yd of water there: {})", Escape::WaterAt(player, spot.x, spot.y, spot.z) ? "in the water" : "at the water's surface",
            depth, Escape::TooWetToRest(player, spot.x, spot.y, spot.z) ? "no place to rest" : "shallow enough to rest in");
    return Acore::StringFormat("{} (level {}; a foe of its level costs it {:.0f}%): at ({:.0f}, {:.0f}, {:.0f}) the target is level {}{}, {} "
        "hostile spawn{} within reach{}{}; the fight would cost about {:.0f}% of what it has (limit {:.0f}%, level cap {}): {}{}",
        player->GetName(), level, Escape::FoeCost(rt, uint8(level), 0) * 100.0f, spot.x, spot.y, spot.z, uint32(foe),
        waterText, count, count == 1 ? "" : "s", joiners.empty() ? "" : ": ",
        joiners, verdict.cost * 100.0f, limit * 100.0f, uint32(MaxFoeLevel(player)),
        verdict.usable ? std::string("it would work there") : Acore::StringFormat("the spot waits: {}{}", verdict.reason,
            verdict.retryLevel ? Acore::StringFormat(" (worth another look at level {})", uint32(verdict.retryLevel)) : std::string()),
        campText);
}

bool Risk::CanFight(Player const* player, BotRuntime const& rt, uint8 foeLevel, uint32 foes, uint32 entry)
{
    if (!sBotSettings.IsOn(player, Setting::RiskEnable))
        return true;
    if (foeLevel > MaxFoeLevel(player))
        return false;
    int32 const levelsAbove = int32(foeLevel) - int32(player->GetLevel());
    std::vector<Part> parts(std::max<uint32>(foes, 1), Part{ Escape::FoeCostAlone(player, rt, player->GetLevel(), levelsAbove, entry),
        1.0f, int32(foeLevel) + Escape::GroupLevelsBelow >= int32(player->GetLevel()) });
    parts[0].cost = Escape::FoeCost(rt, player->GetLevel(), levelsAbove, entry); // a pet holds one
    float need = NeedOf(parts, Escape::OthersShareOf(player));
    need *= PetFactor(player, rt);
    // A squad shares the fight, as for the spots (SaferSpot): the limit grows with the characters together (Squads.h).
    return need <= sBotSettings.GetFloat(player, Setting::RiskMaxFightCostPercent) * float(sSquadService.CountTogether(player, SquadRiskYards));
}

namespace
{
    // A place a character died at (ga_bot_death_place).
    struct DeathPlace
    {
        uint16 slot = 0;
        uint32 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        uint8 level = 0;
        uint16 deaths = 0;
        uint32 lastDeath = 0;      // unix time
        uint32 setAsideUntil = 0;  // unix time, 0 = not set aside
    };
    std::unordered_map<ObjectGuid::LowType, std::vector<DeathPlace>> deathPlaces;

    void SavePlace(ObjectGuid guid, DeathPlace const& place)
    {
        CharacterDatabase.Execute("REPLACE INTO ga_bot_death_place (bot_guid, slot, map, x, y, level, deaths, last_death, set_aside_until) "
            "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {})", guid.GetCounter(), place.slot, place.mapId, place.x, place.y,
            uint32(place.level), place.deaths, place.lastDeath, place.setAsideUntil);
    }
}

namespace
{
    struct FightStats
    {
        float fightCost = 0.4f;
        float healthRate = 0.02f;
        float runFactor = 1.0f;
        uint32 fights = 0;
    };
    std::unordered_map<ObjectGuid::LowType, FightStats> fightStats;
    std::unordered_map<ObjectGuid::LowType, std::unordered_map<uint32, BotRuntime::FoeNote>> foeCosts;

    // What each character learned of a kind, as a share of its average fight, by class and kind (Risk::KindRatio).
    // A kind a bot has not fought was judged by the bot's average: two Shadowmages of the Eastern Plaguelands came to
    // "about 45 %" for a level-60 warrior that had never fought a pair of them, and it died to them, round 172; the
    // guild's other warriors had learned what such a pair costs them.
    std::unordered_map<uint64, std::unordered_map<ObjectGuid::LowType, float>> kindRatios;
    constexpr uint16 KindRatioFights = 2;  // a note counts once it has this many fights behind it (or KindRatioAtOnce)
    constexpr float KindRatioAtOnce = 0.8f;

    // What each character learned of two foes at once (Risk::PairShare), and the mean of its class (recomputed when a
    // value changes).
    struct PairNote
    {
        uint8 playerClass = 0;
        float share = 0.5f;
        uint16 fights = 0;
    };
    std::unordered_map<ObjectGuid::LowType, PairNote> pairNotes;
    std::unordered_map<uint8, std::pair<float, uint32>> pairClassShares; // class -> mean, characters
    constexpr uint16 PairShareFights = 3;

    void UpdatePairClass(uint8 playerClass)
    {
        if (!playerClass)
            return;
        float sum = 0.0f;
        uint32 count = 0;
        for (auto const& [guid, note] : pairNotes)
            if (note.playerClass == playerClass && note.fights >= PairShareFights)
            {
                sum += note.share;
                ++count;
            }
        if (count)
            pairClassShares[playerClass] = { sum / float(count), count };
        else
            pairClassShares.erase(playerClass);
    }

    uint64 KindKey(uint8 playerClass, uint32 entry)
    {
        return (uint64(playerClass) << 32) | uint64(entry);
    }

    void NoteKindRatio(ObjectGuid::LowType guid, uint8 playerClass, uint32 entry, BotRuntime::FoeNote const& note)
    {
        if (!playerClass || !entry)
            return;
        auto& ratios = kindRatios[KindKey(playerClass, entry)];
        if (note.fights < KindRatioFights && note.cost < KindRatioAtOnce)
        {
            ratios.erase(guid);
            return;
        }
        auto stats = fightStats.find(guid);
        float const average = stats != fightStats.end() ? std::max(stats->second.fightCost, 0.05f) : 0.15f;
        ratios[guid] = note.cost / average;
    }
}

bool Risk::RestoreFightStats(BotRuntime& rt, ObjectGuid guid)
{
    auto itr = fightStats.find(guid.GetCounter());
    if (itr == fightStats.end())
        return false;
    rt.fightCost = std::clamp(itr->second.fightCost, 0.05f, 1.0f);
    rt.fightHealthRate = std::clamp(itr->second.healthRate, 0.005f, 0.2f);
    // (Not below 1: factors learned against the first, three times higher estimate of a run would understate it.)
    rt.runFactor = std::clamp(itr->second.runFactor, 1.0f, 4.0f);
    rt.fightsLearned = itr->second.fights;
    if (auto notes = foeCosts.find(guid.GetCounter()); notes != foeCosts.end())
        rt.foeCost = notes->second;
    if (auto pair = pairNotes.find(guid.GetCounter()); pair != pairNotes.end())
    {
        rt.pairShare = pair->second.share;
        rt.pairFights = pair->second.fights;
    }
    return true;
}

float Risk::PairShare(uint8 playerClass)
{
    auto itr = pairClassShares.find(playerClass);
    return itr == pairClassShares.end() ? -1.0f : itr->second.first;
}

void Risk::SavePairShare(ObjectGuid guid, uint8 playerClass, float share, uint16 fights)
{
    PairNote& note = pairNotes[guid.GetCounter()];
    note.playerClass = playerClass ? playerClass : note.playerClass;
    note.share = share;
    note.fights = fights;
    UpdatePairClass(note.playerClass);
    CharacterDatabase.Execute("REPLACE INTO ga_bot_pair_share (bot_guid, share, fights, updated_at) VALUES ({}, {}, {}, {})",
        guid.GetCounter(), share, uint32(fights), uint32(GameTime::GetGameTime().count()));
}

std::string Risk::DescribePairShares()
{
    static char const* const names[] = { "", "warrior", "paladin", "hunter", "rogue", "priest", "death knight", "shaman", "mage",
        "warlock", "", "druid" };
    std::string text;
    for (uint8 playerClass = 1; playerClass <= 11; ++playerClass)
        if (auto itr = pairClassShares.find(playerClass); itr != pairClassShares.end())
            text += Acore::StringFormat("{}{} {:.2f} ({} character{})", text.empty() ? "" : ", ", names[playerClass],
                itr->second.first, itr->second.second, itr->second.second == 1 ? "" : "s");
    return text.empty() ? std::string("none learned yet (0.5 for every class)") : text;
}

void Risk::SaveFoeCost(ObjectGuid guid, uint32 entry, float cost, uint16 fights, uint8 playerClass)
{
    BotRuntime::FoeNote& note = foeCosts[guid.GetCounter()][entry];
    note.cost = cost;
    note.fights = fights;
    NoteKindRatio(guid.GetCounter(), playerClass, entry, note);
    CharacterDatabase.Execute("REPLACE INTO ga_bot_foe_cost (bot_guid, entry, cost, fights, updated_at) VALUES ({}, {}, {}, {}, {})",
        guid.GetCounter(), entry, cost, uint32(fights), uint32(GameTime::GetGameTime().count()));
}

float Risk::KindRatio(uint8 playerClass, uint32 entry, ObjectGuid self)
{
    auto itr = kindRatios.find(KindKey(playerClass, entry));
    if (itr == kindRatios.end())
        return 0.0f;
    // The middle one, not the mean: one character whose average is the floor (0.15) or that died to the kind once
    // drew the class's ratio up for every other. A level-53 shaman judged two level-49 Felpaw Scavengers it could not
    // take by its class's mean of 1.95 (one shaman's note stood at 3.8 times its floored average; the others at 0.9
    // and 1.3), kept away from them on its way four times in a minute and went home by hearthstone, 13 km from its
    // zone (Mojowind, 2026-11-26).
    std::vector<float> ratios;
    for (auto const& [guid, ratio] : itr->second)
        if (guid != self.GetCounter())
            ratios.push_back(ratio);
    if (ratios.empty())
        return 0.0f;
    std::sort(ratios.begin(), ratios.end());
    std::size_t const middle = ratios.size() / 2;
    return ratios.size() % 2 ? ratios[middle] : (ratios[middle - 1] + ratios[middle]) / 2.0f;
}

void Risk::SaveFightStats(ObjectGuid guid, BotRuntime const& rt)
{
    FightStats& stats = fightStats[guid.GetCounter()];
    stats.fightCost = rt.fightCost;
    stats.healthRate = rt.fightHealthRate;
    stats.runFactor = rt.runFactor;
    stats.fights = rt.fightsLearned;
    CharacterDatabase.Execute("REPLACE INTO ga_bot_fight_stats (bot_guid, fight_cost, health_rate, run_factor, fights, updated_at) "
        "VALUES ({}, {}, {}, {}, {}, {})", guid.GetCounter(), rt.fightCost, rt.fightHealthRate, rt.runFactor, rt.fightsLearned,
        uint32(GameTime::GetGameTime().count()));
}

void Risk::Load()
{
    fightStats.clear();
    if (QueryResult result = CharacterDatabase.Query("SELECT bot_guid, fight_cost, health_rate, run_factor, fights FROM ga_bot_fight_stats"))
        do
        {
            Field* fields = result->Fetch();
            FightStats& stats = fightStats[fields[0].Get<uint32>()];
            stats.fightCost = fields[1].Get<float>();
            stats.healthRate = fields[2].Get<float>();
            stats.runFactor = fields[3].Get<float>();
            stats.fights = fields[4].Get<uint32>();
        } while (result->NextRow());

    foeCosts.clear();
    kindRatios.clear();
    if (QueryResult result = CharacterDatabase.Query("SELECT f.bot_guid, f.entry, f.cost, f.fights, c.class FROM ga_bot_foe_cost f "
        "LEFT JOIN characters c ON c.guid = f.bot_guid"))
        do
        {
            Field* fields = result->Fetch();
            BotRuntime::FoeNote& note = foeCosts[fields[0].Get<uint32>()][fields[1].Get<uint32>()];
            note.cost = fields[2].Get<float>();
            note.fights = uint16(std::min<uint32>(fields[3].Get<uint32>(), 65535));
            NoteKindRatio(fields[0].Get<uint32>(), fields[4].IsNull() ? 0 : fields[4].Get<uint8>(), fields[1].Get<uint32>(), note);
        } while (result->NextRow());

    pairNotes.clear();
    pairClassShares.clear();
    if (QueryResult result = CharacterDatabase.Query("SELECT p.bot_guid, p.share, p.fights, c.class FROM ga_bot_pair_share p "
        "LEFT JOIN characters c ON c.guid = p.bot_guid"))
        do
        {
            Field* fields = result->Fetch();
            PairNote& note = pairNotes[fields[0].Get<uint32>()];
            note.share = fields[1].Get<float>();
            note.fights = uint16(std::min<uint32>(fields[2].Get<uint32>(), 65535));
            note.playerClass = fields[3].IsNull() ? 0 : fields[3].Get<uint8>();
        } while (result->NextRow());
    for (uint8 playerClass = 1; playerClass <= 11; ++playerClass)
        UpdatePairClass(playerClass);
    LOG_INFO("module", "mod-guild-adventure: two foes at once, the share of a second foe by class: {}", DescribePairShares());

    deathPlaces.clear();
    uint32 const now = uint32(GameTime::GetGameTime().count());
    // What is neither set aside any more nor a recent death is of no use.
    CharacterDatabase.Execute("DELETE FROM ga_bot_death_place WHERE set_aside_until < {} AND last_death < {}", now,
        now > GuildPlaceSeconds ? now - GuildPlaceSeconds : 0);
    uint32 count = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT bot_guid, slot, map, x, y, level, deaths, last_death, set_aside_until "
        "FROM ga_bot_death_place"))
        do
        {
            Field* fields = result->Fetch();
            DeathPlace place;
            place.slot = fields[1].Get<uint16>();
            place.mapId = fields[2].Get<uint16>();
            place.x = fields[3].Get<float>();
            place.y = fields[4].Get<float>();
            place.level = fields[5].Get<uint8>();
            place.deaths = fields[6].Get<uint16>();
            place.lastDeath = fields[7].Get<uint32>();
            place.setAsideUntil = fields[8].Get<uint32>();
            deathPlaces[fields[0].Get<uint32>()].push_back(place);
            ++count;
        } while (result->NextRow());
    LOG_INFO("module", "mod-guild-adventure: loaded {} place{} where guild characters died", count, count == 1 ? "" : "s");
}

uint32 Risk::RestorePlaces(BotRuntime& rt, ObjectGuid guid)
{
    auto itr = deathPlaces.find(guid.GetCounter());
    if (itr == deathPlaces.end())
        return 0;
    uint32 const now = uint32(GameTime::GetGameTime().count());
    uint32 restored = 0;
    for (DeathPlace const& place : itr->second)
        if (place.setAsideUntil > now)
        {
            rt.AddAvoidArea(place.mapId, place.x, place.y, place.level,
                uint32(std::min<uint64>(uint64(place.setAsideUntil - now) * IN_MILLISECONDS, MaxPlaceAreaMs)), true);
            ++restored;
        }
    return restored;
}

uint8 Risk::FoeLevel(SpawnPoint const& spot)
{
    if (spot.gameObject || !spot.entry)
        return 0;
    CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(spot.entry);
    return info ? info->maxlevel : 0;
}

uint8 Risk::ThreatLevel(Player const* player, SpawnPoint const& spot)
{
    if (spot.gameObject || !spot.entry)
        return 0;
    CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(spot.entry);
    return info && WorldIndex::IsAttackableFaction(info->faction, player) ? info->maxlevel : 0;
}

namespace
{
    // The danger of a spot and the level of the strongest mob standing around it.
    float SpotDangerAndPack(Player const* player, SpawnPoint const& spot, uint8& strongestNeighbour);
}

float Risk::SpotDanger(Player const* player, SpawnPoint const& spot)
{
    uint8 strongest = 0;
    return SpotDangerAndPack(player, spot, strongest);
}

namespace
{
float SpotDangerAndPack(Player const* player, SpawnPoint const& spot, uint8& strongestNeighbour)
{
    float danger = 0.0f;
    int32 const level = player->GetLevel();

    // The pack around it.
    ForEachMobNear(spot.mapId, spot.x, spot.y, PackRadius, [&](MobSpawn const& mob)
    {
        if (!spot.gameObject && mob.spawn.spawnId == spot.spawnId)
            return;
        // Creatures that do not attack by themselves (the yellow ones of a starting area) are no pack.
        if (std::fabs(mob.spawn.z - spot.z) > PackRadius || !IsAggressive(mob, player))
            return;
        // Mobs below the bot come only from close by and die fast; mobs above it weigh more.
        int32 const mobLevel = EffectiveLevel(mob);
        int32 const below = level - mobLevel;
        danger += below <= 0 ? 1.0f : (below == 1 ? 0.6f : (below == 2 ? 0.3f : 0.1f));
        if (mobLevel > level)
            danger += 0.5f * float(mobLevel - level);
        strongestNeighbour = std::max(strongestNeighbour, uint8(mobLevel));
    });

    // The target itself (when the bot fights it).
    if (int32 const foe = Risk::ThreatLevel(player, spot); foe > level)
        danger += float(foe - level);

    if (IsIndoors(player, spot))
        danger += CaveDanger;
    return danger;
}
}

std::optional<SpawnPoint> Risk::SaferSpot(std::vector<SpawnPoint> const& spawns, Player const* player, BotRuntime const& rt,
    SpawnPoint const& nearest, uint32 zoneId)
{
    if (!sBotSettings.IsOn(player, Setting::RiskEnable))
        return nearest;

    [[maybe_unused]] uint8 const level = player->GetLevel();
    // Usable: the target is not above the level cap, neither is anything standing around it (one level more is
    // allowed there), the bot does not avoid the place, and the fight the spot means (the target and what would join)
    // costs it no more than it should by its own estimate. A fixed margin (no pack unless two levels above it, no foe
    // above its own level) left a level-3 character without any quest work in its starting area. `danger` gets the
    // spot's danger for the ranking.
    // A squad takes on more: the limit grows with the characters that stand together in it (Squads.h).
    auto usable = [&](SpawnPoint const& spawn, float& danger)
    {
        // The verdict every planner path uses (JudgeSpot): the level cap, what stands around, the fight's cost.
        if (!JudgeSpot(player, rt, spawn).usable)
            return false;
        // An object to use in the middle of a camp the bot cannot take: it stands there for the seconds of the use, among
        // the camp, and no careful pull draws the creatures off one by one first. Wartank (48) opened the Stolen Cargo
        // in the middle of the Southsea camp of Tanaris and died with four on it: it had never fought a Southsea
        // Swashbuckler, so the fight's verdict took its average cost of 15 % for the five of them (24 %, learned on its
        // way in). The camp's verdict does not hang on the costs of single kinds. A partner or a squad makes it workable
        // (CanFight counts them).
        if (spawn.gameObject && spawn.spawnId && InCampItCannotTake(player, rt, spawn.mapId, spawn.x, spawn.y, spawn.z))
            return false;
        uint8 strongest = 0;
        danger = SpotDangerAndPack(player, spawn, strongest);
#ifdef GA_RISK_ROAD_MAP
        // The straight way there must not lead through creatures far above the bot (a level-13 rogue walked 520 yards
        // through level-16 gnolls to its quest spot); longer walks are routed along roads by the planner.
        if (spawn.mapId == player->GetMapId() &&
            Dist2d(player->GetPositionX(), player->GetPositionY(), spawn.x, spawn.y) <= StraightApproachYards &&
            sRoadMap.DangerAlong(spawn.mapId, player->GetPositionX(), player->GetPositionY(), spawn.x, spawn.y, player->GetTeamId()) >=
                level + OutmatchedLevels)
            return false;
        // A camp is worked from its edge: no spawn whose straight approach leads into the middle of a camp the bot
        // cannot take (EntersCamp: from where the bot stands to OwnCompanyYards before the spawn; the spawn's own
        // company is the verdict's matter). A druid and its partner were sent to a Kurzen Jungle Fighter in the
        // middle of the Kurzen compound, two rings of packs away: two walks around, a keep-away five yards from a
        // pack, the task ended, the next spawn of the same camp, the same again.
        // (The first build asked the road map for any two creatures within 18 yards of the approach. In a field of
        // beasts that is everywhere: "Tiger Mastery", "Panther Mastery", "Raptor Mastery" and the basilisks of
        // "Singing Blue Shards" waited for one to five levels, 66 "every spot of it near is too much" lines in one
        // live window. A camp is what CampAtPoint calls one: beasts and single creatures are none.)
        // (The rule held for spawns more than 70 yards away only, and not for a bot that stood in the camp: a pair
        // pulled the compound's outer Kurzen one by one, each next spawn 30 to 60 yards further in, and stood in its
        // middle a minute later. Now from 35 yards on; and a bot that stands in the middle of such a camp gets no
        // spawn that lies in the middle too: it goes out first, Escape::Step.)
        constexpr float OwnCompanyYards = 30.0f;
        constexpr float OwnGroupYards = 10.0f;
        if (spawn.mapId == player->GetMapId())
        {
            float const distance = Dist2d(player->GetPositionX(), player->GetPositionY(), spawn.x, spawn.y);
            CampAt camp;
            float at = 0.0f;
            if (InCampItCannotTake(player, rt, spawn.mapId, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ()))
            {
                if (InCampItCannotTake(player, rt, spawn.mapId, spawn.x, spawn.y, spawn.z, &camp))
                {
                    LOG_DEBUG("module.ga.activity", "activity: {} leaves the spawn at ({:.0f}, {:.0f}) alone: it lies in the middle of "
                        "the camp the bot stands in ({} spawns of level {}-{})", player->GetName(), spawn.x, spawn.y, uint32(camp.count),
                        uint32(camp.minLevel), uint32(camp.maxLevel));
                    return false;
                }
            }
            // A spawn in the middle of such a camp, at any distance: within 35 yards of the bot it was taken, and a
            // paladin of level 52 worked the Jaedenar Cultists of "Forces of Jaedenar" one spawn after the other into
            // the middle of their camp (each next one less than 35 yards on), where every way out had creatures on
            // it, and died against four of them.
            else if (InCampItCannotTake(player, rt, spawn.mapId, spawn.x, spawn.y, spawn.z, &camp))
            {
                LOG_DEBUG("module.ga.activity", "activity: {} leaves the spawn at ({:.0f}, {:.0f}) alone: it lies in the middle of a camp "
                    "it cannot take ({} spawns of level {}-{})", player->GetName(), spawn.x, spawn.y, uint32(camp.count),
                    uint32(camp.minLevel), uint32(camp.maxLevel));
                return false;
            }
            else if (distance > OwnCompanyYards + 5.0f &&
                EntersCamp(player, rt, spawn.mapId, player->GetPositionX(), player->GetPositionY(), spawn.x, spawn.y, &camp, &at,
                    OwnGroupYards, OwnCompanyYards))
            {
                LOG_DEBUG("module.ga.activity", "activity: {} leaves the spawn at ({:.0f}, {:.0f}) alone: its approach leads into the "
                    "middle of a camp ({} spawns of level {}-{}) after {:.0f} of {:.0f} yd", player->GetName(), spawn.x, spawn.y,
                    uint32(camp.count), uint32(camp.minLevel), uint32(camp.maxLevel), at, distance);
                return false;
            }
        }
#endif
        return true;
    };

    float const nearestDistance = Dist2d(player->GetPositionX(), player->GetPositionY(), nearest.x, nearest.y);
    float nearestDanger = 0.0f;
    bool const nearestUsable = usable(nearest, nearestDanger);
    if (nearestUsable && nearestDanger <= CalmDanger)
        return nearest;

    // The other spawns in reach, nearest first.
    std::vector<std::pair<float, SpawnPoint const*>> candidates;
    for (SpawnPoint const& spawn : spawns)
    {
        if (spawn.mapId != player->GetMapId() || rt.IsAvoided(spawn))
            continue;
        float const distance = Dist2d(player->GetPositionX(), player->GetPositionY(), spawn.x, spawn.y);
        if (distance > nearestDistance + SaferSpotExtraDistance)
            continue;
        candidates.emplace_back(distance, &spawn);
    }
    std::sort(candidates.begin(), candidates.end(), [](auto const& a, auto const& b)
    {
        return a.first < b.first || (a.first == b.first && a.second->spawnId < b.second->spawnId);
    });
    if (candidates.size() > MaxSpotCandidates)
        candidates.resize(MaxSpotCandidates);

    std::optional<SpawnPoint> best;
    float bestCost = 0.0f;
    if (nearestUsable)
    {
        best = nearest;
        bestCost = nearestDistance + nearestDanger * YardsPerDangerPoint;
    }
    Region const region = GetRegion(player);
    for (auto const& [distance, spawn] : candidates)
    {
        float danger = 0.0f;
        if (!usable(*spawn, danger))
            continue;
        float const cost = distance + danger * YardsPerDangerPoint;
        if (best && cost >= bestCost)
            continue;
        // Only spots the bot can walk to, and inside the zone the planner works in.
        if (sWorldIndex.GetSpawnRegion(*spawn) != region || (zoneId && sWorldIndex.GetSpawnZone(*spawn) != zoneId))
            continue;
        best = *spawn;
        bestCost = cost;
    }
    return best;
}

uint8 Risk::WayDanger([[maybe_unused]] Player const* player, [[maybe_unused]] uint32 mapId, [[maybe_unused]] float x,
    [[maybe_unused]] float y, [[maybe_unused]] float z)
{
#ifdef GA_RISK_ROAD_MAP
    if (mapId != player->GetMapId())
        return 0;
    if (Dist2d(player->GetPositionX(), player->GetPositionY(), x, y) > StraightApproachYards)
    {
        RoadRoute const route = sRoadMap.FindRoute(mapId, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), x, y, z,
            player->GetTeamId(), player->GetLevel());
        if (route.found)
            return route.danger;
    }
    // (With the heights of both ends: what only swims does not count for a walker above the water. From the end of
    // Theramore's pier the town's flight master was "behind level-46 creatures", the Coral Sharks under the planks,
    // and a level-31 bot had "no flight master it can walk to".)
    return sRoadMap.DangerAlong(mapId, player->GetPositionX(), player->GetPositionY(), x, y, player->GetTeamId(), 0.0f,
        player->GetPositionZ(), z);
#else
    return 0;
#endif
}

namespace
{
    // CampAtPoint's answers: key = map, team, height band and the 12-yard cell of the point.
    constexpr float CampCellYards = 12.0f;
    constexpr float CampAroundYards = 55.0f;
    constexpr float CampNearYards = 30.0f;
    constexpr float CampTogetherYards = 12.0f;
    constexpr float CampHeightYards = 30.0f;
    constexpr std::size_t CampMinSpawns = 4;
    constexpr std::size_t CampMinNear = 2;
    constexpr float CampOpenGap = 150.0f * 3.14159265f / 180.0f;
    constexpr int32 CampLevelsBelow = 3;
    std::unordered_map<uint64, Risk::CampAt> campCells;

    struct CampMember
    {
        float x;
        float y;
        uint8 level;
        uint32 entry;
    };

    // The camp test on the members of at least `fromLevel`, around (px, py).
    bool IsCampOf(std::vector<CampMember> const& all, uint8 fromLevel, float px, float py, Risk::CampAt& camp)
    {
        std::vector<CampMember const*> members;
        for (CampMember const& member : all)
            if (member.level >= fromLevel)
                members.push_back(&member);
        if (members.size() < CampMinSpawns)
            return false;
        std::size_t together = 0;
        std::size_t closeBy = 0;
        std::vector<float> bearings;
        std::vector<uint8> levels;
        float sx = 0.0f, sy = 0.0f;
        for (CampMember const* member : members)
        {
            for (CampMember const* other : members)
                if (member != other && std::hypot(member->x - other->x, member->y - other->y) <= CampTogetherYards)
                {
                    ++together;
                    break;
                }
            if (std::hypot(member->x - px, member->y - py) <= CampNearYards)
                ++closeBy;
            bearings.push_back(std::atan2(member->y - py, member->x - px));
            levels.push_back(member->level);
            sx += member->x;
            sy += member->y;
        }
        if (together * 2 < members.size() || closeBy < CampMinNear)
            return false;
        std::sort(bearings.begin(), bearings.end());
        float widest = bearings.front() + 2.0f * 3.14159265f - bearings.back();
        for (std::size_t i = 1; i < bearings.size(); ++i)
            widest = std::max(widest, bearings[i] - bearings[i - 1]);
        if (widest >= CampOpenGap)
            return false;
        std::sort(levels.begin(), levels.end());
        camp.inside = true;
        camp.minLevel = levels.front();
        camp.maxLevel = levels.back();
        camp.level = levels[levels.size() / 2];
        camp.walkerLevel = uint8(std::min<int32>(255, int32(fromLevel) + CampLevelsBelow));
        camp.count = uint16(std::min<std::size_t>(members.size(), 65535));
        // The kinds most of them are (CannotTake: what the walker learned of them).
        std::unordered_map<uint32, uint32> kinds;
        for (CampMember const* member : members)
            ++kinds[member->entry];
        std::vector<std::pair<uint32, uint32>> byCount(kinds.begin(), kinds.end());
        std::sort(byCount.begin(), byCount.end(), [](auto const& a, auto const& b)
            { return a.second != b.second ? a.second > b.second : a.first < b.first; });
        for (std::size_t k = 0; k < byCount.size() && k < 2; ++k)
            camp.kinds[k] = byCount[k].first;
        camp.x = sx / float(members.size());
        camp.y = sy / float(members.size());
        camp.radius = 0.0f;
        for (CampMember const* member : members)
            camp.radius = std::max(camp.radius, std::hypot(member->x - camp.x, member->y - camp.y));
        return true;
    }
}

Risk::CampAt Risk::CampAtPoint(uint32 mapId, float x, float y, TeamId team, float z)
{
    int32 const cx = int32(std::floor(x / CampCellYards));
    int32 const cy = int32(std::floor(y / CampCellYards));
    uint32 const band = z == NoHeight ? 0u : 1u + uint32(std::clamp(int32(std::floor((z + 4000.0f) / CampHeightYards)), 0, 500));
    uint64 const key = (uint64(mapId & 0xFFF) << 52) | (uint64(team == TEAM_HORDE ? 1 : 0) << 51) | (uint64(band) << 42) |
        (uint64(uint32(cx + 0x100000) & 0x1FFFFF) << 21) | uint64(uint32(cy + 0x100000) & 0x1FFFFF);
    if (auto known = campCells.find(key); known != campCells.end())
        return known->second;

    float const px = (float(cx) + 0.5f) * CampCellYards;
    float const py = (float(cy) + 0.5f) * CampCellYards;
    std::vector<CampMember> members;
    ForEachMobNear(mapId, px, py, CampAroundYards, [&](MobSpawn const& mob)
    {
        if (mob.patrol || mob.rare || (z != NoHeight && std::fabs(mob.spawn.z - z) > CampHeightYards))
            return;
        if (std::hypot(mob.spawn.x - px, mob.spawn.y - py) > CampAroundYards)
            return;
        // Beasts make no camp: where they stand thick (the panthers of northern Stranglethorn, 25 around one
        // point, half of them by chance within 12 yards of another) it is a field to hunt in, one or two at a time,
        // and the look for packs on the way sees the two.
        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(mob.spawn.entry);
        if (info && (info->type == CREATURE_TYPE_BEAST || info->type == CREATURE_TYPE_CRITTER))
            return;
        if (!WorldIndex::IsFriendlyFaction(mob.factionTemplate, team))
            members.push_back({ mob.spawn.x, mob.spawn.y, mob.maxLevel, mob.spawn.entry });
    });

    // The highest level from which the creatures still make a camp around the point: for walkers up to three
    // levels above it this is one (a single strong creature beside a camp of weak ones makes no camp of its level).
    CampAt camp;
    if (members.size() >= CampMinSpawns)
    {
        std::vector<uint8> levels;
        for (CampMember const& member : members)
            levels.push_back(member.level);
        std::sort(levels.begin(), levels.end(), std::greater<uint8>());
        levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
        for (uint8 fromLevel : levels)
            if (IsCampOf(members, fromLevel, px, py, camp))
                break;
    }
    campCells.emplace(key, camp);
    return camp;
}

bool Risk::LegEntersCamp(uint32 mapId, float fromX, float fromY, float toX, float toY, TeamId team, uint8 walkerLevel, CampAt* camp,
    float* atYards, float skipStart, float stopBefore, float z)
{
    constexpr float StepYards = 10.0f;
    float const dx = toX - fromX;
    float const dy = toY - fromY;
    float const length = std::sqrt(dx * dx + dy * dy);
    float const end = length - stopBefore;
    for (float along = skipStart; along <= end + 0.01f; along += StepYards)
    {
        float const share = length > 0.1f ? along / length : 0.0f;
        CampAt const here = CampAtPoint(mapId, fromX + dx * share, fromY + dy * share, team, z);
        if (!here.inside || (walkerLevel && walkerLevel > here.walkerLevel))
            continue;
        if (camp)
            *camp = here;
        if (atYards)
            *atYards = along;
        return true;
    }
    return false;
}

namespace
{
    // A camp the character has to keep out of: its creatures are not far below it, and three of them at once are
    // more than it (and the squad beside it) can take.
    bool CannotTake(Player const* player, BotRuntime const& rt, Risk::CampAt const& camp)
    {
        // (In a big camp it is four: in one night the Kurzen compound, 15 to 18 spawns around any point of its
        // middle, put four on a level-36 warrior, four on a level-31 warrior and four on a level-35 hunter; its
        // Medicine Men heal, the fight lasts, the next ones come.)
        constexpr uint32 CampFoesAtOnce = 3;
        constexpr uint32 BigCampFoesAtOnce = 4;
        constexpr uint16 BigCampSpawns = 10;
        if (!camp.inside)
            return false;
        // What the walker learned of the camp's kinds, against its average: a camp of a kind that costs it more is
        // judged by that cost, and also when its creatures are more than three levels below it (walkerLevel). The
        // Unliving Mossflayers of Zaeldarr's camp (54-55) were "too far below" a level-60 priest to count, while one
        // of them cost it 33 % against its average of 24 %: its grind walk led through their middle, three were on it
        // and it died (round 249). A kind it found cheap does not make it bolder (only upwards).
        constexpr float CostlierKind = 1.2f;
        uint32 costliest = 0;
        float factor = 1.0f;
        float const average = Escape::FoeCost(rt, player->GetLevel(), 0);
        for (uint32 kind : camp.kinds)
            if (kind && average > 0.0f)
                if (float const own = Escape::FoeCost(rt, player->GetLevel(), 0, kind) / average; own > factor)
                {
                    factor = own;
                    costliest = kind;
                }
        if (player->GetLevel() > camp.walkerLevel && factor < CostlierKind)
            return false;
        return !Risk::CanFight(player, rt, camp.level, std::min<uint32>(camp.count, camp.count >= BigCampSpawns ? BigCampFoesAtOnce :
            CampFoesAtOnce), costliest);
    }
}

bool Risk::EntersCamp(Player const* player, BotRuntime const& rt, uint32 mapId, float fromX, float fromY, float toX, float toY,
    CampAt* camp, float* atYards, float skipStart, float stopBefore)
{
    if (!sBotSettings.IsOn(player, Setting::RiskEnable))
        return false;
    constexpr float StepYards = 10.0f;
    float const dx = toX - fromX;
    float const dy = toY - fromY;
    float const length = std::sqrt(dx * dx + dy * dy);
    float const end = length - stopBefore;
    // (The height of the walker counts when the leg is on its map: a camp in a cave below is none on the hill.)
    float const z = mapId == player->GetMapId() ? player->GetPositionZ() : NoHeight;
    for (float along = skipStart; along <= end + 0.01f; along += StepYards)
    {
        float const share = length > 0.1f ? along / length : 0.0f;
        CampAt const here = CampAtPoint(mapId, fromX + dx * share, fromY + dy * share, player->GetTeamId(), z);
        if (!CannotTake(player, rt, here))
            continue;
        if (camp)
            *camp = here;
        if (atYards)
            *atYards = along;
        return true;
    }
    return false;
}

bool Risk::InCampItCannotTake(Player const* player, BotRuntime const& rt, uint32 mapId, float x, float y, float z, CampAt* camp)
{
    if (!sBotSettings.IsOn(player, Setting::RiskEnable))
        return false;
    CampAt const here = CampAtPoint(mapId, x, y, player->GetTeamId(), z);
    if (!CannotTake(player, rt, here))
        return false;
    if (camp)
        *camp = here;
    return true;
}

bool Risk::OnStrongRoad(Player const* player, BotRuntime const& rt, uint32 mapId, float x, float y, float z, float yards,
    std::string* who)
{
    if (!player || !sBotSettings.IsOn(player, Setting::RiskEnable))
        return false;
    int32 const level = player->GetLevel();
    float const limit = sBotSettings.GetFloat(player, Setting::RiskMaxFightCostPercent);
    // (Or one of a pack on the road that the bot cannot take together: the Crimson Courier and its four bodyguards
    // walk paths of their own side by side, none of them too strong alone, and a level-60 shaman rested 33 yd from
    // their road after a walk of 58 yd to a quieter place and died to the five of them, round 187.)
    std::vector<PatrolNear> const patrols = PatrolsNear(mapId, x, y, z, yards, PullHeight);
    std::vector<uint8> const packs = PatrolPacks(player, rt, patrols, level, limit);
    for (std::size_t p = 0; p < patrols.size(); ++p)
    {
        MobSpawn const& mob = *patrols[p].mob;
        if (!IsAggressive(mob, player))
            continue;
        bool strong = false;
        SpotWeight(player, rt, mob, patrols[p].distance, level, 0, limit, &strong);
        if (!strong && !packs[p])
            continue;
        if (who)
            if (CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(mob.spawn.entry))
                *who = packs[p] ? Acore::StringFormat("{} and the {} walking with it", info->Name, packs[p] - 1) : info->Name;
        return true;
    }
    return false;
}

bool Risk::IsSafePlace(Player const* player, BotRuntime const& rt, uint32 mapId, float x, float y, float z)
{
    // (Not in the middle of a camp it cannot take: a graveyard, a vendor, an area to explore.)
    if (InCampItCannotTake(player, rt, mapId, x, y, z))
        return false;
    SpawnPoint place;
    place.gameObject = true; // no creature of its own
    place.mapId = uint16(mapId);
    place.x = x;
    place.y = y;
    place.z = z;
    return SaferSpot({}, player, rt, place, 0).has_value();
}

float Risk::RankPenalty(Player const* player, Quest const* quest, SpawnPoint const& spot)
{
    if (!sBotSettings.IsOn(player, Setting::RiskEnable))
        return 0.0f;
    float penalty = SpotDanger(player, spot) * YardsPerDangerPoint;
    if (quest)
        if (int32 const above = player->GetQuestLevel(quest) - int32(player->GetLevel()); above > 0)
            penalty += float(above) * YardsPerQuestLevel;
    return penalty;
}

uint32 Risk::HostilesNear(Player const* player, float x, float y, float radius)
{
    int32 const level = player->GetLevel();
    uint32 count = 0;
    ForEachMobNear(player->GetMapId(), x, y, radius, [&](MobSpawn const& mob)
    {
        // (Down to twelve levels below the bot, as for the creatures that are alive around it: FightEscape's
        // PackLevelsBelow. Five left a camp of creatures six levels below out of a rest's and a run's count.)
        if (int32(mob.maxLevel) < level - 11 || std::fabs(mob.spawn.z - player->GetPositionZ()) > 30.0f)
            return;
        if (WorldIndex::IsAttackableFaction(mob.factionTemplate, player))
            ++count;
    });
    return count;
}

std::vector<std::pair<float, float>> Risk::HostileSpawnsNear(Player const* player, float x, float y, float radius, int32 levelsBelow)
{
    int32 const level = player->GetLevel();
    std::vector<std::pair<float, float>> places;
    ForEachMobNear(player->GetMapId(), x, y, radius, [&](MobSpawn const& mob)
    {
        if (int32(mob.maxLevel) < level - levelsBelow || std::fabs(mob.spawn.z - player->GetPositionZ()) > 30.0f)
            return;
        if (!WorldIndex::IsFriendlyFaction(mob.factionTemplate, player))
            places.emplace_back(mob.spawn.x, mob.spawn.y);
    });
    return places;
}

float Risk::GrindScore(Player const* player, MobSpawn const& mob, uint32 neighbours, float distance)
{
    if (!sBotSettings.IsOn(player, Setting::RiskEnable))
        return float(neighbours) * 50.0f - distance; // the old rule: the densest cluster

    // Enough targets to keep busy; beyond that the cluster is a pack.
    float score = float(std::min<uint32>(neighbours, 6)) * 50.0f - float(neighbours > 8 ? neighbours - 8 : 0) * 25.0f - distance;
    score -= SpotDanger(player, mob.spawn) * 40.0f;
    if (mob.maxLevel > player->GetLevel())
        score -= 150.0f;
    return score;
}

uint32 Risk::GuardsNear(Player const* player, uint32 mapId, float x, float y, float z, float yards, uint8* strongest)
{
    // Guards down to eight levels below the bot count (were three): fights against four or more creatures five to
    // eight levels below cost 43-46% on average and ended in 9 deaths of 105 in a day, none of 100 more than eight
    // below did (Escape::CrowdTail). A frost mage of level 40 died at Dalinda Malem's side in Thunder Axe Fortress
    // against four Burning Blade of level 32, at a place with five of them within 30 yards that was not "guarded"
    // for it, because none of them was within three levels.
    constexpr int32 LevelsBelow = 8;
    constexpr float GuardHeight = 20.0f;
    int32 const level = player->GetLevel();
    uint32 count = 0;
    uint8 highest = 0;
    ForEachMobNear(mapId, x, y, yards, [&](MobSpawn const& mob)
    {
        if (std::fabs(mob.spawn.z - z) > GuardHeight || Dist2d(mob.spawn.x, mob.spawn.y, x, y) > yards || mob.rare ||
            int32(mob.maxLevel) + LevelsBelow < level || !IsAggressive(mob, player))
            return;
        ++count;
        highest = std::max(highest, EffectiveLevel(mob));
    });
    if (strongest)
        *strongest = highest;
    return count;
}

void Risk::SetPlaceAside(Player const* player, BotRuntime& rt, uint32 mapId, float x, float y, uint32 ms, uint8 level)
{
    uint32 const unixNow = uint32(GameTime::GetGameTime().count());
    std::vector<DeathPlace>& places = deathPlaces[player->GetGUID().GetCounter()];
    DeathPlace* place = nullptr;
    for (DeathPlace& known : places)
        if (known.mapId == mapId && Dist2d(known.x, known.y, x, y) <= SamePlaceRadius)
        {
            place = &known;
            break;
        }
    if (!place)
    {
        uint16 slot = 0;
        for (DeathPlace const& known : places)
            slot = std::max<uint16>(slot, uint16(known.slot + 1));
        places.push_back({ slot, mapId, x, y, 0, 0, unixNow, 0 });
        place = &places.back();
    }
    place->level = std::max<uint8>(place->level, std::max<uint8>(level, player->GetLevel()));
    place->setAsideUntil = std::max(place->setAsideUntil, unixNow + ms / IN_MILLISECONDS);
    rt.AddAvoidArea(mapId, x, y, place->level, ms, true);
    SavePlace(player->GetGUID(), *place);
}

std::string Risk::OnDeath(Player* player, BotRuntime& rt, uint8 foeLevel, uint8 foes)
{
    Settings const settings = sBotSettings.For(player);
    if (!settings.riskEnable)
        return "";

    uint32 const now = getMSTime();
    uint8 const level = player->GetLevel();
    std::erase_if(rt.recentDeaths, [now](BotRuntime::DeathMark const& mark) { return getMSTimeDiff(mark.at, now) > DeathMemoryMs; });

    rt.recentDeaths.push_back({ player->GetMapId(), player->GetPositionX(), player->GetPositionY(), now });

    // The place, kept across restarts (a camp killed one bot seven times in three hours: its deaths lay more than half
    // an hour or a server restart apart, so the count by memory never reached two).
    uint32 const unixNow = uint32(GameTime::GetGameTime().count());
    std::vector<DeathPlace>& places = deathPlaces[player->GetGUID().GetCounter()];
    DeathPlace* place = nullptr;
    for (DeathPlace& known : places)
        if (known.mapId == player->GetMapId() && unixNow - std::min(unixNow, known.lastDeath) <= PlaceMemorySeconds &&
            Dist2d(known.x, known.y, player->GetPositionX(), player->GetPositionY()) <= SamePlaceRadius)
        {
            place = &known;
            break;
        }
    if (!place)
    {
        uint16 slot = 0;
        for (DeathPlace const& known : places)
            slot = std::max<uint16>(slot, uint16(known.slot + 1));
        places.push_back({ slot, player->GetMapId(), player->GetPositionX(), player->GetPositionY(), 0, 0, 0, 0 });
        place = &places.back();
    }
    ++place->deaths;
    place->lastDeath = unixNow;
    uint32 const here = place->deaths;

    std::string note;
    uint32 areaMs = 0;
    if (foeLevel >= level + 2)
    {
        // Until the bot has the level of those foes.
        place->level = std::max<uint8>(place->level, uint8(foeLevel - 1));
        areaMs = FoeAreaMs;
        note = Acore::StringFormat("the foes there are level {}: the place is set aside until level {}", uint32(foeLevel),
            uint32(foeLevel));
        // What killed it is far above it: for as long as a place is kept (two hours were over when a level-37
        // paladin came back to the water of a level-46 shark three and a half hours after its first death there).
        constexpr uint8 FarAboveLevels = 4;
        if (foeLevel >= level + FarAboveLevels)
        {
            areaMs = MaxPlaceAreaMs;
            note = Acore::StringFormat("what killed it there is level {}, far above it: the place is set aside until level {} or for {} h",
                uint32(foeLevel), uint32(foeLevel), areaMs / (HOUR * IN_MILLISECONDS));
        }
    }
    // Three or more were on it at once: a camp. The place is set aside at once, not from the second death on (a
    // level-15 bot died to four gnolls of its level in a field of ten, was sent past the same field for a pickup four
    // minutes later and died there again; two more bots died in that field within the hour).
    constexpr uint8 CampFoes = 3;
    if (foes >= CampFoes && here < 2)
    {
        place->level = std::max<uint8>(place->level, std::max<uint8>(level, foeLevel));
        areaMs = std::max(areaMs, PlaceAreaMs);
        note = Acore::StringFormat("{} creatures were on it at once: the place is set aside until level {} or for {} min", uint32(foes),
            uint32(place->level) + 1, areaMs / (MINUTE * IN_MILLISECONDS));
    }
    if (here >= 2)
    {
        // A level-up frees the place only when it puts the bot above what killed it there; every further death there
        // doubles the time.
        place->level = std::max<uint8>(place->level, std::max<uint8>(level, foeLevel));
        // (An hour at the second death, two at the third: with half an hour a rogue was back at a murloc place 46
        // minutes after its first death there and died again.)
        areaMs = std::max(areaMs, std::min<uint32>(MaxPlaceAreaMs, PlaceAreaMs << std::min<uint32>(here - 1, 4)));
        note = Acore::StringFormat("{} deaths at this place: it is set aside until level {} or for {} min", here,
            uint32(place->level) + 1, areaMs / (MINUTE * IN_MILLISECONDS));
    }
    if (areaMs)
    {
        rt.AddAvoidArea(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), place->level, areaMs, true);
        place->setAsideUntil = unixNow + areaMs / IN_MILLISECONDS;
    }
    SavePlace(player->GetGUID(), *place);

    // A place that kills the guild's characters, one after the other: set aside for all of them. (The Twilight camp of
    // Silithus, creatures of level 60 in a hollow with one way out, killed five level-60 characters of one guild on three
    // days, each with up to eight creatures on it; it was no camp by its open sides, and each character came there on its
    // own.) A death with three or more creatures on it, and another guild character's death within 80 yd in the last two
    // days: every guild character in the world keeps away from it for eight hours or until it is above the level of what
    // died there. Two deaths of the guild's characters at one place within six hours do it too, whatever was on them (two
    // level-60 warriors died at one pair of Shadowmages in the Eastern Plaguelands four minutes apart, round 163).
    constexpr uint8 GuildPlaceFoes = 3;
    uint32 const guildWindow = foes >= GuildPlaceFoes ? GuildPlaceSeconds : PlaceMemorySeconds;
    if (AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(player->GetGUID()))
    {
        std::vector<ObjectGuid> members;
        for (auto const& [guid, bot] : guild->bots)
            members.push_back(guid);
        if (guild->leader && !guild->bots.count(guild->leader))
            members.push_back(guild->leader);
        float const x = player->GetPositionX();
        float const y = player->GetPositionY();
        uint8 placeLevel = std::max<uint8>(level, foeLevel);
        std::string others;
        for (ObjectGuid const& guid : members)
        {
            if (guid == player->GetGUID())
                continue;
            auto known = deathPlaces.find(guid.GetCounter());
            if (known == deathPlaces.end())
                continue;
            for (DeathPlace const& other : known->second)
                if (other.mapId == player->GetMapId() && other.lastDeath && unixNow - std::min(unixNow, other.lastDeath) <= guildWindow &&
                    Dist2d(other.x, other.y, x, y) <= SamePlaceRadius)
                {
                    GuildBot const* bot = guild->FindMember(guid);
                    others += Acore::StringFormat("{}{}", others.empty() ? "" : ", ", bot ? bot->name : guid.ToString());
                    placeLevel = std::max(placeLevel, other.level);
                    break;
                }
        }
        if (!others.empty())
        {
            uint32 shared = 0;
            for (ObjectGuid const& guid : members)
            {
                Player* member = guid == player->GetGUID() ? player : sGuildBotHolder.GetPlannedPlayer(guid);
                // (The runtime of a bot in the world is no const object: GetRuntime only hands it out as one.)
                BotRuntime* memberRt = guid == player->GetGUID() ? &rt :
                    const_cast<BotRuntime*>(OrderService::Instance().GetRuntime(guid));
                if (!member || !memberRt || member->GetLevel() > placeLevel)
                    continue;
                SetPlaceAside(member, *memberRt, player->GetMapId(), x, y, MaxPlaceAreaMs, placeLevel);
                ++shared;
            }
            note += Acore::StringFormat("{}{} of the guild died here too in the last {} h: the place is set aside for {} of the "
                "guild's characters until level {} or for {} h", note.empty() ? "" : "; ", others, guildWindow / HOUR, shared,
                uint32(placeLevel) + 1, MaxPlaceAreaMs / (HOUR * IN_MILLISECONDS));
        }
    }

    uint32 recent = 0;
    for (BotRuntime::DeathMark const& mark : rt.recentDeaths)
        if (getMSTimeDiff(mark.at, now) <= settings.riskCautiousWindowMinutes * MINUTE * IN_MILLISECONDS)
            ++recent;
    if (settings.riskCautiousDeaths && recent >= settings.riskCautiousDeaths && !IsCautious(rt, level))
    {
        // One level-up does not end it (a bot went straight back and died at the same camp): two do, or the time.
        rt.cautiousUntil = now + settings.riskCautiousMinutes * MINUTE * IN_MILLISECONDS;
        rt.cautiousLevel = uint8(level + 1);
        note += Acore::StringFormat("{}{} deaths in {} min: plays it safe (grinds at a safe spot) for {} min or until level {}",
            note.empty() ? "" : "; ", recent, settings.riskCautiousWindowMinutes, settings.riskCautiousMinutes, uint32(level) + 2);
    }

    // A place that keeps killing it, whatever the levels: the bot walks back to its corpse, stands up in the middle
    // of the place, and every way out leads through what killed it (a level-24 rogue died five times within sixty
    // yards in eleven minutes, to creatures two to seven levels below it; the place was set aside from the second
    // death on, and it stood in it). From the third death at one place it goes home instead.
    constexpr uint32 RescuePlaceDeaths = 3;
    if (settings.riskRescueDeaths && !rt.rescue && here >= RescuePlaceDeaths)
    {
        rt.rescue = true;
        note += Acore::StringFormat("{}it gives the place up and goes home", note.empty() ? "" : "; ");
    }
    // A death loop where the bot does not belong (the foes or the whole zone far above it): it is taken home.
    if (settings.riskRescueDeaths && !rt.rescue)
    {
        uint32 inWindow = 0;
        for (BotRuntime::DeathMark const& mark : rt.recentDeaths)
            if (getMSTimeDiff(mark.at, now) <= settings.riskRescueWindowMinutes * MINUTE * IN_MILLISECONDS)
                ++inWindow;
        ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(player->GetZoneId());
        bool const foesAbove = foeLevel >= level + RescueFoeLevels;
        bool const zoneAbove = zone && zone->minLevel > level + RescueZoneLevels;
        if ((foesAbove || zoneAbove) && inWindow >= settings.riskRescueDeaths)
        {
            rt.rescue = true;
            note += Acore::StringFormat("{}{} deaths in {} min where the {} far above level {}: it gives the place up and goes home",
                note.empty() ? "" : "; ", inWindow, settings.riskRescueWindowMinutes,
                foesAbove ? Acore::StringFormat("foes (level {}) are", uint32(foeLevel)) :
                    Acore::StringFormat("zone ({}, from level {}) is", zone->name, uint32(zone->minLevel)), uint32(level));
        }
    }
    return note;
}

bool Risk::Rescue(Player* player, BotRuntime& rt)
{
    if (!rt.rescue || player->IsAlive() || player->IsBeingTeleported() || !player->IsInWorld())
        return false;
    rt.rescue = false;

    // The spirit healer's resurrection (half health and mana, a quarter of the durability) and the hearthstone, in one
    // step: a ghost cannot use the stone, and alive at that spirit healer the bot would not live to finish the cast.
    player->ResurrectPlayer(0.5f);
    player->SpawnCorpseBones();
    player->DurabilityLossAll(0.25, true);
    player->TeleportTo(player->m_homebindMapId, player->m_homebindX, player->m_homebindY, player->m_homebindZ,
        player->GetOrientation());
    return true;
}

bool Risk::IsCautious(BotRuntime const& rt, uint8 level)
{
    return rt.cautiousUntil && level <= rt.cautiousLevel && getMSTime() < rt.cautiousUntil;
}
