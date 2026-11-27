/*
 * FightEscape: an AI-played guild character leaves a fight it will lose (docs/orders.md "Leaving a lost fight").
 *
 * What kills the bots are fights against two or more foes: on the dev instance 11 % of 1034 fights, but 68 of the 91
 * deaths; a fight against one foe was lost in 2.5 % of the cases, against two in 48 %, against three or more in 91 %.
 * mod-playerbots fights every fight to the end. A player in that spot stops hitting and runs: a creature gives up when
 * it is 30 yards from its home and has not been damaged for about 11 seconds (Creature::CanCreatureAttack), a second
 * foe that was never hit gives up at the 30 yards.
 *
 *   - Step: once a tick while the bot is alive. In a fight it estimates what the rest of the fight costs (the foes die
 *     one after the other, the weakest first, and everyone still standing hits meanwhile; one foe of the bot's level
 *     costs BotRuntime::fightCost, learned from its own fights: the health it loses plus half the mana it spends) and
 *     compares that with what it has (its health, for a mana user less the emptier its mana is).
 *     Only crowded fights (two or more foes), elites and foes three or more levels above it are judged. Leaving: the
 *     bot stops attacking, its pet comes back, mod-playerbots is paused, and it runs away from the foes in legs of 45
 *     yards, each leg in the direction with the fewest hostile spawns it has a path to.
 *   - OnFightEnd: learns the cost of a fight against one foe (and of a kind from two or three of it at once).
 *
 *   - The way out: the bot remembers where it walked out of combat (a trail of points ten yards apart). The first legs
 *     of a run go back along that trail, the way it came in, as long as nothing hostile stands on it; only then, or
 *     without a trail, the direction with the fewest hostile creatures (alive now) and spawns is taken. In a camp
 *     (hostile creatures within 25 yards that are not in the fight yet) the estimate counts a fifth more per creature,
 *     so the bot leaves when the second foe joins.
 *   - Keeping away (also Step): every two seconds the bot looks for a roaming creature far above it within 35 yards
 *     (hostile, four or more levels above it, and a rare, an elite or on a patrol: Vultros in Westfall). Out of combat
 *     it steps aside, the same run in legs, until the creature is 60 yards away or 20 seconds have passed; in a fight
 *     it leaves the fight at once. The place is set aside for ten minutes.
 *
 *   - Careful pulls (the owner, 2026-11-13: [quote left out]). CheckPull judges a target for the way the bot would fight it: in melee at the target's place, a caster
 *     without a pet at its own. With company at the target's place it first looks for a creature of the same kind
 *     near it that would come alone and turns to that. Else a bot that fights in melee brings the creature to its
 *     own side when fewer would join there: it opens from a distance with the bow, gun or thrown weapon in its
 *     hands or a spell of its class and waits for the creature, or, without either, walks just into the creature's
 *     sight and a few steps back. mod-playerbots is held meanwhile; StepPull runs four times a second.
 *     BeginPull is the same pull for another caller (a dungeon run's tank): it brings `target` to `fightAt`.
 *
 * World thread only, called from OrderService::UpdateBot and OrderService::EndFight.
 */

#ifndef MOD_GUILD_ADVENTURE_FIGHT_ESCAPE_H
#define MOD_GUILD_ADVENTURE_FIGHT_ESCAPE_H

#include "Define.h"
#include <string>
#include <utility>
#include <vector>

namespace G3D
{
    class Vector3;
}

class Creature;
class Player;
class Unit;
struct CreatureTemplate;

namespace GuildAdventure
{
    struct BotRuntime;

    namespace Escape
    {
        enum class Result : uint8
        {
            None,     // not on the run
            Running,  // on the run: the planner does nothing else this tick
            Aside,    // keeping away from a roaming creature far above its level: the same for the planner
            ClearOfPack, // it has moved away from a pack on its way: the task ends, and counts against its target
            WentAround,  // it walked around a pack on its way: the task goes on
            GotAway,  // just left combat: the place of the fight is set aside
            Clear,    // just got clear of the creature it kept away from: the place is set aside
            Failed    // gave up running (the foes kept up): the fight goes on
        };

