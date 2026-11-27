/*
 * RiskModel: how dangerous a spot, a quest and a place are for a bot (docs/orders.md "Risk").
 *
 * The quest planner picked its targets by distance alone: the nearest spawn of a quest mob, whatever its level, however
 * many other mobs stand around it, cave or open field, and the densest mob cluster for grinding. Its death rule counted
 * deaths per quest id, so a bot that had set a quest aside died on at the same pack under the next quest's name
 * (The owner's test run, 2026-11-12: a level-9 priest died ten times in 26 minutes at one murloc camp).
 *
 * This file holds the judgement; OrderService asks it at four places:
 *   - SaferSpot / RankPenalty: which spawn of a quest target the bot works at, and how far down the ranking a risky
 *     objective goes (quests at the bot's level and lonely mobs first);
 *   - GrindScore: grind where a few mobs of the bot's level stand apart, not in the thickest pack;
 *   - OnDeath: deaths are remembered by place and foe: an area is set aside after a death to a foe two or more levels
 *     above the bot, or after two deaths at one place, and after several deaths in a short time the bot plays it safe
 *     (grinds at a safe spot) for a while;
 *   - IsCautious.
 *
 * World thread only. The world data comes from WorldIndex (mob spawns), the terrain from the map of the bot.
 */

#ifndef MOD_GUILD_ADVENTURE_RISK_MODEL_H
#define MOD_GUILD_ADVENTURE_RISK_MODEL_H

#include "WorldIndex.h"
#include <optional>
#include <string>
#include <vector>

class Player;
class Quest;

namespace GuildAdventure
{
    struct BotRuntime;

    namespace Risk
    {
        // Danger of working at a spot, in points, for the ranking of spots: 0 = a lone target of the bot's level in the
        // open. Hostile mob spawns within reach of the spot count one each (less when below the bot; creatures that
        // do not attack by themselves not at all), every level of the target above the bot counts one, a cave or
        // building one and a half.
        float SpotDanger(Player const* player, SpawnPoint const& spot);

        // What the fight at a spot costs the bot, as a share of what it has, by the estimate its own fights taught it
        // (FightEscape): the target, if it is a creature the bot fights, and the hostile creatures whose spawns lie
        // within their pull distance of it, one after the other while the rest keeps hitting. This, not a level
        // margin, decides whether a bot works at a spot (GuildAdventure.Risk.MaxFightCostPercent).
        float SpotFightCost(Player const* player, BotRuntime const& rt, SpawnPoint const& spot);

        // The places where guild characters died (table ga_bot_death_place): read at startup, and given back to a
        // character's planner state when it enters the world, so a camp that killed it stays set aside across a
        // restart. Returns how many places are set aside for it now.
        void Load();
        uint32 RestorePlaces(BotRuntime& rt, ObjectGuid guid);
        // What the character learned from its fights before a restart (ga_bot_fight_stats): its cost of a fight, the
        // health a foe takes per second, the factor of its runs. False when there is no row.
        bool RestoreFightStats(BotRuntime& rt, ObjectGuid guid);
        void SaveFightStats(ObjectGuid guid, BotRuntime const& rt);
        // What a fight with one kind of creature costs the character (ga_bot_foe_cost; RestoreFightStats gives the
        // rows back).
        void SaveFoeCost(ObjectGuid guid, uint32 entry, float cost, uint16 fights, uint8 playerClass);
        // What the other characters of that class learned of that kind, as a share of their own average fight (0: none
        // has learned it): a bot judges a kind it has not learned by it.
        float KindRatio(uint8 playerClass, uint32 entry, ObjectGuid self);
        // What two foes at once cost the characters of a class (the owner, 2026-11-26, option A: each class learns it from
        // its own fights): the mean of what each character of the class learned (BotRuntime::pairShare) once it has
        // PairShareFights fights against two foes behind it; a negative value while none has. SavePairShare keeps a
        // character's value (ga_bot_pair_share; RestoreFightStats gives it back). DescribePairShares: one line per class.
        float PairShare(uint8 playerClass);
        void SavePairShare(ObjectGuid guid, uint8 playerClass, float share, uint16 fights);
        std::string DescribePairShares();

        // The one answer to "may the bot work at this spawn or place now?", for every planner path (objectives,
        // interactions, grind spots, gather nodes): the target and the hostile spawns that would join (elites and
        // rares among them) by the bot's own estimate. Not usable when
        //   - the place is set aside (deaths, a lost fight, a pull called off there),
        //   - the target is above the level cap (Risk.MaxFoeLevelAbove), or a creature more than a level above the
        //     cap stands within 22 yards (an elite counts three levels more),
        //   - as many foes of about the bot's level would join as it leaves a fight for (Escape.MaxFoes), or
        //   - the fight would cost more than Risk.MaxFightCostPercent of what it has.
        // A squad standing together raises the last two (Squads.h). `retryLevel`: the bot's level from which the
        // spot would do (0: no level within eight helps, or the place is only set aside for a time), so a quest whose
        // every spot waits can be left until then, or given to a squad. `extraTogether`: judge it as if that many more
        // squad characters stood beside the bot (QuestPartners: would a partner make the spot workable?).
        struct SpotVerdict
        {
            bool usable = true;
            float cost = 0.0f;        // the fight there, as a share of the bot's full strength
            uint32 foes = 0;          // the target and the spawns that would join it
            uint8 strongest = 0;      // the highest level among them and what stands within 22 yards (elites: +3)
            uint8 retryLevel = 0;
            char const* reason = "";  // not usable: why, for a log line
        };
        SpotVerdict JudgeSpot(Player const* player, BotRuntime const& rt, SpawnPoint const& spot, uint32 extraTogether = 0);