        // The level a creature counts as for the look-outs: an elite by its health and damage, any other by its level.
        uint32 StrengthOfCreature(Creature const* creature);
        // Whether a creature answers the call for help of ally against enemy: the core's Creature::CanAssistTo without
        // its look whether the creature sees the enemy, which a bot in stealth fails until its attack breaks the stealth.
        bool WouldAssist(Creature const* creature, Unit const* ally, Unit const* enemy);
        // The level a creature of that kind and level is worth by its health and damage (an elite's strength for the
        // look-outs; the risk model weighs elites near a spot by it too).
        uint32 ThreatLevelOfKind(CreatureTemplate const* info, uint32 level);

        // followsMaster: a bot that follows its owner stays in the fight with him.
        Result Step(Player* player, BotRuntime& rt, bool followsMaster);

        // Rest (OrderService::StepRest): a quieter place to sit down than where the bot stands: among the points of its
        // trail and three rings around it (0.6, 1 and 1.5 times `maxYards`) the one with the fewest hostile creatures
        // and spawns within 30 yards, if that is none at all or at least two fewer than here, with a whole path on the
        // navmesh (the spot is the path's end, so its height is the ground's) that the ground guard lets it walk.
        // False when nothing hostile is near where it stands, or no spot is that much better. `here` and `there` get the two counts for the log.
        // Spots in water are never taken; a bot that stands in water gets the quietest dry spot (even when nothing
        // hostile is near).
        // With `clearOnly` only a spot where nothing hostile is near counts (a bot that has just run from a fight).
        // A bot in the water: the spots are looked at on the ground under them (not at the bot's own height under the
        // surface, where nothing on land is found), and a bot under the surface is given its way there from the surface
        // above it in `way` (the bot's own spot first): the bot swims that way (MoveSplinePath), up and along the surface.
        bool FindRestSpot(Player* player, BotRuntime const& rt, float maxYards, float& x, float& y, float& z, uint32& here,
            uint32& there, bool clearOnly = false, std::vector<G3D::Vector3>* way = nullptr);

        // Before a pull: playerbots picks its targets at a spot itself, and the nearest one may stand in a pack. When
        // the bot (out of combat) turns to a creature, the creature and every living hostile one that would join
        // (within the assistance distance of the target, or with the bot's way to it inside its own reach) are put
        // through the fight estimate, against what the bot has right now and the limit of the spot rule
        // (Risk.MaxFightCostPercent). The bot lets go of a target it cannot afford:
        //   Rest:    one foe it could afford rested: it rests first (BotRuntime::restWant);
        //   Refused: a pack, or one foe too strong even for the rested bot: (x, y) is the target's place, which the
        //            planner sets aside.
        //   Held:    the same target again within seconds: let go once more, nothing new for the planner.
        enum class Pull : uint8 { Fine, Rest, Refused, Held };
        Pull CheckPull(Player* player, BotRuntime& rt, float& x, float& y);
        // In a fight: a bystander the bot's AI turned to (a civilian or a service NPC of the other side that does not
        // fight the bot, the Horde's flight master of Light's Hope for an Alliance bot) is let go. True when it was.
        bool DropBystander(Player* player, BotRuntime& rt);
        // A druid's Starfall rains stars for ten seconds on every enemy in its radius and sight, also on a creature in no
        // fight: mod-playerbots looks around only when it casts it, and the druid moves while the stars fall. A Starfall
        // ends when its fight is over or when its stars would reach a creature in no fight (four times a second). True
        // when it ended.
        bool StepStarfall(Player* player, BotRuntime& rt);
        // A creature in no fight that a chaining or area attack of the bot would reach now: within `yards` of the bot, or
        // within the reach of a melee chain (6 yd) of the bot's target. Neutral creatures count: the game lets every spell
        // that hits enemies hit them, and they fight what hits them. The nearest, or none.
        Creature* BystanderInReach(Player* player, float yards);
        // A paladin's Seal of Command chains each melee swing to up to two creatures beside its target, neutral ones and
        // creatures in no fight included: while one stands in that reach the seal is taken off (as a player cancels it)
        // and mod-playerbots' next seal is another one (its "seal of command" is not cast meanwhile: BotDriver). Four
        // times a second, for characters the AI plays. True when it was taken off.
        bool StepSealOfCommand(Player* player, BotRuntime& rt);
        // A foe of the bot's fight (its target, or one that attacks it) casts a spell the game lets be interrupted, and
        // the bot's class has an interrupt ready with the foe in its reach: it interrupts (the owner, 2026-11-23: [quote left out]). A warrior changes to Berserker Stance for
        // Pummel when the rage it keeps through the change pays for it. Four times a second, for characters the AI plays.
        bool StepInterrupt(Player* player, BotRuntime& rt);
        // A foe of the bot's fight that runs for help (the core's assistance movement) toward a creature of its kind that
        // would come back with it: the bot stops it with what its class has for that and can cast now (a bear's Bash, a
        // warrior's Hamstring, a shaman's Frost Shock, a hunter's Concussive Shot ...), once per runner. A level-17 druid
        // in bear form let a Bristleback Water Seeker run 27 yd to a Bristleback Hunter and died to the two Hunters that
        // came (the test guild, 2026-11-24). Four times a second, for characters the AI plays.
        bool StepStopRunner(Player* player, BotRuntime& rt);
        // A melee character does not follow a foe of its fight that runs away (for help, or in fear at low health) where
        // the chase would bring it into creatures that are in no fight, nor more than 18 yd from where the run began: it
        // turns to another foe on it, or waits where it stands until the runner comes back. A level-60 warrior followed a
        // Shadowmage that ran at 15 % health 41 yd across the Eastern Plaguelands into a Diseased Flayer and died to the
        // three (Again, 2026-11-26). The open world's version of the dungeon groups' rule (OrderService::StepDungeonFight).
        // Four times a second, for characters the AI plays. True while it holds the bot back.
        bool StepChaseRunner(Player* player, BotRuntime& rt);
        // A drop behind a character at (x, y, z) that a creature at (fromX, fromY) would knock back: the floor 3 to 12 yd
        // on, in the direction from the creature through the point, 6 yd or more below it (water there is no drop).
        bool DropBehind(Player* player, float x, float y, float z, float fromX, float fromY);
        // In a melee fight with a creature that knocks back (WorldIndex::KnocksBack) a character with a drop behind it steps
        // round the creature to a side where none is, before the blow throws it over the edge: a Stitched Horror's Mighty
        // Blow threw bots of level 60 off the plateau above the Infectis Scar into the hollow below, where no navmesh is,
        // three times in four days (round 272). Four times a second, for characters the AI plays. True when it stepped.
        bool StepKnockbackSide(Player* player, BotRuntime& rt);

        // What the character can still take, as a share of its full strength: its health, and for a caster its mana
        // at half weight (the estimate's own measure).
        float BudgetOf(Player const* player);

        // Living hostile creatures around the character that would attack it and are not far below it.
        // `evading`: creatures on their way home count too (what has just killed the bot is).
        uint32 HostilesAround(Player* player, float range, bool evading = false);
        // The strongest of them (nullptr = none) and what it is worth in levels (its level, more for an elite).
        Creature* StrongestAround(Player* player, float range, uint32* level = nullptr, bool evading = false);

        // True when such a creature that is not fighting is about to have the character in its reach (its aggro
        // range plus 8 yards, at least 15): the rest moves on before it is attacked.
        bool ThreatApproaching(Player* player);

        // Strong creatures on their way that guild characters saw in the last minute and a half (a patrol, a summoned
        // elite on its walk: Stitches on the road to Darkshire): is (x, y) within 80 yards of where one was seen
        // last, or within 60 yards of where it walks in the next 45 seconds, for a character of `level` (it counts
        // from four levels above it, by its health and damage)? `who`: its name and level. World thread only.
        bool RoamerNear(uint32 mapId, float x, float y, uint8 level, std::string* who = nullptr, bool* stays = nullptr);
        // The same for a straight leg (its first 300 yards: by then the creature has moved on).
        bool RoamerOnLeg(uint32 mapId, float fromX, float fromY, float toX, float toY, uint8 level, std::string* who = nullptr,
            bool* stays = nullptr);
        // Out of combat: the bot walks away from a creature, as from a roamer it keeps away from (until it is 60 yards
        // off). For a fight in which nothing happened and that the module ended (OrderService::TrackFight).
        bool WalkAway(Player* player, BotRuntime& rt, Creature* from);
        // The last yards to a fight are walked. From level 20 the bots ride everywhere, and mod-playerbots rides a
        // melee character up to five yards from its target: it arrives among the target's neighbours at twice the
        // speed every look-out of this file was made for (the pull check found a paladin standing on its target, a
        // warrior rode onto the first Kurzen of their compound and had four on it, another saw the Bloodsail pack on
        // its way to a hand-in at 8 yards). Within 60 yards of a task's target that has spawns of creatures which
        // are no friends of the bot within 40 yards, the bot goes on foot, and stays on foot while it is within 100
        // yards of that target. Not on a travel or a voyage. With Pull.Careful off: never. The planner asks every
        // tick and gives the answer to BotDriver::SetMountAllowed.
        bool WalksLastYards(Player* player, BotRuntime& rt);