        // What a spell summons that will fight the bot (the Untapped Dowsing Widget of "Gadgetzan Water Survey" calls a
        // level-47 Centipaar Tunneler out of the water hole): judged like a target at a spot, before the item is used.
        // Not usable when the creature is above the level cap, four levels or more above the bot (an elite three
        // more), or its fight would cost more than Risk.MaxFightCostPercent. Summons the bot controls (pets, totems,
        // guardians) and creatures that are not hostile to it do not count; one level of triggered spells is followed.
        struct SummonVerdict
        {
            bool usable = true;
            uint8 retryLevel = 0;     // the bot's level from which it would do (0: none within eight levels)
            std::string what;         // "Centipaar Tunneler (level 48)", for a log line
        };
        SummonVerdict JudgeSummon(Player const* player, BotRuntime const& rt, uint32 spellId);

        // For ".ga admin spot": the fight at a spot spelled out (the target, who would join, what it costs, the verdict).
        std::string DescribeSpot(Player const* player, BotRuntime const& rt, SpawnPoint const& spot);

        // True if the bot takes on `foes` creatures of that level at once by the same estimate and the level cap
        // (GuildAdventure.Risk.MaxFoeLevelAbove, one more with a pet): the range a grind should keep to. `entry`: the
        // creatures are of that kind, and what the bot learned of it counts (Escape::FoeCost); 0 = its average.
        bool CanFight(Player const* player, BotRuntime const& rt, uint8 foeLevel, uint32 foes = 1, uint32 entry = 0);

        // The level of the creature of a spawn point (the highest of its template), 0 for a game object.
        uint8 FoeLevel(SpawnPoint const& spot);
        // The same, but 0 also for a creature the player cannot attack (a quest interaction with a friendly creature:
        // only what stands around it counts).
        uint8 ThreatLevel(Player const* player, SpawnPoint const& spot);
        // A place that is no spawn (a quest POI, an area to explore): true if the bot may go there now by the rules
        // of SaferSpot (the creatures around it, places set aside after deaths, the straight way there).
        bool IsSafePlace(Player const* player, BotRuntime const& rt, uint32 mapId, float x, float y, float z);
        // The patrol path of a hostile creature too strong for the bot alone, or of a pack walking together that the bot
        // cannot take (PatrolPacks), runs within `yards` of a point (a rest's quiet place is not on its road). The
        // creature's name in `who`.
        bool OnStrongRoad(Player const* player, BotRuntime const& rt, uint32 mapId, float x, float y, float z, float yards,
            std::string* who = nullptr);

        // Camps, from the spawn data (docs/orders.md "A camp as an area"). A point lies in the middle of a camp when
        // four or more spawns of creatures hostile to the team stand within 55 yards of it, two of them within 30,
        // half of them or more with another one within 12 yards (a field of single creatures is no camp), and on
        // every side of the point (no gap of 150 degrees between two of them: a point at a camp's edge is outside).
        // Spawns, not creatures: a camp whose creatures are dead for the moment is still one. Patrols, rares and
        // beasts do not make a camp (beasts that stand thick are a field to hunt in). Answers are kept per map, team, 12-yard cell and height band. World thread only.
        constexpr float NoHeight = -100000.0f;
        struct CampAt
        {
            bool inside = false;
            // The camp holds for walkers up to this level: its creatures are at most three levels below such a
            // walker. A walker above it is in no camp there (the creatures are too far below it to count).
            uint8 walkerLevel = 0;
            uint8 level = 0;           // the middle level of the creatures that make it
            uint8 minLevel = 0;
            uint8 maxLevel = 0;
            uint16 count = 0;          // how many of them stand within 55 yards of the point
            uint32 kinds[2] = { 0, 0 }; // the two kinds most of them are (creature entries, the commonest first; 0 = none)
            float x = 0.0f;            // their middle
            float y = 0.0f;
            float radius = 0.0f;       // the farthest of them from that middle
        };
        // `z`: spawns more than 30 yards above or below it do not count (NoHeight: any height).
        CampAt CampAtPoint(uint32 mapId, float x, float y, TeamId team, float z = NoHeight);
        // The first camp a straight leg leads into, for a walker of that team and level (0: of any level, the camp
        // is then judged by camp.walkerLevel): the leg is looked at every 10 yards from `skipStart` yards after its
        // start to `stopBefore` yards before its end. `atYards`: how far along the leg. For callers without a
        // character (the road map's links and nodes; a node itself: CampAtPoint).
        bool LegEntersCamp(uint32 mapId, float fromX, float fromY, float toX, float toY, TeamId team, uint8 walkerLevel = 0,
            CampAt* camp = nullptr, float* atYards = nullptr, float skipStart = 0.0f, float stopBefore = 0.0f, float z = NoHeight);
        // The same for a character, and THE question for a leg of a walk: does this straight leg lead into the
        // middle of a camp that the character (with the squad that stands with it) cannot take? "Cannot take": the
        // fight model's verdict on three of the camp's creatures at once (Risk::CanFight), with what the character
        // learned of the camp's kinds when they cost it more than its average; such a camp counts also for a walker
        // above camp.walkerLevel. With the risk rules off for the character: never.
        bool EntersCamp(Player const* player, BotRuntime const& rt, uint32 mapId, float fromX, float fromY, float toX, float toY,
            CampAt* camp = nullptr, float* atYards = nullptr, float skipStart = 0.0f, float stopBefore = 0.0f);
        // A point in the middle of such a camp.
        bool InCampItCannotTake(Player const* player, BotRuntime const& rt, uint32 mapId, float x, float y, float z, CampAt* camp = nullptr);

        // The highest level of hostile creatures on the bot's way to a point: along the straight line for a short way,
        // along the road map's route for a long one (0 = none, another map, or the module has no road map).
        uint8 WayDanger(Player const* player, uint32 mapId, float x, float y, float z);

        // Among the spawns of a quest target around the planner's nearest pick: the one with the least danger for the
        // way it costs. No value when every candidate is too strong for the bot for now or lies in an area it avoids
        // (the objective then waits; the planner works something else).
        std::optional<SpawnPoint> SaferSpot(std::vector<SpawnPoint> const& spawns, Player const* player, BotRuntime const& rt,
            SpawnPoint const& nearest, uint32 zoneId);

        // Yards added to an objective's distance in the planner's ranking: the danger of its spot and the levels the
        // quest lies above the bot. Quests at the bot's level and lonely targets so come first.
        float RankPenalty(Player const* player, Quest const* quest, SpawnPoint const& spot);

        // Hostile mob spawns of the bot's map within `radius` yards of a point that are not far below the bot (the run
        // out of a lost fight keeps away from them, FightEscape.h).
        uint32 HostilesNear(Player const* player, float x, float y, float radius);
        // Where the spawns of creatures hostile to the character stand within `radius` of a point, those at most
        // `levelsBelow` levels below it (a camp whose creatures are dead for the moment is still a camp).
        std::vector<std::pair<float, float>> HostileSpawnsNear(Player const* player, float x, float y, float radius, int32 levelsBelow);

        // Score of a grind spot, higher is better: `neighbours` fitting mobs within the planner's cluster radius,
        // `distance` yards away. A handful of mobs standing apart beats the thickest pack.
        float GrindScore(Player const* player, MobSpawn const& mob, uint32 neighbours, float distance);

        // The bot just died (foeLevel: the level of the creature it last fought, 0 if unknown). Remembers the place and
        // sets it aside when the rules above say so. Returns a line for the log ("" = nothing was set aside).
        // `foes`: how many creatures were on the bot at once in the fight that killed it (0 = no fight).
        std::string OnDeath(Player* player, BotRuntime& rt, uint8 foeLevel, uint8 foes = 0);
        // A place the bot could not get to for what stands there (a quest giver, an ender or an object among
        // creatures it cannot take): set aside like a place it died at, and kept across restarts the same way.
        // Until the bot has a level more, or for `ms`.
        // (`level`: the level up to which the place holds for it, at least its own; 0 = its own.)
        void SetPlaceAside(Player const* player, BotRuntime& rt, uint32 mapId, float x, float y, uint32 ms, uint8 level = 0);
        // Creatures that would attack the character, not more than three levels below it, with their spawn within
        // `yards` of a point (the highest level among them in `strongest`).
        uint32 GuardsNear(Player const* player, uint32 mapId, float x, float y, float z, float yards, uint8* strongest = nullptr);

        // True while the bot plays it safe after several deaths in a short time: no quest objectives, a safe grind spot.
        bool IsCautious(BotRuntime const& rt, uint8 level);

        // Rescue from a death loop in a place far above the bot (OnDeath sets BotRuntime::rescue: several deaths within
        // minutes where the foes or the zone are far above its level, e.g. a spirit healer that stands between level-25
        // skeletons): the dead bot gives the place up like a player would, takes the spirit healer's resurrection with
        // its durability loss and goes home with its hearthstone (its bind point). True when it was taken home.
        bool Rescue(Player* player, BotRuntime& rt);
    }
}

#endif