        // The water at a spot, asked from the map itself: 0 = dry, 1 = in water, 2 = under water.
        uint8 WaterAt(Player const* player, float x, float y, float z);

        // How deep the water is where a character at (x, y, z) stands or floats: the water's level above the floor
        // under it; 0 on dry ground and on a pier or bridge above the water. WaterAt looks at the character's feet,
        // and the path finder keeps a bot at the surface of deep water: such a bot is "dry" for WaterAt.
        float WaterDepth(Player const* player, float x, float y, float z);
        // Water deeper than a character can sit down in to eat, drink or bandage itself.
        inline constexpr float RestWaterYards = 0.6f;
        bool TooWetToRest(Player const* player, float x, float y, float z);

        // A fight ended: foes = most attackers at once, foeLevel = the strongest, health and mana in percent.
        // What a fight against one ordinary foe that many levels above the bot (below it: negative) costs the bot, as
        // a share of what it has: its learned cost of a foe of its own level (BotRuntime::fightCost; until it has
        // fought a few times at most what its level suggests: fights in the starting areas cost little) times the
        // level factor of the estimate (1.3 per level above, 0.87 per level below).
        // While one foe is being killed, each of the others still standing counts this share of a foe (FightNeed,
        // and the spot verdict's NeedOf): two foes of the bot's level are 2.5 times a single fight, three 4.5 times.
        // That is the start: each class learns its own share from its fights against two foes (OthersShareOf).
        inline constexpr float OthersShare = 0.5f;
        // The share the bot's class learned (Risk::PairShare: the mean of what its characters learned from their fights
        // against exactly two foes, 0 to 1.5), else OthersShare. The owner, 2026-11-26, on the finding that a pair cost a
        // mage or a rogue about 1.8 single fights and a death knight 2.6 (the integration server's fights since
        // 2026-11-24), choosing option A: [quote left out].
        float OthersShareOf(Player const* player);
        // Four or more at once: such a fight costs on average what the estimate says, but it scatters widely, and its
        // bad end kills. Measured over a day on the integration server (fights the bots did not begin, mostly): against
        // creatures five to eight levels below the bot a single foe cost 7-8% of its health, two 17-18%, three 25-31%
        // and none of 146 such fights against three was lost, four or more 43-46% on average with 9 deaths in 105 and
        // 30 left; five Bloodscalp trolls seven levels below killed a level-44 warrior from full health. So for a fight
        // the bot would not begin for its work (a group on its way, a guarded place, a camp, the spot verdict) the need
        // counts up to twice from three foes to four: the mean then has to lie within half the limit.
        // Groups of creatures more than this many levels below the bot are no group it cannot take: over a day of live
        // fights none of 100 against four or more of them was lost (four or more cost 19-35% of the bot's health),
        // and a level-46 warlock kept away from packs of three and four level-35 Kurzen Commandos, which cost it
        // 10-20% each, until what stood around it let it go nowhere and it used its hearthstone. They count for the
        // look for a group on its way and the crowd tail only from this close; what stands around a place (a login,
        // a corpse, a rest, a run) counts them further down (PackLevelsBelow).
        inline constexpr int32 GroupLevelsBelow = 8;
        inline float CrowdTail(float foesAtOnce)
        {
            float const over = foesAtOnce - 3.0f;
            return 1.0f + (over <= 0.0f ? 0.0f : (over >= 1.0f ? 1.0f : over));
        }
        // `entry`: the kind of creature, when known: what the bot's own fights with that kind cost it counts then
        // (BotRuntime::foeCost).
        float FoeCost(BotRuntime const& rt, uint8 botLevel, int32 levelsAbove, uint32 entry = 0);
        // The same for a foe the bot's pet does not hold (a hunter, a warlock: the pet holds one foe, the learned cost
        // is the cost with the pet); for every other character FoeCost.
        float FoeCostAlone(Player const* player, BotRuntime const& rt, uint8 botLevel, int32 levelsAbove, uint32 entry = 0);
        // The kinds of creature whose fights cost the bot clearly more than its average, for `.ga bot plan` ("" = none).
        std::string DescribeFoeCosts(BotRuntime const& rt);
        // How much the creature counts that a foe would fetch when it runs for help (WorldIndex::FleesForHelp): a
        // character that fights at range kills most runners on their way, one that fights in melee few.
        float FetchedShare(Player const* player);
        // A character that fights at range and has what is on it rooted or stunned (a Frost Nova) runs to the far end
        // of its spells' range while the root lasts, as a player does. True when it set off.
        bool StepBack(Player* player, BotRuntime& rt);
        // Whether a step in a fight to (x, y) brings the character nearer to a hostile creature that is not in the
        // fight yet and into its reach (its aggro range and a margin): mod-playerbots' "flee" and a mage's "blink
        // back" go straight away from the target and look at nothing behind (BotDriver). nearest: that creature.
        bool StepWakes(Player* player, float x, float y, Creature** nearest = nullptr);
        // The nearest creature within `yards` of (x, y) that is far above the character (an elite by its health and
        // damage, StrengthOfCreature: ThreatLevels or more above it), hostile, attackable and not in a fight with it:
        // what an area spell there would draw into the fight (a civilian included: it does not come by itself, but it
        // fights what hits it). For BotDriver's guards on a mage's area spells.
        Creature* StrongBystanderWithin(Player* player, float x, float y, float yards);
        // The same for any creature of any level, minStrength 0 (neutral ones too: a kodo of the Barrens fights what hits
        // it, and four came at once into a shaman's fight by its Fire Nova); StrongBystanderWithin passes its strength.
        Creature* BystanderWithin(Player* player, float x, float y, float yards, uint32 minStrength = 0);
        // Two or more creatures on it: what protects the character is cast at once (Evasion, Barkskin, Divine
        // Protection, Shield Wall, Ice Barrier), once per fight. Returns the spell's name when it went out.
        std::string StepDefend(Player* player, BotRuntime& rt);

        // After a pull that was called off: when hostile creatures stand near the bot (it walked up to a camp, or
        // fought its way into one), it goes back the way it came until none is within 30 yards, before it turns to
        // anything else. True when it set out (Escape::Step carries it on and ends with Result::ClearOfPack). `why`
        // ends the log line's "backs out of the place ..." (QuestPartners: a pair that parts in a camp).
        bool BackOut(Player* player, BotRuntime& rt, char const* why = "after the pull it called off");
        // After a fight: within reach of creatures it cannot take together (not in a fight, their helpers counted) the
        // bot goes back the way it came first (BackOut). Not in a dungeon.
        bool BackOutAfterFight(Player* player, BotRuntime& rt);

        // Careful pulls (see above). BeginPull starts one for a caller of its own: the bot opens on `target` from a
        // distance (or walks into its sight), goes to `fightAt` (x, y, z; nullptr: where it pulled from) and holds
        // there until the creature is with it. False: no such pull (the bot is in a fight or in the creature's sight
        // already, there is no way to the place). While BotRuntime::pull.active the bot is the pull's: nothing else
        // may move it or give it a target. StepPull: a step of the pull, true while it runs (OrderService::Update
        // calls it four times a second for every bot with a pull). CancelPull hands the bot back at once.
        bool BeginPull(Player* player, BotRuntime& rt, Creature* target, float const* fightAt = nullptr);
        bool StepPull(Player* player, BotRuntime& rt);
        void CancelPull(Player* player, BotRuntime& rt, char const* why);

        // healthLow: the lowest health of the fight (a potion or a heal in it does not make it cheaper). kinds: entry and
        // level of every creature of the fight when each of them was on the bot at once (empty otherwise): two or three
        // of different kinds teach each kind its share.
        void OnFightEnd(Player const* player, BotRuntime& rt, uint8 foes, uint8 foeLevel, uint8 healthStart, uint8 healthLow,
            uint8 manaStart, uint8 manaEnd, bool died, uint32 seconds, uint32 foeEntry = 0, bool oneKind = false, bool foesLost = true,
            std::vector<std::pair<uint32, uint8>> const& kinds = {});
    }
}

#endif
