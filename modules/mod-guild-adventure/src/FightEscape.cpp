/*
 * FightEscape implementation. See FightEscape.h.
 */

#include "FightEscape.h"
#include "Ammunition.h"
#include "BotSettings.h"
#include "BotDriver.h"
#include "CellImpl.h"
#include "CreatureGroups.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GridTerrainData.h"
#include "GuildAdventureSettings.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "OrderService.h"
#include "PathGenerator.h"
#include "PathProbe.h"
#include "Pet.h"
#include "Player.h"
#include "RiskModel.h"
#include "RoadMap.h"
#include "SpellInfo.h"
#include "Squads.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "TemporarySummon.h"
#include "QuestDef.h"
#include "Timer.h"
#include "World.h"
#include "WaypointMgr.h"
#include "WorldIndex.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

// The ground guard of the travel work (GroundGuard.h, core patch CP-10) cancels point movements that would leave the
// ground. Where the module has it, a leg is asked about first, so the run never starts a leg the guard stops.
#if __has_include("GroundGuard.h")
#include "GroundGuard.h"
#define GA_ESCAPE_GROUND_GUARD 1
#endif

using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";
    // A leg of the run, and when the next one is planned at the latest.
    constexpr float LegYards = 45.0f;
    constexpr uint32 LegMs = 5 * IN_MILLISECONDS;
    // mod-playerbots stays paused this long after each tick of the run.
    constexpr uint32 PauseMs = 2500;
    // After a run that did not shake the foes off the bot fights on and does not run again for this long.
    constexpr uint32 FailedPauseMs = 90 * IN_MILLISECONDS;
    // The place of a fight the bot left is set aside this long (or until its next level); twice as long for every
    // earlier escape within EscapePlaceYards of it during the last EscapeMemoryMs, up to MaxAvoidMs.
    constexpr uint32 AvoidMs = 15 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 MaxAvoidMs = 2 * HOUR * IN_MILLISECONDS;
    constexpr uint32 EscapeMemoryMs = 2 * HOUR * IN_MILLISECONDS;
    constexpr float EscapePlaceYards = 100.0f;
    // A foe that hits the bot's pet costs the bot this share of a foe that hits the bot itself.
    constexpr float PetTankShare = 0.4f;
    // ... but a bot that cannot move for the moment (a net, a stun) only for this long.
    constexpr uint32 HeldPauseMs = 8 * IN_MILLISECONDS;
    // Class spells that help to get away are tried during the first seconds of a run.
    constexpr uint32 HelpMs = 4 * IN_MILLISECONDS;
    // The learned cost of a fight against one foe stays inside these bounds; every level of a foe above the bot
    // multiplies it (misses and resists make foes above it far harder than foes below it are easier), an elite counts
    // as this many foes.
    constexpr float MinFightCost = 0.15f;
    constexpr float MaxFightCost = 1.0f;
    // Mana spent counts half as much as health lost, and an empty mana bar halves what a mana user can still take.
    constexpr float ManaWeight = 0.5f;
    // (1.2, was 1.3: measured on the integration server, 15,000 single fights the bots below level 60 won from
    // 2026-11-20 to 26, each bot against its own fights at its level, health and half the mana: one level above 1.20,
    // two 1.44, three 1.61 to 1.70. With 1.3 a pair two levels above came to 4.2 fights at the bot's level, above the
    // 3.8 of the measured pairs' worst tenth (their mean 2.3, none of 119 lost), and the walk gate refused 146 ways
    // past such pairs in a day for level 25-36 bots in Desolace, Thousand Needles, Ashenvale and Stonetalon.)
    constexpr float CostPerLevelAbove = 1.2f;
    // What an elite's or a rare's health and damage are worth in levels (ThreatLevelOf, EliteMultiplier): as before.
    constexpr float StrengthPerLevel = 1.3f;
    // Measured on the integration server (3,300 single fights the bots won in one evening): a creature three levels
    // below the bot costs 0.71 (melee) to 0.78 (casters) of one of its own level, four below 0.69 to 0.73, five
    // below 0.70 to 0.84. The first value, 0.87 per level (0.66, 0.57, 0.50), made weaker creatures too cheap, and
    // worse, it is also what a fight's cost is divided by when it is learned: a bot that fought creatures four
    // levels below it learned a cost for its own level a quarter too high (a shaman's went from 0.39 to 0.51 in a
    // round of crocolisks one to five levels below it), and the spots of its own level were refused for it.
    constexpr float CostPerLevelBelow = 0.92f;
    constexpr float EliteFactor = 2.5f;
    constexpr float LearnRate = 0.25f;
    // A single foe is only judged when it is an elite or this many levels above the bot.
    constexpr uint8 OutmatchedLevels = 3;
    // Foes this many levels below the bot are no reason to run.
    constexpr uint8 TrivialLevels = 5;
    // The estimate has to say "leave" on this many ticks in a row.
    constexpr uint8 TicksToLeave = 2;
    constexpr float Pi = 3.14159265f;
    // A fight against one ordinary foe is judged only below this health.
    constexpr float LosingHealthPct = 35.0f;
    // (A fight in the water counted three times what it costs on land, after a level-17 druid had lost to single
    // level-13 murlocs under water. That was the aquatic form it stayed in, which LeaveTravelForm ends since: 83
    // single fights in or under water of 14 bots in one night cost what their fights on land cost (a hunter 0.12
    // against 0.10, a priest 0.35 against 0.29, a rogue 0.16 against 0.18), none was lost. The factor refused pulls
    // a hunter's pet tanks at full health: "does not pull Young Wetlands Crocolisk: the fight would cost it 91 %".)
    // The trail: a point every TrailStepYards out of combat, the newest TrailPoints of them; a leg back along it ends
    // at the newest point at least TrailLegMinYards away. A jump of TrailBreakYards (a teleport, a flight) starts a new one.
    constexpr float TrailStepYards = 10.0f;
    constexpr std::size_t TrailPoints = 20;
    constexpr float TrailLegMinYards = 30.0f;
    constexpr float TrailLegMaxYards = 80.0f;
    constexpr float TrailBreakYards = 150.0f;
    // A camp: hostile creatures this close that are not in the fight yet make the estimate this much worse each
    // (at most CampCount of them).
    constexpr float CampYards = 25.0f;
    constexpr float CampShare = 0.2f;
    constexpr uint32 CampCount = 5;
    // Keeping away from a roaming creature far above the bot: looked for this often, within WatchYards; the bot is
    // clear of it at ClearYards or gives up after AsideMaxMs; the place is set aside for AsideAvoidMs.
    constexpr uint32 WatchMs = 2 * IN_MILLISECONDS;
    constexpr float WatchYards = 35.0f;
    constexpr float ClearYards = 60.0f;
    constexpr uint8 ThreatLevels = 4;
    constexpr uint32 AsideMaxMs = 20 * IN_MILLISECONDS;
    // Clear of a roamer that is on its way: the bot waits until it is this far off, for this long at most.
    constexpr float RoamerWaitYards = 110.0f;
    constexpr uint32 RoamerWaitMs = 75 * IN_MILLISECONDS;
    constexpr uint32 AsideAvoidMs = 10 * MINUTE * IN_MILLISECONDS;
    // A strong creature that keeps to its place (KeepsToItsPlace): the bot's way stays StandClearYards outside its
    // aggro range over the next StandAheadYards of the way, the bot is clear of it StandAwayYards farther out, and
    // it goes around it up to StandMaxDetours times in the detour window.
    constexpr float StandClearYards = 6.0f;
    constexpr float StandAheadYards = 70.0f;
    constexpr float StandAwayYards = 12.0f;
    constexpr uint8 StandMaxDetours = 8;

    // For what a pull or a walk brings together, creatures far below the bot count too: a level-22 mage pulled one
    // level-17 gnoll, the check saw nobody else (five levels below: "trivial"), and five of them killed it in ten
    // seconds, twice.
    // The same wherever the question is what stands around a place (a camp at a login, at a corpse, at a place to
    // rest, on a leg of a run): everything that many levels below the bot and lower was left out with five, and a
    // level-21 priest entered the world among gnolls of level 14 to 16, saw "nothing hostile", pulled one and died
    // to four; its run led "past no other creature" through the same camp. (Twelve since: it was ten, and a
    // level-27 priest died three times in a morning to murlocs and crawlers of level 17 at the shore it fished at.
    // What such creatures cost is small, 0.92 per level below; many of them are still a fight.)
    constexpr uint8 PackLevelsBelow = 12;
    // (levelsBelow: creatures that many levels below the bot and lower are left out.)
    std::vector<Creature*> LiveHostiles(Player* player, float range, uint8 levelsBelow = PackLevelsBelow, bool evading = false);

    // A squad fights together (Squads.h): the foes of the squad characters within this distance are the bot's foes, and
    // what they can still take counts beside its own.
    constexpr float SquadPartnerYards = 30.0f;

    std::vector<Creature*> FoesOf(Player* player)
    {
        std::vector<Creature*> foes;
        auto add = [&foes](Unit* unit)
        {
            Creature* creature = unit ? unit->ToCreature() : nullptr;
            if (!creature || !creature->IsAlive() || creature->IsPet() || creature->IsTotem() || creature->IsInEvadeMode())
                return;
            if (std::find(foes.begin(), foes.end(), creature) == foes.end())
                foes.push_back(creature);
        };
        for (Unit* attacker : player->getAttackers())
            add(attacker);
        if (Pet* pet = player->GetPet())
            for (Unit* attacker : pet->getAttackers())
                add(attacker);
        for (Player* partner : sSquadService.GetPartners(player, SquadPartnerYards))
        {
            for (Unit* attacker : partner->getAttackers())
                add(attacker);
            if (Pet* pet = partner->GetPet())
                for (Unit* attacker : pet->getAttackers())
                    add(attacker);
        }
        return foes;
    }

    // What a fight against one foe of its level costs a bot that has hardly fought yet, at most: the first levels are
    // easy (their creatures hit weakly), the measured 40 % belong to bots of level 10 and more in poor gear.
    constexpr uint32 LearnedAfterFights = 4;
    float PriorCost(uint8 level)
    {
        return level < 6 ? 0.15f : (level < 10 ? 0.25f : 0.4f);
    }

    float CostOf(BotRuntime const& rt, uint8 level)
    {
        return rt.fightsLearned >= LearnedAfterFights ? rt.fightCost : std::min(rt.fightCost, PriorCost(level));
    }

    // What a fight against one creature of that kind and the bot's level costs it: what its own fights with that
    // kind taught it, once it has fought two of them (or one that took nearly everything), else its average. Not
    // below half its average (a kind that was cheap twice is not cheap three at a time).
    constexpr uint16 FoeNoteFights = 2;
    // A death or a fight the bot had to leave counts as that many fights of the kind: the note counts at once
    // (EntryCost, Risk::KindRatio). Against one foe such a fight puts the note above FoeNoteAtOnce anyway; against two
    // or three it does not (all the bot had, shared out), and a first death to a group taught nothing: "Thunderhoof died
    // to 3 at once (Private Hendel, Theramore Sentry): from now on it counts them together as about 54% (it had
    // reckoned 54%)", the test guild, round 238.
    void CountSevere(BotRuntime::FoeNote& note)
    {
        note.fights = std::max(note.fights, FoeNoteFights);
    }
    constexpr float FoeNoteAtOnce = 0.8f;
    constexpr float MaxFoeNoteCost = 1.5f;
    float EntryCost(BotRuntime const& rt, uint8 level, uint32 entry)
    {
        float const base = CostOf(rt, level);
        if (!entry)
            return base;
        auto itr = rt.foeCost.find(entry);
        if (itr == rt.foeCost.end() || (itr->second.fights < FoeNoteFights && itr->second.cost < FoeNoteAtOnce))
        {
            // A kind it has not learned: what the guild's characters of its class learned of it, against its own
            // average (round 172: a warrior that had never fought a pair of Shadowmages judged the pair by its average
            // and died; the other warriors' fights said what a pair costs a warrior).
            // Only upwards: what others found cheap does not make the bot bolder before its own first fights.
            // And a kind whose script stuns its foe every few seconds counts half again: Thunderhoof (50, a cat)
            // pulled a Frayfeather Stagwing, whose Swoop stuns all within 5 yd every 3-5 s; two Hippogryphs came with
            // it, it had judged the three at its average (48 %) and died, round 257 (its single fights: 5-20 %).
            constexpr float StunningPrior = 1.5f;
            float ratio = Risk::KindRatio(rt.playerClass, entry, rt.guid);
            // Its own single fight with the kind counts beside the class's: half each (Mojowind's one Felpaw fight
            // cost it three quarters of its average while its class said twice).
            if (itr != rt.foeCost.end() && itr->second.fights >= 1 && ratio > 1.0f && base > 0.0f)
                ratio = (ratio + itr->second.cost / base) / 2.0f;
            if (sWorldIndex.StunsOften(entry))
                ratio = std::max(ratio, StunningPrior);
            if (ratio > 1.0f)
                return std::min(base * ratio, MaxFoeNoteCost);
            return base;
        }
        return std::clamp(itr->second.cost, base * 0.5f, MaxFoeNoteCost);
    }

    float LevelFactor(Player const* player, uint8 foeLevel)
    {
        int32 const diff = std::clamp<int32>(int32(foeLevel) - int32(player->GetLevel()), -5, 5);
        return diff >= 0 ? std::pow(CostPerLevelAbove, float(diff)) : std::pow(CostPerLevelBelow, float(-diff));
    }

    // What an elite counts for beside its level factor: EliteFactor, or what its health and damage are worth (the
    // look-outs' measure, ThreatLevelOf) when that is more. The Scarlet Praetorians of Tyr's Hand (three times the
    // health, 3.75 times the damage) counted as level-56 creatures times 2.5 in a level-60 mage's pack look, "the fight
    // about 64%: it takes them"; it died to two of them and an Archmage. By their health and damage they are level 66.
    float EliteMultiplier(Player const* player, Creature const* foe)
    {
        CreatureTemplate const* info = foe->GetCreatureTemplate();
        float const strength = std::max(1.0f, info->ModHealth) * std::max(1.0f, info->DamageModifier);
        uint32 const worth = foe->GetLevel() + uint32(std::floor(std::log(strength) / std::log(StrengthPerLevel) + 0.01f));
        // (By StrengthPerLevel for the levels above the bot, as it was with the old factor: what its health and damage
        // are worth stays what it was.)
        auto const byStrength = [player](uint32 lvl)
        {
            int32 const diff = std::clamp<int32>(int32(lvl) - int32(player->GetLevel()), -5, 5);
            return diff >= 0 ? std::pow(StrengthPerLevel, float(diff)) : std::pow(CostPerLevelBelow, float(-diff));
        };
        float const byLevel = byStrength(foe->GetLevel());
        return std::max(EliteFactor, byLevel > 0.0f ? byStrength(std::min<uint32>(worth, 255)) / byLevel : EliteFactor);
    }

    // A hunter and a warlock learn what a fight costs them with the pet holding the foe. A pet holds one: every
    // further foe hits the bot itself, and that costs it at least what a fight costs a bot that has learned nothing
    // yet (a hunter whose fights cost it 3 % judged a pull of three murlocs affordable and ended it at 10 %).
    bool OwnsPet(Player const* player)
    {
        return (player->getClass() == CLASS_HUNTER || player->getClass() == CLASS_WARLOCK) && player->GetLevel() >= 10;
    }

    float CostAlone(Player const* player, float cost)
    {
        return OwnsPet(player) ? std::max(cost, PriorCost(player->GetLevel())) : cost;
    }

    // What the rest of the fight costs, as a share of the bot's full health. weights: how surely each foe is in the
    // fight (none: every one is). A creature that only may come counts with its share, as a foe to kill and as one
    // that hits meanwhile: one that comes every second time is half a foe, not a whole one that is half as strong.
    // notes: the bot's runtime, for what each kind of creature costs it (EntryCost); without it every foe costs `cost`.
    float FightNeed(Player const* player, std::vector<Creature*> const& foes, float cost, std::vector<float> const* weights = nullptr,
        BotRuntime const* notes = nullptr)
    {
        struct Part
        {
            float cost;
            float weight;
        };
        std::vector<Part> parts;
        parts.reserve(foes.size());
        Pet const* pet = player->GetPet();
        bool const petHere = pet && pet->IsAlive();
        // A rider's pet is put away while it rides (the core unsummons it at the mount and calls it back at the
        // dismount), and the check before a pull runs while the bot rides up to its target: the pet counted as
        // absent, every foe as one the bot fights alone ("does not pull Young Wetlands Crocolisk: the fight would
        // cost it about 91%", for a hunter whose fights there end at full health).
        bool const petUp = petHere || (!pet && player->IsMounted() && player->GetTemporaryUnsummonedPetNumber() != 0);
        bool held = false;
        if (petHere)
            for (Creature const* foe : foes)
                if (foe->GetVictim() == pet)
                    held = true;
        for (std::size_t i = 0; i < foes.size(); ++i)
        {
            Creature const* foe = foes[i];
            float const weight = weights && i < weights->size() ? (*weights)[i] : 1.0f;
            bool const onPet = petHere && foe->GetVictim() == pet;
            // Before the pull nothing is on the pet yet: it will take the first foe, the bot's target.
            bool const petTakes = onPet || (petUp && !held && i == 0);
            float const own = notes ? EntryCost(*notes, player->GetLevel(), foe->GetEntry()) : cost;
            float part = foe->GetHealthPct() / 100.0f * (petTakes ? own : CostAlone(player, own)) * LevelFactor(player, foe->GetLevel());
            if (foe->isElite())
                part *= EliteMultiplier(player, foe);
            // The pet holds it (what the pet's fights cost the bot is in `cost` already: it was learned with the pet).
            if (onPet)
                part *= PetTankShare;
            parts.push_back({ part * weight, weight });
            // What its death brings is one more foe of the fight (WorldIndex::DeathSummonOf): a Tortured Druid's
            // Hive'Ashi Drone. Not held by the pet (it is on the summoner), at its own level and its kind's cost.
            if (auto const [summon, count] = sWorldIndex.DeathSummonOf(foe->GetEntry()); summon)
                if (CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(summon))
                {
                    float const summonOwn = notes ? EntryCost(*notes, player->GetLevel(), summon) : cost;
                    float summonPart = CostAlone(player, summonOwn) * LevelFactor(player, uint8(std::max(info->minlevel, info->maxlevel)));
                    if (info->rank != CREATURE_ELITE_NORMAL && info->rank != CREATURE_ELITE_RARE)
                        summonPart *= EliteFactor;
                    for (uint8 n = 0; n < count; ++n)
                        parts.push_back({ summonPart * weight, weight });
                }
            // What it summons in the fight (WorldIndex::CombatSummonOf: a Centipaar Swarmer's Silithid Swarms), before
            // the pull only: once it fights, its summons stand in the fight and are foes of their own.
            if (!foe->IsInCombat())
                if (auto const [summon, count] = sWorldIndex.CombatSummonOf(foe->GetEntry()); summon)
                    if (CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(summon))
                    {
                        float const summonOwn = notes ? EntryCost(*notes, player->GetLevel(), summon) : cost;
                        float summonPart = CostAlone(player, summonOwn) * LevelFactor(player, uint8(std::max(info->minlevel, info->maxlevel)));
                        if (info->rank != CREATURE_ELITE_NORMAL && info->rank != CREATURE_ELITE_RARE)
                            summonPart *= EliteFactor;
                        for (uint8 n = 0; n < count; ++n)
                            parts.push_back({ summonPart * weight, weight });
                    }
        }
        // The weakest dies first; everyone still standing hits meanwhile, and counts OthersShare of a foe for that
        // time. (It counted a whole one: two foes of the bot's level were three times a single fight. Measured over
        // a night on the integration server, 351 fights against two foes: the 291 that were won cost 1.9 times
        // the bot's single fights, 9 were lost, 51 left. The second foe seldom hits from the first second, and
        // part of a fight's cost is mana and the way in, which a second foe does not double.)
        std::sort(parts.begin(), parts.end(), [](Part const& a, Part const& b) { return a.cost < b.cost; });
        float standing = 0.0f;
        for (Part const& part : parts)
            standing += part.weight;
        float need = 0.0f;
        for (Part const& part : parts)
        {
            need += part.cost * (part.weight + Escape::OthersShareOf(player) * (standing - part.weight));
            standing -= part.weight;
        }
        return need;
    }

    bool UsesMana(Player const* player)
    {
        return player->getPowerType() == POWER_MANA && player->GetMaxPower(POWER_MANA) > 0;
    }

    // What one character can still take, as a share of its full health.
    float OwnBudget(Player const* player)
    {
        float budget = player->GetHealthPct() / 100.0f;
        if (UsesMana(player))
            budget *= (1.0f - ManaWeight) + ManaWeight * float(player->GetPower(POWER_MANA)) / float(player->GetMaxPower(POWER_MANA));
        return budget;
    }

    // What a rest gives the bot by the guild leader's rules: food only below his health threshold (Rest.BelowHealth,
    // then up to Rest.UntilHealth), drink up to Rest.UntilMana.
    float RestedOwnBudget(Player const* player)
    {
        Settings const settings = sBotSettings.For(player);
        float health = player->GetHealthPct();
        if (health < float(settings.restBelowHealth))
            health = std::max(health, float(settings.restUntilHealth));
        float budget = health / 100.0f;
        if (UsesMana(player))
        {
            float const mana = std::max(float(player->GetPower(POWER_MANA)) * 100.0f / float(player->GetMaxPower(POWER_MANA)),
                float(settings.restUntilMana));
            budget *= (1.0f - ManaWeight) + ManaWeight * mana / 100.0f;
        }
        return budget;
    }

    // What the bot can still take, as a share of its full health: its own, and in a squad what the squad characters
    // beside it can take.
    float Budget(Player const* player)
    {
        float budget = OwnBudget(player);
        for (Player const* partner : sSquadService.GetPartners(player, SquadPartnerYards))
            budget += OwnBudget(partner);
        return budget;
    }

    // What a pull may cost a squad: its partners count half. The foes do not share themselves out: they stay on the one
    // who pulled them, or go to the one who hits them first, and nobody of an open-world squad takes them off another.
    // Two level-44/45 warriors of a squad pulled Wastewander camps of Tanaris as "affordable for two": three foes stayed on
    // the member and killed it, then five on the leader ("5 expected"), which died with its partner at full health
    // beside it (Bramhorn and Gorruk, the test guild, 2026-11-26).
    constexpr float PullPartnerShare = 0.5f;
    float PullBudget(Player const* player)
    {
        float budget = OwnBudget(player);
        for (Player const* partner : sSquadService.GetPartners(player, SquadPartnerYards))
            budget += PullPartnerShare * OwnBudget(partner);
        return budget;
    }

    // The fight's own numbers (OrderService::TrackFight): how fast the bot takes the foes' health and how fast each
    // foe takes the bot's. The learned cost is an average of fights against one foe; a fight that goes worse (an add
    // that doubles the hits, a foe that heals, a bot whose weapon is ten levels old) shows here within seconds.
    // `rate` gets the health one foe takes per second (share of full health), 0 when nothing is measured yet.
    constexpr float MeasureAfterSeconds = 6.0f;
    constexpr float NoProgressSeconds = 20.0f;
    constexpr float KillWindowMinSeconds = 4.0f;
    float MeasuredNeed(Player const* player, BotRuntime const& rt, std::vector<Creature*> const& foes, float& rate)
    {
        rate = 0.0f;
        BotRuntime::Fight const& fight = rt.fight;
        if (!fight.active || foes.empty())
            return 0.0f;
        float const elapsed = float(getMSTimeDiff(fight.startedAt, getMSTime())) / float(IN_MILLISECONDS);
        if (elapsed < MeasureAfterSeconds || fight.attackerSeconds < MeasureAfterSeconds - 1.0f)
            return 0.0f;
        // What the fight has taken so far, in the estimate's own measure (health, and mana at half weight): a mage
        // behind a Mana Shield loses no health for half a minute and then all of it.
        float const lostHealth = std::max(0.0f, float(fight.healthStart) - player->GetHealthPct());
        float const lost = std::max(lostHealth, (fight.budgetStart - OwnBudget(player)) * 100.0f);
        if (lost < 5.0f)
            return 0.0f; // it is not being hurt: nothing to project
        rate = lostHealth / fight.attackerSeconds / 100.0f;
        float const lossRate = lost / fight.attackerSeconds / 100.0f;
        float const removed = std::max(0.0f, fight.unitsJoined - fight.unitsNow);
        // Hardly any progress yet says nothing in the first seconds; after twenty it says the fight does not move. And
        // how fast the bot takes the foes' health is measured from the moment they first lost some, not from the
        // fight's start: a level-60 mage's first spell lands 9 to 13 seconds into a fight (its Frost Nova, the step
        // back to range, then Ice Barrier and its other cooldowns), and the ten seconds of the first rule read that as
        // a fight that does not move. Five mages left 25 fights against two ordinary foes in four hours at 80 to 100 %
        // health "by how the fight went", and Bommbang died on such a run from two Shadowmages, whose bolts follow a
        // run; the same fight, staged, was won at 27 % with a potion, round 148.
        if (removed < 5.0f && elapsed < NoProgressSeconds)
            return 0.0f;
        float window = elapsed;
        if (fight.firstLossAt)
        {
            window = float(getMSTimeDiff(fight.firstLossAt, getMSTime())) / float(IN_MILLISECONDS);
            if (window < KillWindowMinSeconds)
                return 0.0f; // one spell says little about the rate
        }
        float const killRate = std::max(removed, 2.0f) / window; // foe health (percent of one foe) per second
        std::vector<float> rest;
        rest.reserve(foes.size());
        Pet const* pet = player->GetPet();
        for (Creature const* foe : foes)
            rest.push_back(foe->GetHealthPct() * (pet && pet->IsAlive() && foe->GetVictim() == pet ? PetTankShare : 1.0f));
        std::sort(rest.begin(), rest.end());
        float need = 0.0f;
        for (std::size_t i = 0; i < rest.size(); ++i)
            need += lossRate * float(rest.size() - i) * rest[i] / killRate;
        return need;
    }

    // A foe that runs for help comes back with it: the creature it runs to and what stands beside that one are added
    // to the foes (the core sends it to the nearest creature of its kind within 30 yards that is not fighting, and
    // what stands within 10 yards of that one comes along). Returns how many were added.
    uint32 AddFetched(Player* player, std::vector<Creature*>& foes)
    {
        constexpr float FleeAssistYards = 30.0f;
        constexpr float HelperPackYards = 10.0f;
        constexpr std::size_t MaxFetched = 3;
        Creature* fleeing = nullptr;
        for (Creature* foe : foes)
        {
            MovementGeneratorType const type = foe->GetMotionMaster()->GetCurrentMovementGeneratorType();
            if (type == ASSISTANCE_MOTION_TYPE || type == ASSISTANCE_DISTRACT_MOTION_TYPE)
            {
                fleeing = foe;
                break;
            }
        }
        if (!fleeing)
            return 0;
        std::vector<Creature*> around = LiveHostiles(player, 70.0f, PackLevelsBelow);
        std::erase_if(around, [&](Creature* creature)
        {
            return creature->IsInCombat() || std::find(foes.begin(), foes.end(), creature) != foes.end() ||
                creature->GetExactDist(fleeing) > FleeAssistYards + HelperPackYards + 2.0f * creature->GetObjectSize();
        });
        // (Both creatures' sizes count, as the core's IsWithinDistInMap counts them.)
        Creature* helper = nullptr;
        for (Creature* creature : around)
            if (creature->GetExactDist(fleeing) <= FleeAssistYards + creature->GetObjectSize() + fleeing->GetObjectSize() &&
                creature->GetFaction() == fleeing->GetFaction() &&
                (!helper || creature->GetExactDist(fleeing) < helper->GetExactDist(fleeing)))
                helper = creature;
        if (!helper)
            return 0;
        std::size_t const before = foes.size();
        foes.push_back(helper);
        for (Creature* creature : around)
            if (creature != helper && foes.size() - before < MaxFetched && creature->GetExactDist(helper) <= HelperPackYards)
                foes.push_back(creature);
        return uint32(foes.size() - before);
    }

    // byCount: the number of foes alone says the fight is lost, whatever `need` says. byFight: the fight's own
    // numbers say so (MeasuredNeed), `rate` is what one foe takes per second by them (0 = not measured).
    // What a class can give itself back in a fight with a cooldown that is ready (mod-playerbots presses these at
    // critical health): a paladin's Lay on Hands (all its health), else its Divine Shield (twelve seconds to heal
    // in, with the mana for it); neither with Forbearance on it. Counted in the decision to leave a fight, not before
    // a pull: nobody plans a pull on a cooldown of twenty minutes. (The owner, 2026-11-14: [quote left out]: a level-40
    // paladin left four murlocs of level 36 and 37 at 89 % health with both ready.)
    float EmergencyReserve(Player const* player)
    {
        if (player->getClass() != CLASS_PALADIN)
            return 0.0f;
        constexpr uint32 Forbearance = 25771;
        constexpr uint32 LayOnHands = 633;
        constexpr uint32 DivineShield = 642;
        if (player->HasAura(Forbearance))
            return 0.0f;
        auto ready = [player](uint32 first)
        {
            uint32 known = 0;
            for (uint32 spell = sSpellMgr->GetLastSpellInChain(first); spell; spell = sSpellMgr->GetPrevSpellInChain(spell))
            {
                if (player->HasSpell(spell))
                {
                    known = spell;
                    break;
                }
                if (spell == first)
                    break;
            }
            return known && !player->HasSpellCooldown(known);
        };
        if (ready(LayOnHands))
            return 0.9f;
        if (ready(DivineShield) && UsesMana(player) &&
            uint64(player->GetPower(POWER_MANA)) * 100 >= uint64(player->GetMaxPower(POWER_MANA)) * 30)
            return 0.5f;
        return 0.0f;
    }

    // An escort's NPC stands beside the bot (alive, within 30 yd): the fight is the escort's, and leaving it
    // ends the escort. The ambush of "Questioning Reethe" (Lieutenant Caldwell and three men) was four foes on the
    // guild leader beside Ogron, who fought them too; the leader left at 72 % health and the escort was over, 2026-11-25.
    constexpr float WardBesideYards = 30.0f;
    constexpr float WardStayHealthPct = 35.0f;
    bool WardFightsBeside(Player* player, BotRuntime const& rt)
    {
        // (The same for a quest event the bot defends at its NPC or leads an NPC through: Thrakgar, a level-29 shaman,
        // left the first wave of three Galak Assassins at Kanati Greycloud ("Protect Kanati Greycloud") at 94 % health,
        // was netted, could not shake them off and died 9 s after the event had ended, round 243.)
        uint32 ward = 0;
        if (rt.task.kind == TaskKind::Escort)
            ward = rt.task.escortEntry;
        else if (rt.task.kind == TaskKind::Event &&
            (rt.task.event.kind == QuestEventKind::Stay || rt.task.event.kind == QuestEventKind::Lead))
            ward = rt.task.event.entry;
        if (!ward)
            return false;
        // (In a fight or not: Ogron stood beside the guild leader while the ambush was on it alone, and leaving it there
        // ended the escort as well.)
        return player->FindNearestCreature(ward, WardBesideYards, true) != nullptr;
    }

    bool ShouldLeave(Player* player, BotRuntime const& rt, std::vector<Creature*> const& foes, float& need, bool& byCount,
        bool& byFight, float& rate)
    {
        byCount = false;
        byFight = false;
        rate = 0.0f;
        if (foes.empty())
            return false;
        // (It stays with the escort's NPC while it has more than a third of its health.)
        if (player->GetHealthPct() > WardStayHealthPct && WardFightsBeside(player, rt))
            return false;
        float const margin = sBotSettings.GetFloat(player, Setting::EscapeMargin);
        float const measured = MeasuredNeed(player, rt, foes, rate);
        // (What the bot has left, in the measure the measurement uses; with what a cooldown gives it back.)
        float const reserve = EmergencyReserve(player);
        float const healthShare = OwnBudget(player) + reserve;
        uint8 const level = player->GetLevel();
        bool outmatched = false;
        bool trivial = true;
        for (Creature const* foe : foes)
        {
            if (foe->isElite() || foe->GetLevel() >= level + OutmatchedLevels)
                outmatched = true;
            if (foe->GetLevel() + TrivialLevels > level)
                trivial = false;
        }
        // One ordinary foe is fought out, unless the bot is losing: below LosingHealthPct the estimate decides here too
        // (a level-15 rogue went down to 40 % against one level-14 gnoll and died when a second one came). Foes far
        // below the bot are fought out while it is above half health (a level-19 mage ran out of mana against two foes
        // of level 13 and 14 and died without a look at the estimate).
        constexpr float TrivialHealthPct = 50.0f;
        // Against several foes, or one far above it, a fight that clearly goes worse than the bot can take is left
        // early (a warrior stayed with two orcs by its learned cost and died with the first at 9 %). One ordinary
        // foe is not left for that: a first try left six such fights in ten minutes that were close, not lost.
        constexpr float ClearlyLosing = 1.2f;
        bool const single = foes.size() < 2;
        bool const losing = measured > healthShare * margin * ClearlyLosing && (!single || outmatched);
        // (Only one or two of them: three and more foes far below it are a fight like any other.)
        if (trivial && foes.size() >= 3)
            trivial = false;
        if (!losing && (trivial ? player->GetHealthPct() >= TrivialHealthPct :
            (single && !outmatched && player->GetHealthPct() >= LosingHealthPct)))
            return false;
        // One ordinary foe and the bot low: the fight's own numbers decide when there are some (a foe at 5 % is
        // finished, whatever the average says; one at 80 % is not).
        if (single && !outmatched && measured > 0.0f)
        {
            need = measured;
            byFight = true;
            return measured > healthShare * margin;
        }
        need = FightNeed(player, foes, CostOf(rt, player->GetLevel()), nullptr, &rt);
        // Three or more foes of about the bot's level or above: the count alone says the fight is lost (91 % of such
        // fights were, and a caster that waits for the estimate leaves at half health). Foes two or more levels below
        // it are left to the estimate: a level-16 rogue ran from three level-12 gnolls at 96 % and died on the run.
        if (uint32 const maxFoes = uint32(sBotSettings.Get(player, Setting::EscapeMaxFoes)) * sSquadService.CountTogether(player, SquadPartnerYards))
        {
            uint32 serious = 0;
            for (Creature const* foe : foes)
                if (foe->GetLevel() + 1 >= level)
                    ++serious;
            if (serious >= maxFoes)
            {
                byCount = true;
                return true;
            }
        }
        // In a camp the next foe is a matter of seconds: what stands near and is not in the fight yet counts.
        if (foes.size() >= 2)
        {
            std::vector<Creature*> others = LiveHostiles(player, CampYards);
            std::erase_if(others, [&foes](Creature* creature) { return std::find(foes.begin(), foes.end(), creature) != foes.end(); });
            need *= 1.0f + CampShare * float(std::min<std::size_t>(others.size(), CampCount));
        }
        if (need > (Budget(player) + reserve) * margin)
            return true;
        // The learned cost says it can afford the fight; the fight itself says otherwise.
        if (losing)
        {
            need = std::max(need, measured);
            byFight = true;
            return true;
        }
        return false;
    }

    // Hostile creatures around the bot that are alive now, would attack it and are not far below it.
    // evading: also the creatures that are on their way home from a fight (they are hostile again when they arrive).
    // (No civilians: they neither attack nor help. Gringer, the Horde's flight master in Booty Bay, level 65, stands
    // 15 yd from the inn: an Alliance bot that entered the world there "backed out of the place" from him, the
    // keep-away set the inn's place aside for ten minutes and with it the town's own flight master: every walk north
    // was refused, no flight took its place, and the bot stood for half an hour.)
    std::vector<Creature*> LiveHostiles(Player* player, float range, uint8 levelsBelow, bool evading)
    {
        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, range);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, units, check);
        Cell::VisitObjects(player, searcher, range);
        std::vector<Creature*> hostiles;
        for (Unit* unit : units)
        {
            Creature* creature = unit->ToCreature();
            if (creature && !creature->IsPet() && !creature->IsTotem() && !creature->IsCivilian() &&
                (evading || !creature->IsInEvadeMode()) &&
                creature->IsHostileTo(player) && !creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_IMMUNE_TO_PC) &&
                creature->GetLevel() + levelsBelow > player->GetLevel())
                hostiles.push_back(creature);
        }
        return hostiles;
    }

    uint32 CountNear(std::vector<Creature*> const& creatures, float x, float y, float radius)
    {
        uint32 count = 0;
        for (Creature const* creature : creatures)
        {
            float const dx = creature->GetPositionX() - x;
            float const dy = creature->GetPositionY() - y;
            if (dx * dx + dy * dy <= radius * radius)
                ++count;
        }
        return count;
    }

    // How many of the creatures a straight run from the bot to a point would lead past within their reach (their aggro
    // range plus a margin): a leg that crosses other creatures of the camp brings them into the run (a warlock ran from
    // murlocs three times and each time into further murlocs).
    uint32 CountAlong(std::vector<Creature*> const& creatures, Player const* player, float toX, float toY)
    {
        float const fromX = player->GetPositionX();
        float const fromY = player->GetPositionY();
        float const wx = toX - fromX;
        float const wy = toY - fromY;
        float const length2 = wx * wx + wy * wy;
        uint32 count = 0;
        for (Creature const* creature : creatures)
        {
            float const ax = creature->GetPositionX() - fromX;
            float const ay = creature->GetPositionY() - fromY;
            float const along = length2 > 1.0f ? std::clamp((ax * wx + ay * wy) / length2, 0.0f, 1.0f) : 0.0f;
            float const cx = ax - along * wx;
            float const cy = ay - along * wy;
            float const reach = std::max(12.0f, creature->GetAggroRange(player) + 5.0f);
            if (cx * cx + cy * cy <= reach * reach)
                ++count;
        }
        return count;
    }

    // How far a creature stands from the straight way between the bot and a point.
    float WayDistance(Creature const* creature, Player const* player, float toX, float toY)
    {
        float const fromX = player->GetPositionX();
        float const fromY = player->GetPositionY();
        float const wx = toX - fromX;
        float const wy = toY - fromY;
        float const length2 = wx * wx + wy * wy;
        float const ax = creature->GetPositionX() - fromX;
        float const ay = creature->GetPositionY() - fromY;
        float const along = length2 > 1.0f ? std::clamp((ax * wx + ay * wy) / length2, 0.0f, 1.0f) : 0.0f;
        return std::hypot(ax - along * wx, ay - along * wy);
    }

    // How far a creature stands from the way (x1, y1) -> (x2, y2).
    float SegmentDistance(Creature const* creature, float x1, float y1, float x2, float y2)
    {
        float const wx = x2 - x1;
        float const wy = y2 - y1;
        float const length2 = wx * wx + wy * wy;
        float const ax = creature->GetPositionX() - x1;
        float const ay = creature->GetPositionY() - y1;
        float const along = length2 > 1.0f ? std::clamp((ax * wx + ay * wy) / length2, 0.0f, 1.0f) : 0.0f;
        return std::hypot(ax - along * wx, ay - along * wy);
    }

    // Out of combat: the trail grows.
    void RecordTrail(Player const* player, BotRuntime& rt)
    {
        if (player->IsInFlight() || player->GetTransport())
            return;
        if (!rt.trail.empty())
        {
            BotRuntime::TrailPoint const& last = rt.trail.back();
            float const dx = player->GetPositionX() - last.x;
            float const dy = player->GetPositionY() - last.y;
            float const d2 = dx * dx + dy * dy;
            if (last.mapId != player->GetMapId() || d2 > TrailBreakYards * TrailBreakYards)
                rt.trail.clear();
            else if (d2 < TrailStepYards * TrailStepYards)
                return;
        }
        rt.trail.push_back({ player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ() });
        if (rt.trail.size() > TrailPoints)
            rt.trail.erase(rt.trail.begin());
    }

    // True if the bot has a whole path on the navmesh to the spot. The spot then becomes the path's own end: its height
    // is the ground's (a spot that kept the bot's own height lay under a slope, and a bot that sat down there sat under
    // the terrain).
    // With `pass`: not a path that leads the bot into the reach of one of those creatures (one it already stands in
    // the reach of does not count): a bot at 39 % health walked 38 yd to a quieter place past a gnoll scout.
    // With `from`: the path from there instead of from the bot (a bot under the water's surface: from the surface above
    // it), and `way` gets its points with the bot's own spot first; the ground guard is not asked (the bot swims the way
    // as a spline, not as a point movement).
    bool CanWalkTo(Player* player, float& x, float& y, float& z, float straight, std::vector<Creature*> const* pass = nullptr,
        float const* from = nullptr, Movement::PointsArray* way = nullptr)
    {
        PathGenerator path(player);
        if (from ? !path.CalculatePath(from[0], from[1], from[2], x, y, z, false) : !path.CalculatePath(x, y, z))
            return false;
        uint32 const type = path.GetPathType();
        if (!(type & PATHFIND_NORMAL) || (type & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH |
            PATHFIND_FARFROMPOLY)) || path.getPathLength() > straight * 1.8f)
            return false;
        G3D::Vector3 const& end = path.GetActualEndPosition();
        float const dx = end.x - x;
        float const dy = end.y - y;
        if (dx * dx + dy * dy > 5.0f * 5.0f)
            return false;
        if (pass)
        {
            constexpr float ReachMargin = 8.0f;
            constexpr float MinReach = 14.0f;
            constexpr float StepYards = 5.0f;
            Movement::PointsArray const& points = path.GetPath();
            for (Creature const* creature : *pass)
            {
                float reach = std::max(MinReach, creature->GetAggroRange(player) + ReachMargin);
                // One it already stands within that margin of has not seen it yet unless it stands within its aggro
                // range: the path may not lead nearer to it than it stands now and into that range. (A level-18 warrior
                // at 23 % health among nine Bristlebacks "walked 58 yd away when something came near" straight through
                // their camp, every one of them within the margin and so none counted, and died on the way, round 196.)
                if (float const now = creature->GetExactDist2d(player); now <= reach)
                {
                    reach = std::min(now - 2.0f, creature->GetAggroRange(player) + 3.0f);
                    if (reach <= 0.0f || creature->IsInCombat())
                        continue;
                }
                for (std::size_t i = 1; i < points.size(); ++i)
                {
                    G3D::Vector3 const& a = points[i - 1];
                    G3D::Vector3 const& b = points[i];
                    float const length = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
                    uint32 const steps = std::max<uint32>(1, uint32(length / StepYards));
                    for (uint32 step = 1; step <= steps; ++step)
                    {
                        float const t = float(step) / float(steps);
                        if (creature->GetExactDist2d(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t) <= reach)
                            return false;
                    }
                }
            }
        }
#ifdef GA_ESCAPE_GROUND_GUARD
        if (!from && sGroundGuard.WouldCancel(player, end.x, end.y, end.z))
            return false;
#endif
        if (way)
        {
            way->clear();
            way->emplace_back(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
            for (G3D::Vector3 const& point : path.GetPath())
                way->push_back(point);
        }
        x = end.x;
        y = end.y;
        z = end.z;
        return true;
    }

    bool CanRun(Player const* player, bool mounted = false)
    {
        return (mounted || !player->IsMounted()) && !player->IsInFlight() && !player->GetTransport() && !player->GetVehicle() &&
            !player->GetMap()->Instanceable() &&
            !player->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING);
    }

    // A creature that does not stay at its spawn (so no spot can be chosen around it): a rare, an elite, or on a patrol.
    bool IsRoamer(Creature* creature)
    {
        if (creature->isElite() || creature->GetCreatureTemplate()->rank == CREATURE_ELITE_RARE)
            return true;
        return creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE;
    }

    // A strong creature that keeps to its place: it has a spawn, walks no round (no waypoints, a random walk of a few
    // yards at most), is neither fighting nor on its way home, and stands within a few yards of its home. `wander`: how
    // far it walks about. It does not come at a bot that passes it outside its reach. Jin'Zallah the Sandbringer, a
    // level-46 rare elite, stands still 72 yards from the water hole of "Gadgetzan Water Survey": a level-43 paladin
    // treated it as one on a round (a way that led within 40 yards of it was a way into it), kept away from it a dozen
    // times in five minutes, ended its task at the pool each time, wrote Tanaris off and waited minutes for a voyage.
    // (Up to 30 yards of a random walk or of a waypoint path around its spawn is a small area: the bronze dragons that
    // guard the Caverns of Time walk 16 to 18 yards about, and a level-48 warrior on its way to a vendor there waited
    // for Chronalis to pass, 20 s at a time, five times in three minutes; it never leaves.)
    bool KeepsToItsPlace(Creature* creature, float* wander = nullptr)
    {
        constexpr float MaxWanderYards = 30.0f;
        constexpr float HomeYards = 5.0f;
        if (!creature->GetSpawnId() || creature->IsInCombat() || creature->IsInEvadeMode())
            return false;
        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
        creature->GetRespawnPosition(sx, sy, sz);
        MovementGeneratorType const type = creature->GetMotionMaster()->GetCurrentMovementGeneratorType();
        float roam = 0.0f;
        if (type == RANDOM_MOTION_TYPE || type == ANIMAL_RANDOM_MOTION_TYPE)
            roam = creature->GetWanderDistance();
        else if (type == WAYPOINT_MOTION_TYPE)
        {
            // A path that keeps near its spawn (how far its farthest point lies from there).
            WaypointPath const* path = creature->GetWaypointPath() ? sWaypointMgr->GetPath(creature->GetWaypointPath()) : nullptr;
            if (!path || path->Nodes.empty())
                return false;
            for (WaypointNode const& node : path->Nodes)
                roam = std::max(roam, std::hypot(node.X - sx, node.Y - sy));
        }
        else if (type != IDLE_MOTION_TYPE)
            return false;
        if (roam > MaxWanderYards)
            return false;
        if (creature->GetExactDist2d(sx, sy) > roam + HomeYards)
            return false;
        if (wander)
            *wander = roam;
        return true;
    }

    // How far the bot's way keeps from a creature that keeps to its place: its aggro range for the bot, how far it walks
    // about, and StandClearYards.
    float StandReach(Creature* creature, Player const* player, float wander)
    {
        return creature->GetAggroRange(player) + wander + StandClearYards;
    }

    // How near the bot's way comes to a creature over its next `aheadYards`: along the path of its movement, from the
    // path point nearest to the bot on. A bot that does not move: its distance to the creature.
    float WayClosest(Player* player, Creature const* creature, float aheadYards)
    {
        float closest = creature->GetExactDist2d(player);
        if (!player->isMoving() || player->movespline->Finalized())
            return closest;
        auto const& path = player->movespline->_Spline().getPoints();
        if (path.empty())
            return closest;
        float const px = player->GetPositionX();
        float const py = player->GetPositionY();
        std::size_t nearest = 0;
        float nearestDist = -1.0f;
        for (std::size_t i = 0; i < path.size(); ++i)
        {
            float const d = std::hypot(path[i].x - px, path[i].y - py);
            if (nearestDist < 0.0f || d < nearestDist)
            {
                nearest = i;
                nearestDist = d;
            }
        }
        float fromX = px;
        float fromY = py;
        float walked = 0.0f;
        for (std::size_t i = std::min(nearest + 1, path.size() - 1); i < path.size() && walked < aheadYards; ++i)
        {
            closest = std::min(closest, SegmentDistance(creature, fromX, fromY, path[i].x, path[i].y));
            walked += std::hypot(path[i].x - fromX, path[i].y - fromY);
            fromX = path[i].x;
            fromY = path[i].y;
        }
        return closest;
    }

    // The level a roaming creature is worth: its own, plus what its health and damage are worth against an ordinary
    // creature's (a level as much as a fight's cost grows per level: a rare elite with three times the health and 1.7
    // times the damage counts seven levels more; an ogre patrol of that kind one level above a warlock killed it,
    // and the plain level rule did not see it).
    // Whole levels only (rounded down): rounded up, any creature a little above the plain one counted two levels
    // more. A Highland Thrasher (level 34, an ordinary raptor with 1.1 times the health and 1.2 times the damage)
    // was "level 36" for a level-32 paladin, which kept away from it four times in 40 seconds, ended a task each
    // time, left the one fight it had with it at 95 % health and lost the zone.
    uint32 ThreatLevelOf(CreatureTemplate const* info, uint32 level)
    {
        if (!info)
            return level;
        float const strength = std::max(1.0f, info->ModHealth) * std::max(1.0f, info->DamageModifier);
        return level + uint32(std::floor(std::log(strength) / std::log(StrengthPerLevel) + 0.01f));
    }

    uint32 ThreatLevelOf(Creature const* creature)
    {
        return ThreatLevelOf(creature->GetCreatureTemplate(), creature->GetLevel());
    }

    // The strength by which a creature counts as one the bot keeps away from, waits for or does not pull beside
    // (ThreatLevels above the bot): an elite by its health and damage (ThreatLevelOf), an ordinary creature, a rare
    // that is no elite included, by its level alone (the owner, 2026-11-14: [quote left out]). Over a day of live fights a single
    // ordinary foe two or three levels above the bot cost 21-22% of its health (12 deaths in 1743), four levels above
    // 26% (6 in 71), five 39% (4 in 34), six and more 69% (32 in 58). A level-34 Highland Thrasher, an ordinary beast
    // with a little more health and damage, counted as level 36 for a level-32 paladin, which kept away from it four
    // times in 40 seconds.
    uint32 StrengthOf(Creature const* creature)
    {
        return creature->isElite() ? ThreatLevelOf(creature) : creature->GetLevel();
    }

    // A creature far above the bot by its own level (or a rare elite, a boss), not only by what an elite's health and
    // damage are worth. One elite of the second kind on the bot is fought by the fight's own numbers: a run does not
    // shake it off. Crimson Bodyguards (level 57, elite, worth 66) caught level-60 bots on their keep-aways from the
    // Crimson Courier of the Eastern Plaguelands: of the runs from one, one died after 29 s, one could not shake it off
    // after 36 s and won the fight then (lowest 50 % health), one got away after 27 s; the fights stayed in were won
    // (one against four of them, lowest 29 %), round 226.
    bool StrongByLevel(Player const* player, Creature const* creature)
    {
        uint32 const rank = creature->GetCreatureTemplate()->rank;
        return creature->GetLevel() >= uint32(player->GetLevel()) + ThreatLevels || rank == CREATURE_ELITE_RARE ||
            rank == CREATURE_ELITE_WORLDBOSS;
    }

    // Divine Protection puts Forbearance on the paladin for two minutes, and Forbearance keeps Divine Shield and Lay on
    // Hands from it (spell_pal_immunities, spell_pal_lay_on_hands). Not while either of them is ready: two paladins
    // cast it as they left a fight with an elite or a rare elite and died a minute later at 11 and 38 % health with
    // neither of them to hand, round 158.
    bool KeepsForbearanceFree(Player const* player);

    // Whether a creature can get at the bot: there is a whole path between them that is not far longer than the
    // straight line. The Sewer Beast, a level-50 rare, swims in Stormwind's canals below the bridges: a level-25
    // rogue "kept away" from it four times in 70 seconds on its way to a hand-in in the city, 26 to 33 yards above
    // it, and its hand-in was set aside. Caged creatures and creatures behind a wall are the same case.
    // A creature in the water to which the bot has no path may still come ashore: it counts when it is at about
    // the bot's own height.
    bool CanReach(Player* player, Creature* creature)
    {
        constexpr float ShoreHeight = 5.0f;
        // Both in the water: nothing is between them (the path finder has no paths under water, and the rule below
        // was made for a walker on the shore: a level-46 shark on patrol swam up to a level-37 paladin 30 yards under
        // the surface twice, unseen by the look-out because it was more than five yards above or below it).
        if (creature->IsInWater() && Escape::WaterAt(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ()))
            return true;
        float const straight = creature->GetExactDist(player);
        PathGenerator path(player);
        bool const found = path.CalculatePath(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
        uint32 const type = path.GetPathType();
        if (found && (type & PATHFIND_NORMAL) && !(type & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT |
            PATHFIND_NOT_USING_PATH | PATHFIND_FARFROMPOLY)))
            return path.getPathLength() <= std::max(straight * 2.0f, straight + 25.0f);
        return creature->IsInWater() && std::fabs(creature->GetPositionZ() - player->GetPositionZ()) <= ShoreHeight;
    }

    // Strong creatures on their way, as the guild's characters see them (Escape::RoamerNear): where each was seen
    // last, how it moves, and what it is worth. A patrol, a summoned creature (Stitches comes with a quest and walks
    // the road to Darkshire: no spawn, no waypoints in the spawn data, so no spot verdict knew him) or one far from
    // its home. Kept for RoamerMemoryMs after the last sight. World thread.
    struct RoamerSight
    {
        uint32 mapId = 0;
        uint32 worth = 0;      // ThreatLevelOf
        float x = 0.0f;
        float y = 0.0f;
        float vx = 0.0f;       // yards per second while it walked
        float vy = 0.0f;
        uint32 seenAt = 0;
        std::string name;
        bool swimOnly = false; // it cannot leave the water: no matter for a point on dry land
        bool stays = false;    // it walks about the place and does not pass (StaysAround): nothing to wait for
    };
    std::unordered_map<ObjectGuid, RoamerSight> roamerSights;
    constexpr uint32 RoamerMemoryMs = 90 * IN_MILLISECONDS;
    constexpr float RoamerAroundYards = 80.0f;
    constexpr float RoamerPathYards = 60.0f;
    constexpr float RoamerAheadSeconds = 45.0f;
    constexpr float RoamerAheadMaxYards = 250.0f;
    constexpr float RoamerFromHomeYards = 40.0f;

    // A strong creature on a round that does not pass but stays where it is: a guard (it walks its town or camp), one
    // in a town of the other side, and one whose whole waypoint round lies within PatrolStaysYards of the bot. Waiting
    // for it to move on is waiting for nothing (Owldudu waited 75 seconds beside Stonard for a level-65 Stonard Grunt
    // on its round there, its task ended, and the rescue took it for a bot that had not moved for five minutes).
    constexpr float PatrolStaysYards = 150.0f;
    bool StaysAround(Player* player, Creature* creature)
    {
        if (creature->IsGuard() ||
            sRoadMap.InHostileTown(creature->GetMapId(), creature->GetPositionX(), creature->GetPositionY(), player->GetTeamId()))
            return true;
        if (creature->GetMotionMaster()->GetCurrentMovementGeneratorType() != WAYPOINT_MOTION_TYPE)
            return false;
        uint32 const pathId = creature->GetWaypointPath();
        WaypointPath const* path = pathId ? sWaypointMgr->GetPath(pathId) : nullptr;
        if (!path || path->Nodes.empty())
            return false;
        // A short round (the Stonard Grunts walk loops of four points some forty yards across, 100 to 250 yards from
        // where the bot waited) never takes it away; nor does a round that lies wholly around the bot.
        constexpr float ShortRoundYards = 60.0f;
        float cx = 0.0f, cy = 0.0f;
        for (WaypointNode const& node : path->Nodes)
        {
            cx += node.X;
            cy += node.Y;
        }
        cx /= float(path->Nodes.size());
        cy /= float(path->Nodes.size());
        bool shortRound = true;
        bool aroundBot = true;
        for (WaypointNode const& node : path->Nodes)
        {
            shortRound = shortRound && std::hypot(node.X - cx, node.Y - cy) <= ShortRoundYards;
            aroundBot = aroundBot && player->GetExactDist2d(node.X, node.Y) <= PatrolStaysYards;
        }
        return shortRound || aroundBot;
    }

    void NoteRoamer(Creature* creature, Player* seenBy = nullptr)
    {
        Position const& home = creature->GetHomePosition();
        bool const onItsWay = !creature->GetSpawnId() ||
            creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE ||
            creature->GetExactDist2d(home.GetPositionX(), home.GetPositionY()) > RoamerFromHomeYards;
        if (!onItsWay)
            return;
        uint32 const now = getMSTime();
        RoamerSight& sight = roamerSights[creature->GetGUID()];
        uint32 const since = sight.seenAt ? getMSTimeDiff(sight.seenAt, now) : 0;
        if (sight.seenAt && since < 500)
            return;
        // How it walks: between two sights of it out of a fight (in a fight it chases; its walk goes on afterwards).
        if (sight.seenAt && since <= 10 * IN_MILLISECONDS && !creature->IsInCombat())
        {
            float const seconds = float(since) / float(IN_MILLISECONDS);
            float const vx = (creature->GetPositionX() - sight.x) / seconds;
            float const vy = (creature->GetPositionY() - sight.y) / seconds;
            float const speed = std::hypot(vx, vy);
            if (speed >= 0.5f && speed <= 14.0f)
            {
                sight.vx = vx;
                sight.vy = vy;
            }
            else if (speed < 0.5f && since >= 4 * IN_MILLISECONDS)
                sight.vx = sight.vy = 0.0f; // it stands
        }
        sight.mapId = creature->GetMapId();
        sight.worth = StrengthOf(creature);
        sight.x = creature->GetPositionX();
        sight.y = creature->GetPositionY();
        sight.seenAt = now;
        sight.swimOnly = !creature->CanWalk() && creature->CanSwim();
        if (seenBy)
            sight.stays = StaysAround(seenBy, creature);
        if (sight.name.empty())
            sight.name = Acore::StringFormat("{} (level {})", creature->GetName(), uint32(creature->GetLevel()));
    }

    bool InRoamerZone(RoamerSight const& sight, float x, float y, uint32 now)
    {
        if (std::hypot(x - sight.x, y - sight.y) <= RoamerAroundYards)
            return true;
        float const speed = std::hypot(sight.vx, sight.vy);
        if (speed < 0.5f)
            return false;
        float const age = float(getMSTimeDiff(sight.seenAt, now)) / float(IN_MILLISECONDS);
        float const reach = std::min(RoamerAheadMaxYards, speed * (age + RoamerAheadSeconds));
        float const ux = sight.vx / speed;
        float const uy = sight.vy / speed;
        float const along = std::clamp((x - sight.x) * ux + (y - sight.y) * uy, 0.0f, reach);
        return std::hypot(x - (sight.x + ux * along), y - (sight.y + uy * along)) <= RoamerPathYards;
    }

    // Water at a point of an open-world map, without a character: the water's level lies above the ground (a pier
    // above the water is ground).
    bool WaterPoint(uint32 mapId, float x, float y)
    {
        Map const* map = sMapMgr->FindBaseNonInstanceMap(mapId);
        if (!map)
            return false;
        float const water = map->GetWaterLevel(x, y);
        if (water <= INVALID_HEIGHT)
            return false;
        float const ground = map->GetHeight(x, y, MAX_HEIGHT);
        return ground <= INVALID_HEIGHT || water > ground + 1.0f;
    }

    // The strongest roaming creature within `range` that would attack the bot and is far above its level.
    Creature* FindThreat(Player* player, float range)
    {
        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, range);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, units, check);
        Cell::VisitObjects(player, searcher, range);

        Creature* worst = nullptr;
        for (Unit* unit : units)
        {
            Creature* creature = unit->ToCreature();
            // (A creature that is walking back from a fight counts: it is back on its round in seconds. Stitches, a
            // level-35 elite that walks the Raven Hill road, had just killed one bot; the next one's keep-away was
            // "clear of it after 0 s" at 26 yards while it turned back, walked on, and was dead 19 seconds later.)
            if (!creature || creature->IsPet() || creature->IsTotem() ||
                creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_IMMUNE_TO_PC) || !creature->IsHostileTo(player) ||
                StrengthOf(creature) < uint32(player->GetLevel()) + ThreatLevels)
                continue;
            // A civilian never attacks (the other side's flight master in a neutral town: Gringer, level 65 elite, beside
            // the Alliance's flight master in Booty Bay; every walk of a level-32 bot there ended "kept away from a
            // creature far above its level", the walk to its own flight master included). Nor does one immune to players
            // (filtered above): the Horde's Bountiful Feast Hostess of Pilgrim's Bounty, level 80, beside the inn of Light's
            // Hope Chapel, held every Alliance bot off the inn and its vendors, 2442 log lines in a day, round 158.
            // A civilian in a fight with the bot does attack: something of the bot's hit it. Twilight Marauder Morna, a
            // level-60 elite on a long round through Silithus with 3.45 times the health and 4.6 times the damage, is
            // flagged a civilian; a level-60 mage's fight drew her in, the look-out let her be for 18 seconds while she
            // was on it, it left by the estimate at 73 % health with its Frost Nova spent and died on the run, round 192.
            if (creature->IsCivilian() && !creature->IsInCombatWith(player))
                continue;
            if (IsRoamer(creature))
                NoteRoamer(creature, player);
            // A rare, an elite or a patrol is seen from afar. An ordinary creature that far above the bot only once
            // the bot comes near its reach: Bellygrub, a level-24 boar that wanders among the level-16 boars of a
            // quest, killed a level-17 paladin and a level-19 warrior there (neither rare nor elite nor on a
            // patrol, so nothing kept away from it).
            constexpr float NearReachYards = 8.0f;
            if (!IsRoamer(creature) && creature->GetExactDist(player) > creature->GetAggroRange(player) + NearReachYards)
                continue;
            // (Looked at last: it costs a path.)
            if ((!worst || StrengthOf(creature) > StrengthOf(worst)) && !CanReach(player, creature))
                continue;
            if (!worst || StrengthOf(creature) > StrengthOf(worst))
                worst = creature;
        }
        return worst;
    }

    // What a walking bot is about to walk into: two or more hostile creatures that are not fighting and whose reach
    // covers the next 45 yards of its way, when the fight with them is more than it can afford (the pull check's
    // rule). Returns the nearest of them, `count` how many. A bot on a mount rides through; a single creature it
    // fights.
    // The next `aheadYards` of the bot's way along the path of its movement (the core's path points, from the one
    // nearest to the bot on), as a line of points from where the bot stands; `beyond`: that much more along the last
    // piece when the movement ends sooner (a rider's next step leads on the same way). Empty when it does not move.
    // (The looks on the way took the straight line to the movement's end: a long walk's end lay 392 yards off to the
    // south-east while its path curved first through the Bloodsail camp of the Southern Savage Coast, eight creatures
    // within 60 yards and "none on its way", and a shaman rode into the warlocks and their succubi there three times
    // in one evening: Shammpower's sixth death at that camp, staged and repeated on the dev instance.)
    std::vector<std::pair<float, float>> WayAhead(Player* player, float aheadYards, float beyond)
    {
        std::vector<std::pair<float, float>> way;
        if (!player->isMoving() || player->movespline->Finalized())
            return way;
        float const px = player->GetPositionX();
        float const py = player->GetPositionY();
        way.emplace_back(px, py);
        auto const& path = player->movespline->_Spline().getPoints();
        if (path.empty())
            return way;
        std::size_t nearest = 0;
        float nearestDist = -1.0f;
        for (std::size_t i = 0; i < path.size(); ++i)
        {
            float const d = std::hypot(path[i].x - px, path[i].y - py);
            if (nearestDist < 0.0f || d < nearestDist)
            {
                nearest = i;
                nearestDist = d;
            }
        }
        float walked = 0.0f;
        for (std::size_t i = std::min(nearest + 1, path.size() - 1); i < path.size() && walked < aheadYards; ++i)
        {
            auto const [fx, fy] = way.back();
            float const step = std::hypot(path[i].x - fx, path[i].y - fy);
            if (step < 0.5f)
                continue;
            if (walked + step > aheadYards)
            {
                float const t = (aheadYards - walked) / step;
                way.emplace_back(fx + (path[i].x - fx) * t, fy + (path[i].y - fy) * t);
                walked = aheadYards;
                break;
            }
            way.emplace_back(path[i].x, path[i].y);
            walked += step;
        }
        if (walked < aheadYards && beyond > 0.0f && way.size() >= 2)
        {
            auto const [ax, ay] = way[way.size() - 2];
            auto const [bx, by] = way.back();
            float const last = std::hypot(bx - ax, by - ay);
            float const more = std::min(beyond, aheadYards - walked);
            if (last > 0.5f)
                way.emplace_back(bx + (bx - ax) / last * more, by + (by - ay) / last * more);
        }
        return way;
    }

    // How near the way (WayAhead) comes to a creature.
    float WayDistanceAlong(Creature const* creature, std::vector<std::pair<float, float>> const& way)
    {
        if (way.empty())
            return std::numeric_limits<float>::max();
        if (way.size() == 1)
            return std::hypot(creature->GetPositionX() - way[0].first, creature->GetPositionY() - way[0].second);
        float closest = std::numeric_limits<float>::max();
        for (std::size_t i = 1; i < way.size(); ++i)
            closest = std::min(closest, SegmentDistance(creature, way[i - 1].first, way[i - 1].second, way[i].first, way[i].second));
        return closest;
    }

    // How far a creature that walks about at random may come from where it stands now, and the home it walks about
    // (its spawn; a minion its owner's). 0 for one that stands or walks a round. At most WalkAboutMaxYards.
    constexpr float WalkAboutMaxYards = 15.0f;
    float WalkAbout(Creature const* creature, float& homeX, float& homeY)
    {
        Creature const* walker = creature;
        if (!creature->GetSpawnId() && creature->IsSummon())
            if (Unit const* owner = creature->ToTempSummon()->GetSummonerUnit(); owner && owner->ToCreature())
                walker = owner->ToCreature();
        if (!walker->GetSpawnId() || walker->IsInCombat() || walker->IsInEvadeMode())
            return 0.0f;
        MovementGeneratorType const type = walker->GetMotionMaster()->GetCurrentMovementGeneratorType();
        if (type != RANDOM_MOTION_TYPE && type != ANIMAL_RANDOM_MOTION_TYPE)
            return 0.0f;
        float homeZ = 0.0f;
        walker->GetRespawnPosition(homeX, homeY, homeZ);
        return std::min(walker->GetWanderDistance(), WalkAboutMaxYards);
    }

    // How near the way comes to a point.
    float PointWayDistance(float x, float y, std::vector<std::pair<float, float>> const& way)
    {
        float closest = std::numeric_limits<float>::max();
        for (std::size_t i = 1; i < way.size(); ++i)
        {
            float const wx = way[i].first - way[i - 1].first;
            float const wy = way[i].second - way[i - 1].second;
            float const length2 = wx * wx + wy * wy;
            float const ax = x - way[i - 1].first;
            float const ay = y - way[i - 1].second;
            float const along = length2 > 1.0f ? std::clamp((ax * wx + ay * wy) / length2, 0.0f, 1.0f) : 0.0f;
            closest = std::min(closest, std::hypot(ax - along * wx, ay - along * wy));
        }
        return closest;
    }

    // How near a creature may come to the way: where it stands, or anywhere its random walk about its home takes it
    // (a minion goes with its owner). Hordeboss, a level-45 warrior, went around three Wastewander Shadow Mages and
    // their voidwalkers in Tanaris, rode on along the circle's edge, and a mage that walks 15 yd about its spot had
    // walked towards the way: two were on it, and on the live server it died there, round 242.
    float NearestOnWay(Creature const* creature, std::vector<std::pair<float, float>> const& way)
    {
        float const now = WayDistanceAlong(creature, way);
        float hx = 0.0f, hy = 0.0f;
        if (float const walk = WalkAbout(creature, hx, hy); walk > 0.0f && way.size() >= 2)
            return std::min(now, std::max(0.0f, PointWayDistance(hx, hy, way) - walk));
        return now;
    }

    Creature* FindPackAhead(Player* player, BotRuntime const& rt, uint32& count, std::vector<Creature*>* pack = nullptr,
        float* destX = nullptr, float* destY = nullptr)
    {
        constexpr float AheadYards = 45.0f;
        // The creatures at the place the bot is going to fight at are not "on its way": the pull check decides about
        // them when it is there. (In the first live window without this, 36 tasks in 22 minutes ended on "a pack on
        // its way", many of them at the camp the quest sends the bot to.)
        constexpr float OwnCampYards = 30.0f;
        count = 0;
        if (!player->isMoving() || player->movespline->Finalized())
            return nullptr;
        G3D::Vector3 const destination = player->movespline->FinalDestination();
        float const dx = destination.x - player->GetPositionX();
        float const dy = destination.y - player->GetPositionY();
        float const length = std::sqrt(dx * dx + dy * dy);
        if (length < 10.0f)
            return nullptr;
        // (A rider's look goes the whole 45 yards along its heading, also beyond the end of a short spline: it
        // covers a step of the long-distance walk in a second or two, and the next step leads on the same way. A
        // rogue rode into the Kurzen compound and saw the first pack when one of it was two yards away.)
        // (Not the whole 45 yards beyond it, as a first build did: the next step often turns, and the line then ran
        // through camps the road passes. Rider keep-aways rose from 12 to 34 in a round, and tasks ended on packs
        // 25 to 29 yards to the side. Fifteen yards beyond the step's end.)
        constexpr float RiderBeyondYards = 15.0f;
        // Along the movement's own path, not the straight line to its end (WayAhead).
        std::vector<std::pair<float, float>> const way = WayAhead(player, AheadYards, player->IsMounted() ? RiderBeyondYards : 0.0f);
        if (way.size() < 2)
            return nullptr;
        float const tx = way.back().first;
        float const ty = way.back().second;

        // A rider passes closer than a walker dares to: only what would really have it in its aggro radius counts
        // (roads lead past camps). Riders were left out at first, and from level 20 every bot rides: a level-22
        // rogue rode off to its grind target through three murlocs the pull check had refused seconds before, twice
        // in forty seconds, and died of the second time.
        constexpr float RiderMargin = 2.0f;
        bool const riding = player->IsMounted();
        BotTask const& task = rt.task;
        bool const fightsThere = task.hasSpot && task.spot.mapId == player->GetMapId() &&
            (task.kind == TaskKind::Objective || task.kind == TaskKind::Grind || task.kind == TaskKind::Interact || task.kind == TaskKind::Event);
        // Only what the way leads towards. A creature the bot stands beside and walks away from is not "on its
        // way": the walk is what takes the bot out of its reach, and ending the task there keeps it beside the
        // creature. A hunter stood in the Kurzen compound, creatures 27 to 37 yards away on several sides; eight
        // pickups at a camp 600 yards away and then its own retreat ended "a pack on its way" within seconds, 350
        // seconds in all, each target set aside for a quarter of an hour. The way has to bring the bot at least
        // ApproachYards nearer to a creature than it is now.
        constexpr float ApproachYards = 3.0f;
        auto approaches = [&](Creature const* creature)
        {
            return WayDistanceAlong(creature, way) + ApproachYards <= creature->GetExactDist2d(player);
        };
        std::vector<Creature*> ahead;
        for (Creature* creature : LiveHostiles(player, AheadYards + 15.0f, uint8(Escape::GroupLevelsBelow + 1)))
        {
            if (creature->IsInCombat())
                continue;
            if (fightsThere && creature->GetExactDist(task.spot.x, task.spot.y, task.spot.z) <= OwnCampYards)
                continue;
            if (!approaches(creature))
                continue;
            // The group of a pull the bot has just called off is known to be too much: its way keeps KnownPackYards
            // off every one of them, rider or not (a hunter called a pull at the Bloodsail camp off, "6 more would
            // join", was given a vendor 203 yards away, rode off past the same camp, and five were on it 25 seconds
            // later; a rider's look counts only what would have it in its aggro radius).
            constexpr float KnownPackYards = 20.0f;
            constexpr float KnownPackAround = 30.0f;
            bool const known = rt.pullHoldUntil && getMSTime() < rt.pullHoldUntil && rt.pullHoldMap == player->GetMapId() &&
                creature->GetDistance2d(rt.pullHoldX, rt.pullHoldY) <= KnownPackAround;
            float const onWay = NearestOnWay(creature, way);
            if (known ? onWay <= KnownPackYards :
                (riding ? onWay <= creature->GetAggroRange(player) + RiderMargin :
                    onWay <= std::max(12.0f, creature->GetAggroRange(player) + 5.0f)))
                ahead.push_back(creature);
        }
        // ... and what comes when one of those is attacked: the core's call for help (CreatureFamilyAssistanceRadius,
        // repeated every few seconds for as long as a creature fights) brings what stands beside it. A shaman walked
        // along the edge of the Bloodsail camp by Booty Bay to a herb, the look counted the two raiders its way passed
        // and judged two within its means, and three, then six were on it (Shammpower, round 108: the fourth death
        // at that camp that day).
        if (!ahead.empty())
        {
            constexpr float CallMarginYards = 2.0f;
            float const assistYards = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS) + CallMarginYards;
            std::size_t const inReach = ahead.size();
            for (Creature* creature : LiveHostiles(player, AheadYards + 15.0f + assistYards, uint8(Escape::GroupLevelsBelow + 1)))
            {
                if (creature->IsInCombat() || std::find(ahead.begin(), ahead.end(), creature) != ahead.end())
                    continue;
                if (fightsThere && creature->GetExactDist(task.spot.x, task.spot.y, task.spot.z) <= OwnCampYards)
                    continue;
                for (std::size_t i = 0; i < inReach; ++i)
                    if (creature->GetExactDist(ahead[i]) <= assistYards && Escape::WouldAssist(creature, ahead[i], player))
                    {
                        ahead.push_back(creature);
                        break;
                    }
            }
        }
        if (ahead.size() < 2)
            return nullptr;

        uint32 serious = 0;
        for (Creature const* creature : ahead)
            if (creature->GetLevel() + 1 >= player->GetLevel())
                ++serious;
        uint32 const maxFoes = uint32(sBotSettings.Get(player, Setting::EscapeMaxFoes)) * sSquadService.CountTogether(player, SquadPartnerYards);
        // (A fight on its way is no fight for its work: four or more count as they scatter, CrowdTail.)
        float const need = FightNeed(player, ahead, CostOf(rt, player->GetLevel()), nullptr, &rt) * Escape::CrowdTail(float(ahead.size()));
        if (!(maxFoes && serious >= maxFoes) && need <= Budget(player) * sBotSettings.GetFloat(player, Setting::RiskMaxFightCostPercent))
        {
            // (With the debug setting, at most every five seconds: what the look saw and why it let the way be.)
            static std::unordered_map<ObjectGuid, uint32> noted;
            if (GetSettings().debug && getMSTimeDiff(noted[player->GetGUID()], getMSTime()) >= 5 * IN_MILLISECONDS)
            {
                noted[player->GetGUID()] = getMSTime();
                std::string names;
                for (Creature const* creature : ahead)
                    names += Acore::StringFormat("{}{} {} ({:.0f} yd)", names.empty() ? "" : ", ", creature->GetName(),
                        uint32(creature->GetLevel()), player->GetDistance(creature));
                LOG_INFO(ActivityLog, "activity: {} pack look: {} on its way ({}), {} within a level, the fight about {:.0f}% of its {:.0f}%: "
                    "it takes them", rt.name, ahead.size(), names, serious, need * 100.0f, Budget(player) * 100.0f);
            }
            return nullptr;
        }

        count = uint32(ahead.size());
        Creature* nearest = ahead.front();
        for (Creature* creature : ahead)
            if (creature->GetExactDist(player) < nearest->GetExactDist(player))
                nearest = creature;
        if (pack)
            *pack = ahead;
        if (destX && destY)
        {
            *destX = destination.x;
            *destY = destination.y;
        }
        return nearest;
    }

    // A pack on its round coming at a bot that stands (between two grind targets, resting, at an NPC; a moving bot has
    // FindPackAhead): creatures that walk a round, or with a formation leader that does, whose next leg leads into their
    // reach of the bot and nearer to it than they are now, with their formation and what answers their call for help,
    // and that it cannot take together. Skelwyn, a level-30 rogue grinding in Thousand Needles, stood six seconds after
    // a won fight when Achellios the Banished (a level-31 rare on a waypoint round) and his three Galak Packhounds came
    // along their round onto it, six foes in all, and it died, round 243.
    Creature* FindPackComing(Player* player, BotRuntime const& rt, uint32& count, std::vector<Creature*>* pack)
    {
        constexpr float LookYards = 45.0f;
        constexpr float ApproachYards = 3.0f;
        count = 0;
        if (player->isMoving() && !player->movespline->Finalized())
            return nullptr;
        auto const onRound = [](Creature const* one)
        {
            return one->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE ||
                one->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE;
        };
        std::vector<Creature*> coming;
        for (Creature* creature : LiveHostiles(player, LookYards, uint8(Escape::GroupLevelsBelow + 1)))
        {
            if (creature->IsInCombat() || !creature->isMoving() || creature->movespline->Finalized())
                continue;
            CreatureGroup const* group = creature->GetFormation();
            Creature const* leader = group ? group->GetLeader() : nullptr;
            if (!onRound(creature) && !(leader && leader != creature && onRound(leader)))
                continue;
            G3D::Vector3 const end = creature->movespline->FinalDestination();
            float const reach = std::max(12.0f, creature->GetAggroRange(player) + 5.0f);
            std::vector<std::pair<float, float>> const leg{ { creature->GetPositionX(), creature->GetPositionY() }, { end.x, end.y } };
            float const closest = PointWayDistance(player->GetPositionX(), player->GetPositionY(), leg);
            if (closest > reach || closest + ApproachYards > creature->GetExactDist2d(player))
                continue;
            coming.push_back(creature);
        }
        if (coming.empty())
            return nullptr;
        // Its formation and its helpers come with it.
        std::vector<Creature*> all = coming;
        float const assistYards = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS) + 2.0f;
        for (Creature* creature : coming)
            if (CreatureGroup const* group = creature->GetFormation())
                for (auto const& [member, info] : group->GetMembers())
                    if (member && member->IsAlive() && !member->IsInCombat() && member->IsInWorld() &&
                        std::find(all.begin(), all.end(), member) == all.end() && member->IsHostileTo(player))
                        all.push_back(member);
        std::size_t const inReach = all.size();
        for (Creature* creature : LiveHostiles(player, LookYards + assistYards, uint8(Escape::GroupLevelsBelow + 1)))
        {
            if (creature->IsInCombat() || std::find(all.begin(), all.end(), creature) != all.end())
                continue;
            for (std::size_t i = 0; i < inReach; ++i)
                if (creature->GetExactDist(all[i]) <= assistYards && Escape::WouldAssist(creature, all[i], player))
                {
                    all.push_back(creature);
                    break;
                }
        }
        if (all.size() < 2)
            return nullptr;
        uint32 serious = 0;
        for (Creature const* creature : all)
            if (creature->GetLevel() + 1 >= player->GetLevel())
                ++serious;
        uint32 const maxFoes = uint32(sBotSettings.Get(player, Setting::EscapeMaxFoes)) * sSquadService.CountTogether(player, SquadPartnerYards);
        float const need = FightNeed(player, all, CostOf(rt, player->GetLevel()), nullptr, &rt) * Escape::CrowdTail(float(all.size()));
        if (!(maxFoes && serious >= maxFoes) && need <= Budget(player) * sBotSettings.GetFloat(player, Setting::RiskMaxFightCostPercent))
            return nullptr;
        count = uint32(all.size());
        Creature* nearest = coming.front();
        for (Creature* creature : coming)
            if (creature->GetExactDist(player) < nearest->GetExactDist(player))
                nearest = creature;
        if (pack)
            *pack = all;
        return nearest;
    }

    // In the middle of a camp: creatures that would attack on every side. Four or more within CampAroundYards that
    // are not fighting and at most three levels below the bot, half of them or more standing with another one (a
    // field of single creatures is where a bot works, one at a time), and no gap of 150 degrees between two
    // neighbours as seen from the bot (through a gap that wide it is at the camp's edge, and walks off).
    // `nearest`: the nearest of them.
    constexpr float CampAroundYards = 55.0f;
    // One number per task and target (0: no task), to tell a new task from the one looked at before.
    uint64 TaskKeyOf(BotTask const& task)
    {
        if (task.kind == TaskKind::None)
            return 0;
        uint64 key = (uint64(task.kind) << 56) ^ (uint64(task.questId) << 34);
        if (task.hasSpot)
            key ^= ((uint64(task.spot.spawnId) << 1) | uint64(task.spot.gameObject ? 1 : 0)) ^
                (uint64(uint32(int32(task.spot.x)) & 0xFFFFF) << 14) ^ (uint64(uint32(int32(task.spot.y)) & 0xFFFFF) << 3);
        return key ? key : 1;
    }

    // `levelsBelow`: creatures farther below the bot than this do not count (three for a camp the bot keeps out
    // of; six for the question whether it should get out of where it stands: four creatures five levels below a
    // level-38 hunter took it from 100 to 58 % in that compound).
    bool Surrounded(Player* player, Creature** nearest = nullptr, uint32* count = nullptr, int32 levelsBelow = 3)
    {
        constexpr std::size_t MinAround = 4;
        constexpr float OpenGap = 150.0f * Pi / 180.0f;
        constexpr float TogetherYards = 12.0f;
        constexpr float SameYards = 8.0f;
        std::vector<float> bearings;
        std::vector<std::pair<float, float>> around;
        Creature* closest = nullptr;
        for (Creature* creature : LiveHostiles(player, CampAroundYards, PackLevelsBelow))
        {
            if (creature->IsInCombat() || int32(creature->GetLevel()) + levelsBelow < int32(player->GetLevel()))
                continue;
            around.emplace_back(creature->GetPositionX(), creature->GetPositionY());
            if (!closest || creature->GetExactDist(player) < closest->GetExactDist(player))
                closest = creature;
        }
        // The spawns count too: creatures the bot has just killed around it are back in minutes. (A rogue stood
        // where it had killed two Kurzen in the north of their compound, was "clear" of the next pack, set out
        // from there, and three were on it from the respawns and the packs beside them.)
        if (closest)
            for (auto const& place : Risk::HostileSpawnsNear(player, player->GetPositionX(), player->GetPositionY(), CampAroundYards, levelsBelow))
            {
                bool known = false;
                for (auto const& other : around)
                    known = known || std::hypot(other.first - place.first, other.second - place.second) <= SameYards;
                if (!known)
                    around.push_back(place);
            }
        for (auto const& place : around)
            bearings.push_back(std::atan2(place.second - player->GetPositionY(), place.first - player->GetPositionX()));
        if (nearest)
            *nearest = closest;
        if (count)
            *count = uint32(bearings.size());
        if (!closest || bearings.size() < MinAround)
            return false;
        std::size_t together = 0;
        for (std::size_t i = 0; i < around.size(); ++i)
            for (std::size_t j = 0; j < around.size(); ++j)
                if (i != j && std::hypot(around[i].first - around[j].first, around[i].second - around[j].second) <= TogetherYards)
                {
                    ++together;
                    break;
                }
        if (together * 2 < around.size())
            return false;
        std::sort(bearings.begin(), bearings.end());
        float widest = bearings.front() + 2.0f * Pi - bearings.back();
        for (std::size_t i = 1; i < bearings.size(); ++i)
            widest = std::max(widest, bearings[i] - bearings[i - 1]);
        return widest < OpenGap;
    }
    // Out of a camp it goes for this long and this far at most.
    constexpr int32 LeaveLevelsBelow = 6;
    constexpr uint32 CampMaxMs = 40 * IN_MILLISECONDS;
    constexpr float CampMaxYards = 160.0f;

    // A camp ahead (RiskModel.h, from the spawn data): the bot's way leads into the middle of a camp it cannot take.
    // The look goes along the movement to its end and up to CampBeyondYards further along the heading (the next
    // step of a long walk leads on the same way), CampAheadYards at most. The creatures alive around the place where
    // the way enters the camp are the "pack" the caller goes around or keeps away from; fewer than two alive (the
    // camp is dead for the moment): nothing. Not the camp the task's own fight is in (the spot's verdict and the
    // check before a pull decide there), and not while the bot stands in a camp already (its way out leads
    // through it).
    Creature* FindCampAhead(Player* player, BotRuntime const& rt, uint32& count, std::vector<Creature*>& pack, float& destX,
        float& destY)
    {
        constexpr float CampAheadYards = 90.0f;
        constexpr float CampBeyondYards = 30.0f;
        constexpr float OwnSpotYards = 80.0f;
        constexpr float EntryYards = 55.0f;
        count = 0;
        if (!player->isMoving() || player->movespline->Finalized())
            return nullptr;
        G3D::Vector3 const destination = player->movespline->FinalDestination();
        float const px = player->GetPositionX();
        float const py = player->GetPositionY();
        float const dx = destination.x - px;
        float const dy = destination.y - py;
        float const length = std::sqrt(dx * dx + dy * dy);
        if (length < 8.0f)
            return nullptr;
        // Along the movement's own path (WayAhead), piece by piece: where it first leads into the middle of a camp.
        std::vector<std::pair<float, float>> const way = WayAhead(player, CampAheadYards, CampBeyondYards);
        Risk::CampAt camp;
        float at = 0.0f;
        float ex = px;
        float ey = py;
        bool enters = false;
        float walked = 0.0f;
        for (std::size_t i = 1; i < way.size() && !enters; ++i)
        {
            auto const [ax, ay] = way[i - 1];
            auto const [bx, by] = way[i];
            float const piece = std::hypot(bx - ax, by - ay);
            float into = 0.0f;
            // (The first ten yards from where the bot stands are not looked at, as before: a camp it stands at the
            // edge of is the camp-leaving look's matter.)
            float const skip = walked < 10.0f ? 10.0f - walked : 0.0f;
            if (piece > skip + 0.5f && Risk::EntersCamp(player, rt, player->GetMapId(), ax, ay, bx, by, &camp, &into, skip))
            {
                enters = true;
                at = walked + into;
                ex = ax + (bx - ax) * into / piece;
                ey = ay + (by - ay) * into / piece;
            }
            walked += piece;
        }
        if (!enters)
            return nullptr;
        if (Risk::InCampItCannotTake(player, rt, player->GetMapId(), px, py, player->GetPositionZ()))
            return nullptr;
        BotTask const& task = rt.task;
        // (Only for a spot on the bot's own level: one on a cliff above the camp is no place the bot fights at in it. Gorruk,
        // a level-42 warrior, walked along the beach of the Wild Shore through the middle of the Bloodsail camp for the Naga
        // Explorers 100 yd above it, 200 yd away, and died there with five on it, 2026-11-26; the pack look's creatures
        // beside the spot are taken in three dimensions for the same reason.)
        constexpr float SameLevelYards = 20.0f;
        bool const fightsThere = task.hasSpot && task.spot.mapId == player->GetMapId() &&
            (task.kind == TaskKind::Objective || task.kind == TaskKind::Grind || task.kind == TaskKind::Interact || task.kind == TaskKind::Event) &&
            std::fabs(task.spot.z - player->GetPositionZ()) <= SameLevelYards;
        if (fightsThere && (std::hypot(task.spot.x - ex, task.spot.y - ey) <= OwnSpotYards ||
            std::hypot(task.spot.x - camp.x, task.spot.y - camp.y) <= camp.radius + 30.0f))
            return nullptr;
        // The camp's own creatures that are alive: of its levels, within its radius. (Not whatever else stands near
        // the way in: a first build named two panthers before a Venture Co. camp whose creatures stood further in.)
        constexpr float CampEdgeYards = 12.0f;
        Creature* nearest = nullptr;
        for (Creature* creature : LiveHostiles(player, at + EntryYards + camp.radius, PackLevelsBelow))
        {
            if (creature->IsInCombat() || int32(creature->GetLevel()) + 1 < int32(camp.minLevel) ||
                int32(creature->GetLevel()) > int32(camp.maxLevel) + 1 ||
                creature->GetDistance2d(camp.x, camp.y) > camp.radius + CampEdgeYards)
                continue;
            pack.push_back(creature);
            if (!nearest || creature->GetExactDist(player) < nearest->GetExactDist(player))
                nearest = creature;
        }
        if (pack.size() < 2)
        {
            pack.clear();
            return nullptr;
        }
        count = uint32(pack.size());
        destX = destination.x;
        destY = destination.y;
        return nearest;
    }

    char const* RankName(Creature const* creature)
    {
        switch (creature->GetCreatureTemplate()->rank)
        {
            case CREATURE_ELITE_RARE:      return " rare";
            case CREATURE_ELITE_ELITE:     return " elite";
            case CREATURE_ELITE_RAREELITE: return " rare elite";
            case CREATURE_ELITE_WORLDBOSS: return " boss";
            default:                       return "";
        }
    }

    void EndAside(Player* player, BotRuntime& rt, char const* outcome)
    {
        BotRuntime::Escape const escape = rt.escape;
        rt.escape = BotRuntime::Escape();
        BotDriver::PauseAI(player, 0);
        CreatureTemplate const* foe = escape.foeEntry ? sObjectMgr->GetCreatureTemplate(escape.foeEntry) : nullptr;
        LOG_INFO(ActivityLog, "activity: {} kept away from {} (level {}): {} after {} s", rt.name, foe ? foe->Name : std::string("a creature"),
            uint32(escape.foeLevel), outcome, getMSTimeDiff(escape.startedAt, getMSTime()) / IN_MILLISECONDS);
    }

    // The next leg: away from the foes at first, then on along the heading; among seven directions the one with the
    // fewest hostile spawns on its way that the bot has a path to.
    bool StartLeg(Player* player, BotRuntime& rt, BotRuntime::Escape& escape, std::vector<Creature*> const& foes)
    {
        // What stands around that is not in the fight yet.
        std::vector<Creature*> others = LiveHostiles(player, LegYards + TrailLegMaxYards);
        std::erase_if(others, [&foes](Creature* creature) { return std::find(foes.begin(), foes.end(), creature) != foes.end(); });
        // The camp the bot stands in, if any: the way it came may lead out of it, not deeper in.
        Risk::CampAt here;
        bool const inCamp = Risk::InCampItCannotTake(player, rt, player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ(), &here);

        // Back the way it came: the newest point of its trail far enough away, when nothing hostile stands at it.
        while (!rt.trail.empty())
        {
            BotRuntime::TrailPoint point = rt.trail.back();
            float const distance = player->GetDistance2d(point.x, point.y);
            if (point.mapId != player->GetMapId() || distance > TrailLegMaxYards)
            {
                rt.trail.clear();
                break;
            }
            if (distance < TrailLegMinYards)
            {
                rt.trail.pop_back();
                continue;
            }
            rt.trail.pop_back(); // used, or no good
            // Not past other creatures on the way, and not into the water from dry land (a swimmer is slow).
            uint32 const atPoint = CountNear(others, point.x, point.y, 15.0f);
            uint32 const onWay = CountAlong(others, player, point.x, point.y);
            bool const intoWater = Escape::WaterAt(player, point.x, point.y, point.z) &&
                !Escape::WaterAt(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
            // Nor into the middle of a camp it cannot take (the legs below keep out of one; the way it came did not
            // look): the creatures of a camp wander, and a trail point free of them now is among them a moment later.
            // Pewpew, a level-60 hunter that had ridden along the edge of the Twilight camp of Silithus with its quest
            // partner a minute before, left a fight against four of its creatures "the way it came" and died inside
            // the camp nineteen seconds later, round 196.
            bool intoCamp = false;
            if (Risk::CampAt there; Risk::InCampItCannotTake(player, rt, point.mapId, point.x, point.y, point.z, &there))
                intoCamp = !inCamp || std::hypot(point.x - here.x, point.y - here.y) <
                    std::hypot(player->GetPositionX() - here.x, player->GetPositionY() - here.y);
            else if (!inCamp)
                intoCamp = Risk::EntersCamp(player, rt, point.mapId, player->GetPositionX(), player->GetPositionY(), point.x, point.y);
            if (atPoint || onWay || intoWater || intoCamp || !CanWalkTo(player, point.x, point.y, point.z, distance))
            {
                if (!escape.legs)
                    escape.legNote = Acore::StringFormat("the way it came was closed ({})", atPoint || onWay ?
                        Acore::StringFormat("{} creature{} on it", atPoint + onWay, atPoint + onWay == 1 ? "" : "s") :
                        (intoWater ? std::string("into the water") : intoCamp ? std::string("into a camp") : std::string("no path")));
                break;
            }
            if (!player->IsStandState())
                player->SetStandState(UNIT_STAND_STATE_STAND);
            escape.heading = std::atan2(point.y - player->GetPositionY(), point.x - player->GetPositionX());
            escape.legX = point.x;
            escape.legY = point.y;
            escape.nextLegAt = getMSTime() + 2 * LegMs;
            ++escape.legs;
            escape.onTrail = true;
            player->GetMotionMaster()->Clear();
            player->GetMotionMaster()->MovePoint(0, point.x, point.y, point.z, FORCED_MOVEMENT_RUN);
            return true;
        }

        float base = escape.heading;
        if (!escape.legs)
        {
            float cx = 0.0f;
            float cy = 0.0f;
            for (Creature const* foe : foes)
            {
                cx += foe->GetPositionX();
                cy += foe->GetPositionY();
            }
            if (!foes.empty())
            {
                cx /= float(foes.size());
                cy /= float(foes.size());
            }
            float const dx = player->GetPositionX() - cx;
            float const dy = player->GetPositionY() - cy;
            base = (foes.empty() || (std::fabs(dx) < 0.5f && std::fabs(dy) < 0.5f)) ? player->GetOrientation() + Pi : std::atan2(dy, dx);
        }

        static constexpr float offsets[] = { 0.0f, Pi / 4, -Pi / 4, Pi / 2, -Pi / 2, 3 * Pi / 4, -3 * Pi / 4 };
        float bestScore = std::numeric_limits<float>::max();
        float bestHeading = base;
        float bx = 0.0f;
        float by = 0.0f;
        float bz = 0.0f;
        bool found = false;
        // (In water too deep to stand in: it swims.)
        bool const swims = Escape::WaterAt(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ()) != 0 &&
            Escape::TooWetToRest(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
        uint32 pastOthers = 0;
        uint32 intoWater = 0;
        uint32 noPath = 0;
        uint32 takenPast = 0;
        for (float offset : offsets)
        {
            float const heading = base + offset;
            float tx = player->GetPositionX() + std::cos(heading) * LegYards;
            float ty = player->GetPositionY() + std::sin(heading) * LegYards;
            float tz = player->GetPositionZ();
            player->UpdateAllowedPositionZ(tx, ty, tz);
            // A swimmer's leg is a straight line through the water it is in, at its own depth or just above the floor
            // where that rises: the path finder has no ways under water (a level-39 warrior that a level-46 shark came
            // for: "of 7 directions 0 led past other creatures, 3 into water, 7 had no path; none left").
            bool swimLeg = false;
            if (swims)
            {
                float sz = player->GetPositionZ();
                float const floor = player->GetMap()->GetHeight(player->GetPhaseMask(), tx, ty, sz + 2.0f, true, 100.0f);
                if (floor > INVALID_HEIGHT && floor + 1.5f > sz)
                    sz = floor + 1.5f;
                if (Escape::WaterDepth(player, tx, ty, sz) > Escape::RestWaterYards)
                {
                    swimLeg = true;
                    tz = sz;
                }
            }

            float const mx = (player->GetPositionX() + tx) / 2.0f;
            float const my = (player->GetPositionY() + ty) / 2.0f;
            // Spawns (a camp whose mobs are dead for the moment) and, weighing more, what is alive there now; a leg
            // that leads past a living creature within its reach, or into the water, is taken only when nothing else is
            // left.
            uint32 const along = CountAlong(others, player, tx, ty);
            bool const wet = Escape::WaterAt(player, tx, ty, tz) != 0;
            pastOthers += along ? 1 : 0;
            intoWater += wet ? 1 : 0;
            // (Nor into the middle of a camp it cannot take: a hunter that kept away from three Kurzen at the east
            // side of their compound was led 127 yards west by its legs, each of them free of living creatures, and
            // stood in the compound's middle when the keep-away was over.)
            bool const intoCamp = Risk::InCampItCannotTake(player, rt, player->GetMapId(), tx, ty, tz);
            float const score = std::fabs(offset) / (Pi / 4) +
                3.0f * float(Risk::HostilesNear(player, mx, my, 20.0f) + Risk::HostilesNear(player, tx, ty, 25.0f)) +
                5.0f * float(CountNear(others, mx, my, 20.0f) + CountNear(others, tx, ty, 25.0f)) +
                100.0f * float(along) +
                (wet && !swims ? 50.0f : 0.0f) +
                (intoCamp ? 60.0f : 0.0f);
            if (score >= bestScore)
                continue;
            if (swimLeg ? !player->IsWithinLOS(tx, ty, tz + 1.0f) : !CanWalkTo(player, tx, ty, tz, LegYards))
            {
                ++noPath;
                continue;
            }

            takenPast = along;
            bestScore = score;
            bestHeading = heading;
            bx = tx;
            by = ty;
            bz = tz;
            found = true;
        }
        // What the leg rule made of the seven directions, once per run (the first leg decides most).
        if (!escape.legs)
        {
            escape.legNote += Acore::StringFormat("{}of 7 directions {} led past other creatures, {} into water, {} had no path{}",
                escape.legNote.empty() ? "" : "; ", pastOthers, intoWater, noPath,
                found ? Acore::StringFormat("; the one taken passes {}", takenPast) : std::string("; none left"));
            escape.firstPast = uint8(std::min<uint32>(found ? takenPast : 0, 255));
        }
        if (!found)
            return false;

        // A bot that sat down to rest gets up before it runs.
        if (!player->IsStandState())
            player->SetStandState(UNIT_STAND_STATE_STAND);
        escape.heading = bestHeading;
        escape.legX = bx;
        escape.legY = by;
        escape.nextLegAt = getMSTime() + LegMs;
        ++escape.legs;
        player->GetMotionMaster()->Clear();
        player->GetMotionMaster()->MovePoint(0, bx, by, bz, FORCED_MOVEMENT_RUN);
        return true;
    }

    // What a player presses before turning to run: the first rank of each spell; the bot casts the highest rank it
    // knows. Self: on itself (a nova, a shield, a totem); Foe: on the strongest foe on it.
    enum class HelpTarget : uint8 { Self, Foe };
    // Getaway: makes the run itself cheap (speed, the foes held or scared, combat dropped). Control: holds one foe.
    // Defense: fewer or weaker hits, also for a bot that stays.
    enum class HelpKind : uint8 { Getaway, Control, Defense };
    struct HelpSpell
    {
        uint8 classId;
        uint32 firstRank;
        HelpTarget target;
        HelpKind kind;
    };
    // At most eight per class (a bit each in BotRuntime::Escape::helpsCast).
    constexpr HelpSpell HelpSpells[] =
    {
        { CLASS_MAGE,    122,   HelpTarget::Self, HelpKind::Getaway }, // Frost Nova
        { CLASS_MAGE,    1953,  HelpTarget::Self, HelpKind::Getaway }, // Blink
        { CLASS_ROGUE,   1856,  HelpTarget::Self, HelpKind::Getaway }, // Vanish
        { CLASS_ROGUE,   2983,  HelpTarget::Self, HelpKind::Getaway }, // Sprint
        { CLASS_ROGUE,   5277,  HelpTarget::Self, HelpKind::Defense }, // Evasion
        { CLASS_ROGUE,   1776,  HelpTarget::Foe,  HelpKind::Control }, // Gouge
        { CLASS_ROGUE,   2094,  HelpTarget::Foe,  HelpKind::Control }, // Blind
        { CLASS_PRIEST,  8122,  HelpTarget::Self, HelpKind::Getaway }, // Psychic Scream
        { CLASS_PRIEST,  17,    HelpTarget::Self, HelpKind::Defense }, // Power Word: Shield
        { CLASS_DRUID,   16689, HelpTarget::Self, HelpKind::Control }, // Nature's Grasp
        { CLASS_DRUID,   22812, HelpTarget::Self, HelpKind::Defense }, // Barkskin
        // (After the two above: it cannot cast in the form. A level-23 druid ran from three gnolls on two legs and
        // was dead after eight seconds; in Travel Form it is faster than they are.)
        { CLASS_DRUID,   783,   HelpTarget::Self, HelpKind::Getaway }, // Travel Form
        { CLASS_SHAMAN,  5730,  HelpTarget::Self, HelpKind::Getaway }, // Stoneclaw Totem
        { CLASS_SHAMAN,  2484,  HelpTarget::Self, HelpKind::Getaway }, // Earthbind Totem
        { CLASS_SHAMAN,  8056,  HelpTarget::Foe,  HelpKind::Control }, // Frost Shock
        { CLASS_PALADIN, 642,   HelpTarget::Self, HelpKind::Getaway }, // Divine Shield
        { CLASS_PALADIN, 853,   HelpTarget::Foe,  HelpKind::Control }, // Hammer of Justice
        { CLASS_PALADIN, 498,   HelpTarget::Self, HelpKind::Defense }, // Divine Protection
        { CLASS_PALADIN, 633,   HelpTarget::Self, HelpKind::Defense }, // Lay on Hands
        { CLASS_WARLOCK, 5484,  HelpTarget::Self, HelpKind::Getaway }, // Howl of Terror
        { CLASS_WARLOCK, 6789,  HelpTarget::Foe,  HelpKind::Control }, // Death Coil
        { CLASS_MAGE,    11426, HelpTarget::Self, HelpKind::Defense }, // Ice Barrier
        { CLASS_MAGE,    1463,  HelpTarget::Self, HelpKind::Defense }, // Mana Shield
        // (Its target is a foe: cast on the warrior itself it never went out, in none of 58 runs of an evening.)
        { CLASS_WARRIOR, 5246,  HelpTarget::Foe,  HelpKind::Getaway }, // Intimidating Shout
        { CLASS_WARRIOR, 1715,  HelpTarget::Foe,  HelpKind::Control }, // Hamstring
        { CLASS_WARRIOR, 871,   HelpTarget::Self, HelpKind::Defense }, // Shield Wall
        { CLASS_HUNTER,  5384,  HelpTarget::Self, HelpKind::Getaway }, // Feign Death
        { CLASS_HUNTER,  781,   HelpTarget::Self, HelpKind::Getaway }, // Disengage
        { CLASS_HUNTER,  2974,  HelpTarget::Foe,  HelpKind::Control }, // Wing Clip
    };
    // A run lasts about this long with the foes at the bot's back; a getaway spell that is ready takes all but
    // GetawayShare of the run's cost away. What a run really costs a bot against that estimate it learns from its own
    // runs (BotRuntime::runFactor): hits in the back are not dodged or parried and daze the runner, which killed a
    // rogue within ten seconds of every run, while on the integration server 16 of 18 runs of all bots got away.
    // Measured on the integration server (125 runs of one evening): a run from one foe cost 19 % of the bot's
    // health on average, from two 22 %, from three 26 %, from four 37 %, and took 11 to 16 seconds; 2 of 33 runs
    // from one foe ended in a death, 9 of 72 from two, 11 of 32 from three. The foes trail behind one another and
    // give up 30 yards from where the fight began (CreatureLeashRadius), so the one at the bot's heels counts for
    // this many seconds and every further one for RunSecondsMore. (The first estimate, 15 seconds for every foe,
    // was three times that, and bots stayed in fights they could have left: "the run about 93%".)
    constexpr float RunSeconds = 8.0f;
    constexpr float RunSecondsMore = 2.5f;
    constexpr float GetawayShare = 0.4f;
    constexpr float RunFactorMin = 0.4f;
    constexpr float RunFactorMax = 4.0f;
    constexpr float RunLearnRate = 0.4f;
    // A death on the run: the factor grows at least this much.
    constexpr float RunDeathGrowth = 1.5f;
    // When neither staying nor running is survivable the bot runs only if the run costs clearly less.
    constexpr float RunIfBelowFightShare = 0.6f;
    // What is left of a fight's cost when the bot uses what protects it (Evasion, a shield, Barkskin ...).
    constexpr float DefenseShare = 0.75f;
    // The share of its health a foe takes off the bot per second: the bounds of what is learned (a caster's fights
    // cost it little health because its foes die on the way to it; at its back they do not).
    constexpr float MinHealthRate = 0.01f;
    constexpr float MaxHealthRate = 0.10f;
    // A bot that stays because running would cost more than it has does not think about running again for this long.
    constexpr uint32 StandMs = 12 * IN_MILLISECONDS;

    uint32 HighestKnownRank(Player const* player, uint32 firstRank)
    {
        for (uint32 spell = sSpellMgr->GetLastSpellInChain(firstRank); spell; spell = sSpellMgr->GetPrevSpellInChain(spell))
        {
            if (player->HasSpell(spell))
                return spell;
            if (spell == firstRank)
                break;
        }
        return player->HasSpell(firstRank) ? firstRank : 0;
    }

    // True if the bot has a spell of that kind that is not on cooldown.
    // Whether the bot can pay for a spell now: rage, energy or mana. A warrior seven seconds into a fight has not the
    // 25 rage of its Intimidating Shout (round 238).
    bool CanPay(Player const* player, SpellInfo const* info)
    {
        if (info->PowerType >= MAX_POWERS)
            return true;
        int32 const cost = info->CalcPowerCost(player, info->GetSchoolMask());
        return cost <= 0 || int32(player->GetPower(Powers(info->PowerType))) >= cost;
    }

    // Whether a spell that holds or scares the foes in an area around the bot reaches every one of them (a nova or a
    // fear holds what stands in its radius; a caster that bolts from 30 yards is not in it and bolts the runner).
    // Spells without such an area (a blink, a sprint, a shield) reach them all.
    bool ReachesAll(Player const* player, SpellInfo const* info, std::vector<Creature*> const& foes)
    {
        float radius = 0.0f;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& effect = info->Effects[i];
            if (effect.IsEffect() && effect.HasRadius() && (effect.TargetA.GetTarget() == TARGET_UNIT_SRC_AREA_ENEMY ||
                effect.TargetB.GetTarget() == TARGET_UNIT_SRC_AREA_ENEMY))
                radius = std::max(radius, effect.CalcRadius(const_cast<Player*>(player)));
        }
        if (radius <= 0.0f)
            return true;
        for (Creature const* foe : foes)
            if (player->GetExactDist(foe) > radius + foe->GetCombatReach())
                return false;
        return true;
    }

    bool HasReadyHelp(Player const* player, HelpKind kind)
    {
        for (HelpSpell const& help : HelpSpells)
            if (help.classId == player->getClass() && help.kind == kind)
                if (uint32 const spell = HighestKnownRank(player, help.firstRank); spell && !player->HasSpellCooldown(spell))
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell); info && CanPay(player, info))
                        return true;
        return false;
    }

    // Whether a getaway spell that is ready helps against these foes. One that is only speed does not against a foe as
    // fast: a druid in Travel Form runs 40 % faster, a Dust Stormer of Silithus 43 %; a level-60 druid left two of them
    // at 75 % health "the way it came" in Travel Form and died on the run seventeen seconds later, round 164.
    struct SpeedGetaway
    {
        uint32 firstRank;
        float speed; // its run speed with it, against an ordinary run
    };
    constexpr SpeedGetaway SpeedGetaways[] = { { 783, 1.4f }, { 2983, 1.7f } }; // Travel Form, Sprint
    bool GetawayHelps(Player const* player, std::vector<Creature*> const& foes)
    {
        float fastest = 0.0f;
        for (Creature const* foe : foes)
            fastest = std::max(fastest, foe->GetSpeedRate(MOVE_RUN));
        for (HelpSpell const& help : HelpSpells)
        {
            if (help.classId != player->getClass() || help.kind != HelpKind::Getaway)
                continue;
            uint32 const spell = HighestKnownRank(player, help.firstRank);
            if (!spell || player->HasSpellCooldown(spell))
                continue;
            // One the bot cannot pay for, or whose area does not reach the foes, takes nothing off the run: Warriorio
            // (60) left a Shadowmage and a Dark Adept at 80 % health seven seconds into the fight, its run reckoned at
            // 40 % of its cost for an Intimidating Shout it had not the rage for and that would not have reached the
            // Shadowmage bolting from range; it used Hamstring and died on the run after 35 s. Without that share the
            // run was one it would not live through, and it would have fought on (the fight before against the same
            // pair it had won at 52 %), round 238.
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
            if (!info || !CanPay(player, info) || !ReachesAll(player, info, foes))
                continue;
            float speed = 0.0f;
            for (SpeedGetaway const& getaway : SpeedGetaways)
                if (getaway.firstRank == help.firstRank)
                    speed = getaway.speed;
            if (!speed || speed > fastest)
                return true;
        }
        return false;
    }

    // What the run itself costs, as a share of the bot's full health: every foe at its back for RunSeconds, at the
    // rate the bot's own fights taught (BotRuntime::fightHealthRate), the more for a kind that costs it more than its
    // fights do on average (BotRuntime::foeCost: that druid's Dust Stormers cost it 2.3 times its average fight, and
    // its run from two of them was judged by the average).
    float RunNeed(Player const* player, BotRuntime const& rt, std::vector<Creature*> const& foes, float rate)
    {
        Pet const* pet = player->GetPet();
        uint8 const level = uint8(player->GetLevel());
        float const base = CostOf(rt, level);
        std::vector<float> parts;
        parts.reserve(foes.size());
        for (Creature const* foe : foes)
        {
            float part = rate * LevelFactor(player, foe->GetLevel());
            constexpr float MaxKindFactor = 4.0f;
            if (base > 0.0f)
                part *= std::clamp(EntryCost(rt, level, foe->GetEntry()) / base, 1.0f, MaxKindFactor);
            if (foe->isElite())
                part *= EliteMultiplier(player, foe);
            if (pet && pet->IsAlive() && foe->GetVictim() == pet)
                part *= PetTankShare;
            parts.push_back(part);
        }
        // A foe that runs as fast as the bot or faster stays at its back the whole run, not only its first seconds
        // (Grimtusk, a level-29 hunter, left two Needles Cougars at 66 % health, cats that run 14 % faster than it,
        // "used Disengage, Wing Clip", and died on the run 15 s later, round 239).
        constexpr float RunSecondsFast = 15.0f;
        float const ownSpeed = player->GetSpeedRate(MOVE_RUN);
        float fastPart = 0.0f;
        // And so does a caster that hits the runner as long as it is in its reach, or a creature that nets it
        // (WorldIndex::HoldsARunner: Mojowind, 50, ran from two Bloodsail Mages and a netting Raider of level 40 at 87 %
        // health and died on the run, round 259).
        for (std::size_t i = 0; i < foes.size(); ++i)
            if (!foes[i]->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED) &&
                (foes[i]->GetSpeedRate(MOVE_RUN) >= ownSpeed * 0.98f || sWorldIndex.HoldsARunner(foes[i]->GetEntry())))
                fastPart += parts[i] * RunSecondsFast;
        std::sort(parts.begin(), parts.end(), std::greater<float>());
        float need = 0.0f;
        for (std::size_t i = 0; i < parts.size(); ++i)
            need += parts[i] * (i ? RunSecondsMore : RunSeconds);
        return std::max(need, fastPart);
    }

    // What drops the fight at once; the foes stand where they stood, though.
    constexpr uint32 VanishSpell = 1856;
    constexpr uint32 EvasionSpell = 5277;
    constexpr uint32 FeignDeathSpell = 5384;

    // Casts what is ready (mana, cooldown and the global cooldown decide; a refused cast is tried again next tick).
    // defenseOnly: for a bot that stays in the fight.
    void CastHelp(Player* player, BotRuntime::Escape& escape, std::vector<Creature*> const& foes, bool defenseOnly = false)
    {
        // Nothing while the hunter lies in its Feign Death: the run calls this every tick of its first seconds, and the
        // Disengage of the next tick stood the hunter up again ("used Feign Death, Disengage", round 196, after the
        // break below had covered only the tick of the Feign Death). One that was resisted is gone (the run removes it).
        if (player->HasAura(FeignDeathSpell))
            return;
        Creature* strongest = nullptr;
        for (Creature* foe : foes)
            if (player->IsWithinMeleeRange(foe) && (!strongest || foe->GetLevel() > strongest->GetLevel()))
                strongest = foe;

        uint8 next = 1;
        for (HelpSpell const& help : HelpSpells)
        {
            if (help.classId != player->getClass())
                continue;
            uint8 const bit = next;
            next = uint8(next << 1);
            if ((escape.helpsCast & bit) || (defenseOnly && help.kind != HelpKind::Defense))
                continue;
            // Vanish or Feign Death went out: a Gouge or a Wing Clip after it puts the bot back into the fight (a
            // rogue used "Vanish, Gouge" and died on the run ten seconds later).
            if (escape.vanished && help.target == HelpTarget::Foe)
                continue;
            // Evasion dodges nothing that comes from behind: it is for a bot that stands (StepDefend), not for a run.
            if (!defenseOnly && help.firstRank == EvasionSpell)
                continue;
            // Not Divine Protection while Divine Shield or Lay on Hands is ready: its Forbearance would keep them from it.
            if (help.firstRank == 498 && KeepsForbearanceFree(player))
                continue;
            // Lay on Hands is for a paladin that is nearly dead (one cast it at 99 % health).
            constexpr uint32 LayOnHandsSpell = 633;
            constexpr float LayOnHandsBelow = 35.0f;
            if (help.firstRank == LayOnHandsSpell && player->GetHealthPct() > LayOnHandsBelow)
                continue;
            uint32 const spell = HighestKnownRank(player, help.firstRank);
            Unit* const target = help.target == HelpTarget::Self ? static_cast<Unit*>(player) : strongest;
            if (!spell || !target)
                continue;
            if (target != player)
                player->SetFacingToObject(target);
            SpellCastResult const result = player->CastSpell(target, spell, false);
            if (result == SPELL_CAST_OK)
            {
                escape.helpsCast |= bit;
                if (help.firstRank == VanishSpell || help.firstRank == FeignDeathSpell)
                    escape.vanished = true;
                if (help.firstRank == FeignDeathSpell)
                    escape.feignAt = std::max<uint32>(getMSTime(), 1);
                if (help.kind == HelpKind::Getaway)
                    escape.getawayCast = true;
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell))
                    escape.helps += (escape.helps.empty() ? "" : ", ") + std::string(info->SpellName[0]);
                // Nothing after a Feign Death: a Disengage leaps and stands the hunter up again (round 191).
                if (help.firstRank == FeignDeathSpell)
                    break;
            }
            // (Debug) Why a spell that would have helped did not go out, once per spell and run.
            else if (GetSettings().debug && !(escape.helpsNoted & bit))
            {
                escape.helpsNoted |= bit;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
                LOG_INFO(ActivityLog, "activity: {} could not cast {} on its run: cast result {} (target {}, power {} of {}, cooldown {})",
                    player->GetName(), info ? info->SpellName[0] : "?", uint32(result), target == player ? "itself" : target->GetName(),
                    player->GetPower(player->getPowerType()), player->GetMaxPower(player->getPowerType()),
                    player->HasSpellCooldown(spell) ? "yes" : "no");
            }
        }
    }

    // Held by a root or a net on its run (not stunned or feared: nothing of the bot's helps there): what frees it.
    // A racial (Every Man for Himself, Escape Artist), Hand of Freedom, Blink, a druid's shift into a form.
    bool BreakFree(Player* player, BotRuntime::Escape& escape)
    {
        if (!player->HasUnitState(UNIT_STATE_ROOT) || player->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING))
            return false;
        // (Divine Shield after Hand of Freedom: it ends the root too, and the run goes on under it. A paladin webbed by a
        // Nerubian Overseer used Hand of Freedom, was webbed again and died, round 158.)
        static constexpr uint32 Freeing[] = { 59752, 20589, 1044, 642, 1953, 783, 768 };
        for (uint32 first : Freeing)
        {
            uint32 const spell = HighestKnownRank(player, first);
            if (!spell || player->CastSpell(player, spell, false) != SPELL_CAST_OK)
                continue;
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell))
                escape.helps += (escape.helps.empty() ? "" : ", ") + std::string(info->SpellName[0]);
            return true;
        }
        return false;
    }

    // How long a stun, a root, a fear or a confusion still holds the bot when every one of them ends within a few
    // seconds (0: none of them is on it, or one lasts longer or for good); `name` the longest one. A level-60 warrior
    // ran from two Shadowmages at 56 % health, a Dark Adept's Snap Kick stunned it for two seconds, and the run ended
    // "held fast, fights on"; eight seconds later it ran again, was held again after one second and died, round 183.
    uint32 ShortHold(Player const* player, std::string* name)
    {
        constexpr int32 MaxHoldMs = 6 * IN_MILLISECONDS;
        if (!player->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING) ||
            player->IsMounted() || player->IsInFlight() || player->GetTransport() || player->GetVehicle() ||
            player->GetMap()->Instanceable())
            return 0;
        int32 longest = 0;
        for (AuraType type : { SPELL_AURA_MOD_STUN, SPELL_AURA_MOD_ROOT, SPELL_AURA_MOD_FEAR, SPELL_AURA_MOD_CONFUSE })
            for (AuraEffect const* effect : player->GetAuraEffectsByType(type))
            {
                int32 const left = effect->GetBase()->GetDuration();
                if (left < 0 || left > MaxHoldMs)
                    return 0;
                if (left > longest)
                {
                    longest = left;
                    if (name)
                        *name = effect->GetSpellInfo()->SpellName[0];
                }
            }
        return uint32(longest);
    }

    // Around a pack on the way instead of away from it: a point beside the pack, on the side the bot's way does
    // not lead to, far enough from every creature of it, with nothing else hostile on the way there and a path to
    // it. False when there is no such point on either side.
    bool StartDetour(Player* player, BotRuntime& rt, std::vector<Creature*> const& onWay, Creature const* nearest, float destX, float destY,
        float extraYards = 0.0f)
    {
        constexpr float DetourMargin = 8.0f;
        constexpr float DetourMaxYards = 110.0f;
        if (onWay.empty())
            return false;
        std::vector<Creature*> const around = LiveHostiles(player, DetourMaxYards + 20.0f, PackLevelsBelow);
        // Around the whole camp, not only around what stands on the way: whatever stands within CampLinkYards of a
        // creature of the pack belongs to it, and so on. (The way around three gnolls of a field of ten led past its
        // other seven: "went around 3 creatures on its way after 7 s" and a fight against four two seconds later,
        // twice in one live round, both deaths.)
        constexpr float CampLinkYards = 22.0f;
        constexpr std::size_t CampMaxCreatures = 24;
        std::vector<Creature*> pack = onWay;
        for (bool grown = true; grown && pack.size() < CampMaxCreatures;)
        {
            grown = false;
            for (Creature* creature : around)
            {
                if (creature->IsInCombat() || std::find(pack.begin(), pack.end(), creature) != pack.end())
                    continue;
                for (Creature const* member : pack)
                    if (creature->GetFaction() == member->GetFaction() && creature->GetExactDist2d(member) <= CampLinkYards)
                    {
                        pack.push_back(creature);
                        grown = true;
                        break;
                    }
            }
        }
        float cx = 0.0f;
        float cy = 0.0f;
        for (Creature const* creature : pack)
        {
            cx += creature->GetPositionX();
            cy += creature->GetPositionY();
        }
        cx /= float(pack.size());
        cy /= float(pack.size());
        float radius = 0.0f;
        for (Creature const* creature : pack)
        {
            radius = std::max(radius, std::hypot(creature->GetPositionX() - cx, creature->GetPositionY() - cy) + creature->GetAggroRange(player));
            // (One that walks about at random: as far as its walk takes it from its home.)
            float hx = 0.0f, hy = 0.0f;
            if (float const walk = WalkAbout(creature, hx, hy); walk > 0.0f)
                radius = std::max(radius, std::hypot(hx - cx, hy - cy) + walk + creature->GetAggroRange(player));
        }
        radius += DetourMargin + extraYards;

        float const px = player->GetPositionX();
        float const py = player->GetPositionY();
        float ux = destX - px;
        float uy = destY - py;
        float const length = std::hypot(ux, uy);
        if (length < 1.0f)
            return false;
        ux /= length;
        uy /= length;
        float const nx = -uy;
        float const ny = ux;
        // The points to walk to. From outside the camp's circle: where the straight lines from the bot touch it, a
        // little outside, the one nearer to the bot's heading first (the way to such a point stays clear of the
        // whole circle; the next look goes on around it). From inside the circle: beside the pack, on the side of
        // the way its middle is not on.
        std::pair<float, float> points[2];
        if (float const toCamp = std::hypot(cx - px, cy - py); toCamp > radius + 2.0f)
        {
            constexpr float OutsideRadians = 0.12f;
            constexpr float BeyondYards = 6.0f;
            float const base = std::atan2(cy - py, cx - px);
            float const alpha = std::asin(radius / toCamp) + OutsideRadians;
            float const reach = std::sqrt(toCamp * toCamp - radius * radius) + BeyondYards;
            for (std::size_t i = 0; i < 2; ++i)
            {
                float const angle = base + (i == 0 ? alpha : -alpha);
                points[i] = { px + std::cos(angle) * reach, py + std::sin(angle) * reach };
            }
            auto toward = [&](std::pair<float, float> const& point) { return (point.first - px) * ux + (point.second - py) * uy; };
            if (toward(points[1]) > toward(points[0]))
                std::swap(points[0], points[1]);
        }
        else
        {
            float const side = (cx - px) * nx + (cy - py) * ny > 0.0f ? -1.0f : 1.0f;
            points[0] = { cx + nx * side * radius, cy + ny * side * radius };
            points[1] = { cx - nx * side * radius, cy - ny * side * radius };
        }
        for (std::pair<float, float> const& point : points)
        {
            float wx = point.first;
            float wy = point.second;
            float wz = player->GetPositionZ();
            player->UpdateAllowedPositionZ(wx, wy, wz);
            float const distance = player->GetDistance2d(wx, wy);
            if (distance > DetourMaxYards || distance < 5.0f)
                continue;
            bool blocked = false;
            std::vector<std::pair<float, float>> const leg{ { px, py }, { wx, wy } };
            for (Creature const* creature : around)
                if (!creature->IsInCombat() && NearestOnWay(creature, leg) <= creature->GetAggroRange(player) + 2.0f)
                {
                    blocked = true;
                    break;
                }
            if (blocked || Escape::WaterAt(player, wx, wy, wz) || !CanWalkTo(player, wx, wy, wz, distance))
                continue;
            // (Nor to the road of a creature, or a pack, that walks a round and kills it: a level-60 shaman went around
            // Vile Tutors in the Eastern Plaguelands to a point 24 yd from the road of the Crimson Courier, which came
            // by five seconds later; it left the fight at full health and died on the run, round 189.)
            constexpr float DetourRoadYards = 35.0f;
            if (Risk::OnStrongRoad(player, rt, player->GetMapId(), wx, wy, wz, DetourRoadYards) ||
                Risk::OnStrongRoad(player, rt, player->GetMapId(), (px + wx) / 2.0f, (py + wy) / 2.0f, wz, DetourRoadYards))
                continue;

            BotRuntime::Escape& escape = rt.escape;
            escape = BotRuntime::Escape();
            escape.active = true;
            escape.aside = true;
            escape.pack = true;
            escape.detour = true;
            escape.startedAt = getMSTime();
            escape.mapId = player->GetMapId();
            escape.fromX = cx;
            escape.fromY = cy;
            escape.legX = wx;
            escape.legY = wy;
            escape.legZ = wz;
            escape.healthStart = uint8(player->GetHealthPct());
            escape.foes = uint8(std::min<std::size_t>(pack.size(), 255));
            escape.foeEntry = nearest->GetEntry();
            escape.foeLevel = nearest->GetLevel();
            player->GetMotionMaster()->Clear();
            player->GetMotionMaster()->MovePoint(0, wx, wy, wz, FORCED_MOVEMENT_RUN);
            return true;
        }
        return false;
    }

    void StopFighting(Player* player)
    {
        player->AttackStop();
        player->InterruptNonMeleeSpells(false);
        player->SetTarget();
        if (Pet* pet = player->GetPet())
        {
            pet->AttackStop();
            pet->InterruptNonMeleeSpells(false);
            pet->GetMotionMaster()->MoveFollow(player, PET_FOLLOW_DIST, pet->GetFollowAngle());
        }
    }

    // What the run really cost against what was expected: the bot's own factor for the next decision.
    void LearnFromRun(Player const* player, BotRuntime& rt, bool died)
    {
        BotRuntime::Escape const& escape = rt.escape;
        if (escape.aside || escape.runEstimate < 0.02f)
            return;
        float const lost = died ? float(escape.healthStart) / 100.0f :
            std::max(0.0f, (float(escape.healthStart) - player->GetHealthPct()) / 100.0f);
        float ratio = lost / escape.runEstimate;
        if (died)
            ratio = std::max(ratio, rt.runFactor * RunDeathGrowth);
        rt.runFactor = std::clamp(rt.runFactor + RunLearnRate * (ratio - rt.runFactor), RunFactorMin, RunFactorMax);
        Risk::SaveFightStats(player->GetGUID(), rt);
    }

    void End(Player* player, BotRuntime& rt, char const* outcome)
    {
        BotRuntime::Escape const escape = rt.escape;
        rt.escape = BotRuntime::Escape();
        if (Pet* pet = player->GetPet(); pet && escape.petWasDefensive)
            pet->SetReactState(REACT_DEFENSIVE);
        BotDriver::PauseAI(player, 0);

        CreatureTemplate const* foe = escape.foeEntry ? sObjectMgr->GetCreatureTemplate(escape.foeEntry) : nullptr;
        CreatureTemplate const* threatened = escape.threatEntry ? sObjectMgr->GetCreatureTemplate(escape.threatEntry) : nullptr;
        LOG_INFO(ActivityLog, "activity: {} left a fight against {} foe{}{} at {}% health{}: {} after {} s, health {}%{}", rt.name,
            uint32(escape.foes), escape.foes == 1 ? "" : "s",
            foe ? Acore::StringFormat(" ({} level {})", foe->Name, uint32(escape.foeLevel)) : "", uint32(escape.healthStart),
            (escape.onTrail ? std::string(" the way it came") : std::string()) +
                (escape.helps.empty() ? std::string() : " (used " + escape.helps + ")") +
                (escape.heldBy.empty() ? std::string() : " (waited out " + escape.heldBy + ")"), outcome,
            getMSTimeDiff(escape.startedAt, getMSTime()) / IN_MILLISECONDS, uint32(player->IsAlive() ? player->GetHealthPct() : 0.0f),
            (escape.byFight ? std::string(" [by how the fight went]") : std::string()) +
                (threatened ? Acore::StringFormat(" [{} (level {}) was near]", threatened->Name, uint32(escape.threatLevel)) : std::string()) +
                (escape.fetched ? Acore::StringFormat(" [{} of the foes were being fetched by a fleeing one]", uint32(escape.fetched)) : std::string()) +
                (escape.legNote.empty() ? std::string() : " [" + escape.legNote + "]"));
    }
}

float Escape::BudgetOf(Player const* player)
{
    return OwnBudget(player);
}

uint32 Escape::HostilesAround(Player* player, float range, bool evading)
{
    return uint32(LiveHostiles(player, range, PackLevelsBelow, evading).size());
}

Creature* Escape::StrongestAround(Player* player, float range, uint32* level, bool evading)
{
    Creature* strongest = nullptr;
    uint32 most = 0;
    for (Creature* creature : LiveHostiles(player, range, PackLevelsBelow, evading))
        if (uint32 const worth = ThreatLevelOf(creature); worth > most)
        {
            most = worth;
            strongest = creature;
        }
    if (level)
        *level = most;
    return strongest;
}

bool Escape::ThreatApproaching(Player* player)
{
    constexpr float WatchYards = 45.0f;
    for (Creature const* creature : LiveHostiles(player, WatchYards))
        if (!creature->IsInCombat() && creature->GetExactDist(player) <= std::max(15.0f, creature->GetAggroRange(player) + 8.0f))
            return true;
    return false;
}

namespace
{
    // ------------------------------------------------------------------------------------------------------------
    // Pulls: who comes with a target, and bringing one creature out of its group (FightEscape.h "Careful pulls")
    // ------------------------------------------------------------------------------------------------------------

    // A ranged opener is cast from at most this far, and has to reach at least PullMinRange.
    constexpr float PullMaxRange = 28.0f;
    constexpr float PullMinRange = 18.0f;
    // Without one: this far back from where the creature saw the bot ([quote left out]).
    constexpr float PullStepBackYards = 10.0f;
    constexpr float PullArriveYards = 2.5f;
    constexpr uint32 PullToPlaceMs = 12 * IN_MILLISECONDS;
    constexpr uint32 PullCastMs = 4 * IN_MILLISECONDS;
    constexpr uint32 PullWalkInMs = 10 * IN_MILLISECONDS;
    constexpr uint32 PullBackMs = 5 * IN_MILLISECONDS;
    constexpr uint32 PullWaitMs = 6 * IN_MILLISECONDS;
    constexpr uint32 PullStandStillMs = 2500;
    // mod-playerbots is held this long at every step of a pull (a step every 250 ms).
    constexpr uint32 PullPauseMs = 800;
    // A pull that did not work is not tried again on that creature for this long.
    constexpr uint32 PullFailedMs = 45 * IN_MILLISECONDS;
    // A creature of the kind that stands alone is looked for this often, among this many of the nearest.
    constexpr uint32 PullSwitchLookMs = 2 * IN_MILLISECONDS;
    constexpr float PullSwitchYards = 45.0f;
    constexpr std::size_t PullSwitchLooked = 5;

    enum PullPhase : uint8
    {
        PullToPlace = 1, // to the place it opens from
        PullCast = 2,    // the opener
        PullWalkIn = 3,  // into the creature's sight
        PullBack = 4,    // back to where the fight shall be
        PullWait = 5,    // the creature is on its way
        PullLure = 6     // it stood and cast: out of its sight, or out of its spells' reach, to make it come
    };
    // A creature that stood and cast is waited for this long after a step out of its sight (its cast goes on first).
    constexpr uint32 PullLureStandStillMs = 5 * IN_MILLISECONDS;
    constexpr uint32 PullLureMs = 8 * IN_MILLISECONDS;

    struct PullVerdict
    {
        std::vector<Creature*> foes;   // the target first, then who would join
        std::vector<float> weights;
        std::string joiners;
        uint32 sure = 1;               // the target and who comes for certain
        bool strong = false;           // a creature the bot cannot beat may come: no pull, whatever the sums say
        float need = 0.0f;
        float budget = 0.0f;
        float limit = 0.0f;
        uint32 together = 1;
        bool tooMany = false;
        bool noReserve = false;        // three or more would come, and one more would be too much
        bool fine = false;
    };

    // Kinds of creature that stood and cast when a bot pulled them to its side (the pull ended "the creature fights
    // from a distance"): no careful pull for them again. World thread only; forgotten at a restart.
    std::unordered_set<uint32>& StandingCasters()
    {
        static std::unordered_set<uint32> kinds;
        return kinds;
    }

    // Who comes with the target when the bot walks from where it stands to (toX, toY) and the fight is there. Its
    // call for help reaches the creatures of its own faction within the core's assistance radius (10 yards) that
    // would help it, wherever the fight is; every other creature only joins when the bot comes into its own aggro
    // radius (the core's: 20 yards, a yard per level of difference, times Rate.Creature.Aggro) on that way. A
    // creature that walks about and is that much farther off may come or may not. (Before, everything hostile
    // within 12 yards of the target plus its wander distance counted in full: of 29 pulls refused in half an hour
    // on the integration server 18 were refused for one such creature, often of another kind: a spider beside a
    // wolf.)
    PullVerdict JudgePull(Player* player, BotRuntime& rt, Creature* target, float toX, float toY, float const* fightAt = nullptr)
    {
        // Where the fight is: where the bot walks to, or, after a walk into the creature's sight, the few steps back.
        float const fightX = fightAt ? fightAt[0] : toX;
        float const fightY = fightAt ? fightAt[1] : toY;
        constexpr float AssistMargin = 2.0f;
        constexpr float AggroMargin = 3.0f;
        // (Half at first. Counted on the integration server over one night: of 565 pulls the bots began with one or
        // more creatures that "may" join or that a fleeing foe might fetch, those creatures came in 11 of 100 cases;
        // a creature counted as sure came in 55 of 100. The sure ones stay whole: a pull must be survivable when
        // they all come.)
        constexpr float MaybeShare = 0.25f;
        constexpr float MaxPullYards = 45.0f;

        PullVerdict verdict;
        std::vector<Creature*>& foes = verdict.foes;
        std::vector<float>& weights = verdict.weights;
        std::string& joiners = verdict.joiners;
        uint32& sure = verdict.sure;
        foes.push_back(target);
        weights.push_back(1.0f);
        float const assistYards = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
        for (Creature* creature : LiveHostiles(player, MaxPullYards + 25.0f, PackLevelsBelow))
        {
            if (creature == target || creature->IsInCombat())
                continue;
            // A creature that wanders comes that much closer during the fight (a warrior pulled one bird of three that
            // stood apart and had all three after twenty seconds, twice).
            // (One that has a minion of its own is counted with its whole walk, and in full when it may come at all: it
            // comes with its minion, two foes, not a quarter of one. Thunderhoof (26) pulled a Son of Cenarius with the
            // Treant Ally it summons in Stonetalon, "2 expected"; a second Son that wanders 15 yd, its home 21 yd from
            // the target, came with its treant, four, and the druid died on the run and its squad's shaman after it,
            // round 213.)
            bool const wanders = creature->GetDefaultMovementType() == RANDOM_MOTION_TYPE;
            bool const withMinion = wanders && sWorldIndex.MinionOf(creature->GetEntry()) != 0;
            float const wander = wanders ? (withMinion ? creature->GetWanderDistance() : std::min(creature->GetWanderDistance(), 10.0f)) : 0.0f;
            float weight = 0.0f;
            if (assistYards > 0.0f && Escape::WouldAssist(creature, target, player))
            {
                // (A creature that stands still a few yards outside the call's reach counts as "perhaps" as well: the
                // target takes a step, or it hears the call of one that came. In one live round the creatures that
                // came unexpectedly and stood still were 13 to 18 yards from the target.)
                constexpr float AssistNearYards = 4.0f;
                // Its call for help is not made once where it stands: the core repeats it every
                // CreatureFamilyAssistancePeriod (3 s) for as long as the creature fights, from wherever it is then,
                // so what it runs past on its way to the bot and what stands where the fight is comes with it. (The owner,
                // 2026-11-14: [quote left out]: a
                // murloc "that stands alone" was pulled from 27 yards, three more whose homes lay 15, 17 and 24 yards
                // from it, on its way to the paladin, were on the bot three to five seconds later.) The count was
                // taken around the creature's own place only.
                float const distance = std::min(creature->GetExactDist(target),
                    SegmentDistance(creature, target->GetPositionX(), target->GetPositionY(), fightX, fightY));
                if (distance <= assistYards + AssistMargin)
                    weight = 1.0f;
                else if (distance <= assistYards + AssistMargin + std::max(wander, AssistNearYards))
                    weight = MaybeShare;
            }
            if (weight < 1.0f)
            {
                float const reach = creature->GetAggroRange(player) + AggroMargin;
                float const distance = WayDistance(creature, player, toX, toY);
                if (distance <= reach)
                    weight = 1.0f;
                else if (distance <= reach + wander)
                    weight = std::max(weight, MaybeShare);
            }
            if (withMinion && weight > 0.0f)
                weight = 1.0f;
            // A creature the bot cannot beat (four levels above it by its level, health and damage: an elite, one far
            // above) is not weighed by shares: when it may come at all, the pull is not made. And one that is on its
            // way (a patrol, anything that moves) may come from StrongWalkerYards farther off: a level-37 paladin
            // "turned to a Young Murk Thresher that stands alone", "1 expected", and a level-46 Coral Shark on its
            // round, 16 yards from the thresher four seconds later, killed it in ten seconds; twice in one night.
            constexpr float StrongWalkerYards = 30.0f;
            constexpr float StrongNearYards = 8.0f;
            bool strong = false;
            if (weight < 1.0f && StrengthOf(creature) >= uint32(player->GetLevel()) + ThreatLevels)
            {
                bool const walks = creature->isMoving() ||
                    creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE;
                if (weight > 0.0f || WayDistance(creature, player, toX, toY) <= creature->GetAggroRange(player) + AggroMargin +
                    (walks ? StrongWalkerYards : StrongNearYards))
                {
                    weight = 1.0f;
                    strong = true;
                    verdict.strong = true;
                    // Seen on its way now, not only when it comes within the look-out's reach: the next target is chosen
                    // away from its round. (A shaman refused a pull for a Crimson Courier and its four bodyguards on their
                    // round, took a target 83 yd off six seconds later, met the five at 18 yd on its way and died, round 152.)
                    if (walks)
                        NoteRoamer(creature, player);
                }
            }
            if (weight <= 0.0f)
                continue;
            foes.push_back(creature);
            weights.push_back(weight);
            if (weight >= 1.0f)
                ++sure;
            if (foes.size() <= 6)
                joiners += Acore::StringFormat("{}{} {}{}", joiners.empty() ? "" : ", ", creature->GetName(), uint32(creature->GetLevel()),
                    strong ? " too strong for it" : (weight >= 1.0f ? "" : " perhaps"));
        }

        // ... and what the sure ones bring: every creature that joins a fight calls for help itself as it engages, so
        // what stands beside a sure joiner comes as well. A level-43 warrior pulled a Mosh'Ogg Warmonger on its round
        // in Stranglethorn ("2 expected": a Shaman beside its way), and the Mauler 9.5 yards from the Shaman was on it
        // three seconds later; it died where another guild character had died half an hour before (round 112).
        {
            std::size_t const counted = foes.size();
            std::vector<Creature*> const around = LiveHostiles(player, MaxPullYards + 25.0f + assistYards, PackLevelsBelow);
            for (std::size_t i = 1; i < counted && assistYards > 0.0f; ++i)
            {
                if (weights[i] < 1.0f)
                    continue;
                for (Creature* creature : around)
                {
                    if (creature == target || creature->IsInCombat() || creature->GetExactDist(foes[i]) > assistYards + AssistMargin ||
                        !Escape::WouldAssist(creature, foes[i], player))
                        continue;
                    auto const known = std::find(foes.begin(), foes.end(), creature);
                    if (known != foes.end())
                    {
                        float& counted_weight = weights[std::size_t(known - foes.begin())];
                        if (counted_weight < 1.0f)
                        {
                            counted_weight = 1.0f;
                            ++sure;
                        }
                        continue;
                    }
                    foes.push_back(creature);
                    weights.push_back(1.0f);
                    ++sure;
                    if (foes.size() <= 6)
                        joiners += Acore::StringFormat(", {} {} with {}", creature->GetName(), uint32(creature->GetLevel()), foes[i]->GetName());
                }
            }
        }

        // ... and their formations: a creature that engages makes the members of its formation engage too when its row
        // says so (the core's CreatureGroup::MemberEngagingTarget: the leader's row needs "members assist the leader", a
        // member's row "the leader assists its members", and then every member comes, neutral ones as well, wherever they
        // are). Grimtusk (37, hunter of the test guild) pulled Sleeby, a neutral ogre of Fozruk's formation on its round
        // through Arathi, "1 expected"; Fozruk, Znort and Feeboz were on it in the same second and it died, 2026-11-26.
        {
            std::size_t const counted = foes.size();
            for (std::size_t i = 0; i < counted; ++i)
            {
                if (weights[i] < 1.0f)
                    continue;
                CreatureGroup const* group = foes[i]->GetFormation();
                if (!group)
                    continue;
                auto const row = sFormationMgr->CreatureGroupMap.find(foes[i]->GetSpawnId());
                if (row == sFormationMgr->CreatureGroupMap.end())
                    continue;
                uint16 const groupAI = row->second.groupAI;
                bool const isLeader = group->GetLeader() == foes[i];
                uint16 const needs = isLeader ? uint16(GroupAIFlags::GROUP_AI_FLAG_MEMBER_ASSIST_LEADER) :
                    uint16(GroupAIFlags::GROUP_AI_FLAG_LEADER_ASSIST_MEMBER);
                if (!(groupAI & needs))
                    continue;
                for (auto const& [member, info] : group->GetMembers())
                {
                    if (!member || member == foes[i] || !member->IsAlive() || !member->IsInWorld() || member->IsInCombat() ||
                        !member->IsValidAttackTarget(player) || std::find(foes.begin(), foes.end(), member) != foes.end())
                        continue;
                    foes.push_back(member);
                    weights.push_back(1.0f);
                    ++sure;
                    if (foes.size() <= 6)
                        joiners += Acore::StringFormat("{}{} {} in {}'s formation", joiners.empty() ? "" : ", ", member->GetName(),
                            uint32(member->GetLevel()), foes[i]->GetName());
                }
            }
        }

        // ... and the minions they have: a creature's summoned voidwalker, succubus or imp stands beside it, does not
        // answer a call for help (it has an owner) and is not on the bot's way, but it fights as soon as its owner does.
        // Lilwar (47) pulled two Wastewander Shadow Mages in Tanaris, "2 expected", four came (two Voidwalker Minions),
        // it left at 57 % and died on the run; Shammpower the same at another quest of theirs. A minion counts as its
        // owner does.
        {
            std::size_t const counted = foes.size();
            std::vector<Creature*> const around = LiveHostiles(player, MaxPullYards + 25.0f + 20.0f, PackLevelsBelow);
            for (std::size_t i = 0; i < counted; ++i)
                for (Creature* creature : around)
                {
                    if (creature->IsInCombat() || creature->GetCharmerOrOwnerGUID() != foes[i]->GetGUID() ||
                        std::find(foes.begin(), foes.end(), creature) != foes.end())
                        continue;
                    foes.push_back(creature);
                    weights.push_back(weights[i]);
                    if (weights[i] >= 1.0f)
                        ++sure;
                    if (foes.size() <= 6)
                        joiners += Acore::StringFormat("{}{} {} with {}{}", joiners.empty() ? "" : ", ", creature->GetName(),
                            uint32(creature->GetLevel()), foes[i]->GetName(), weights[i] >= 1.0f ? "" : " perhaps");
                }
        }

        // A foe whose script calls for help in the fight (WorldIndex::CallForHelpYards) brings every creature of its
        // side within that call, from where the fight stands or where the caller stood: a Deadwood Den Watcher called
        // at half its health, and a level-55 warrior's pull of "1 expected" had four on it 15 s in (2026-11-27).
        for (std::size_t i = 0, called = foes.size(); i < called; ++i)
        {
            float const callYards = weights[i] >= 1.0f ? sWorldIndex.CallForHelpYards(foes[i]->GetEntry()) : 0.0f;
            if (callYards <= 0.0f)
                continue;
            for (Creature* creature : LiveHostiles(player, MaxPullYards + callYards + 10.0f, PackLevelsBelow))
            {
                float const from = std::min(creature->GetExactDist2d(fightX, fightY), creature->GetExactDist2d(foes[i]));
                if (creature->IsInCombat() || !Escape::WouldAssist(creature, foes[i], player) ||
                    from > callYards + creature->GetObjectSize() + foes[i]->GetObjectSize())
                    continue;
                auto const known = std::find(foes.begin(), foes.end(), creature);
                if (known != foes.end())
                {
                    weights[std::size_t(known - foes.begin())] = 1.0f;
                    continue;
                }
                foes.push_back(creature);
                weights.push_back(1.0f);
                ++sure;
                if (foes.size() <= 6)
                    joiners += Acore::StringFormat("{}{} {} ({} calls for help)", joiners.empty() ? "" : ", ", creature->GetName(),
                        uint32(creature->GetLevel()), foes[i]->GetName());
            }
        }

        // A foe that runs for help when it is nearly dead (its script says so: troggs, kobolds, murlocs, gnolls) comes
        // back with the nearest creature that would help it within the core's 30 yards. The count left that out: in
        // the first live window with it 3 of 21 pulls brought more foes than expected, and two of them were a level-12
        // warrior's deaths among troggs ("1 expected, 1 more possible", three came) and kobolds ("2 expected", four).
        {
            constexpr uint32 MaxFetched = 3;
            float const fetchYards = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS);
            float const share = Escape::FetchedShare(player);
            std::size_t const counted = foes.size();
            uint32 fetched = 0;
            for (std::size_t i = 0; i < counted && fetched < MaxFetched && fetchYards > 0.0f; ++i)
            {
                if (weights[i] < 1.0f || !sWorldIndex.FleesForHelp(foes[i]->GetEntry()))
                    continue;
                // One that runs early in every fight (a timer of its script, not its health) surely fetches: a Mossflayer
                // Scout runs 4 s into the fight. A level-60 warrior's pull of "1 expected, 1 more possible" in Zul'Mashar
                // brought three, and its next pull there eleven seconds later brought four; it died, round 229.
                bool const early = sWorldIndex.FleesForHelpEarly(foes[i]->GetEntry());
                float const fetchShare = early ? 1.0f : share;
                // It runs from where the fight is, not from where it stands now: a fight the bot has drawn to its
                // own side is out of the camp's reach (the count was taken around the creature's place, so a pull to
                // the bot's side showed no gain among gnolls, kobolds and murlocs and was not done).
                // The core looks from the runner itself and counts both creatures' sizes (IsWithinDistInMap): a helper
                // 31 to 33 yd away is in its reach. And a caster runs from where it stood casting, not from the fight's
                // middle: so the nearer of the two counts. A level-60 warrior's pull of a pair of Shadowmages ("2
                // expected") brought a third Shadowmage from 31 yd and a Crypt Slayer beside it 18 s in, and it died,
                // 2026-11-26. A helper that walks about counts with its walk: a Wastewander Shadow Mage that stood 36 yd
                // from the fight and walks 5 yd came with its voidwalker 14 s into a level-45 warrior's pull of "2
                // expected", and the warrior died to the four (Gorruk, the test guild, 2026-11-26).
                Creature* helper = nullptr;
                for (Creature* creature : LiveHostiles(player, MaxPullYards + fetchYards + 15.0f, PackLevelsBelow))
                {
                    float const from = std::min(creature->GetExactDist2d(fightX, fightY), creature->GetExactDist2d(foes[i]));
                    if (creature->IsInCombat() || !Escape::WouldAssist(creature, foes[i], player) ||
                        from > fetchYards + creature->GetObjectSize() + foes[i]->GetObjectSize() +
                            std::min(creature->GetWanderDistance(), 10.0f))
                        continue;
                    auto const known = std::find(foes.begin(), foes.end(), creature);
                    if (known != foes.end() && weights[std::size_t(known - foes.begin())] >= fetchShare)
                        continue; // counted that much already
                    if (!helper || creature->GetExactDist2d(fightX, fightY) < helper->GetExactDist2d(fightX, fightY))
                        helper = creature;
                }
                if (!helper)
                    continue;
                ++fetched;
                auto const known = std::find(foes.begin(), foes.end(), helper);
                if (known != foes.end())
                {
                    weights[std::size_t(known - foes.begin())] = fetchShare;
                    continue;
                }
                foes.push_back(helper);
                weights.push_back(fetchShare);
                if (foes.size() <= 6)
                    joiners += Acore::StringFormat("{}{} {} {}", joiners.empty() ? "" : ", ", helper->GetName(),
                        uint32(helper->GetLevel()), early ? Acore::StringFormat("({} runs for help early in a fight)", foes[i]->GetName()) :
                            std::string("if one runs for help"));
                // ... its minions with it (a voidwalker answers no call for help, it follows its owner).
                for (Creature* creature : LiveHostiles(player, MaxPullYards + fetchYards + 25.0f, PackLevelsBelow))
                    if (!creature->IsInCombat() && creature->GetCharmerOrOwnerGUID() == helper->GetGUID() &&
                        std::find(foes.begin(), foes.end(), creature) == foes.end())
                    {
                        foes.push_back(creature);
                        weights.push_back(fetchShare);
                        if (foes.size() <= 6)
                            joiners += Acore::StringFormat(", {} {} with it", creature->GetName(), uint32(creature->GetLevel()));
                    }
                // ... and what stands with the helper comes with it (its own call for help): a level-17 warrior's pulls
                // of "1 expected, 1 more possible" among gnolls brought four, twice in ten minutes, the second its death.
                for (Creature* creature : LiveHostiles(player, MaxPullYards + fetchYards + 15.0f, PackLevelsBelow))
                {
                    if (fetched >= MaxFetched)
                        break;
                    if (creature == helper || creature->IsInCombat() || !Escape::WouldAssist(creature, helper, player) ||
                        creature->GetExactDist(helper) > assistYards + AssistMargin ||
                        std::find(foes.begin(), foes.end(), creature) != foes.end())
                        continue;
                    ++fetched;
                    foes.push_back(creature);
                    weights.push_back(fetchShare);
                    if (foes.size() <= 6)
                        joiners += Acore::StringFormat(", {} {} with it", creature->GetName(), uint32(creature->GetLevel()));
                }
            }
        }

        // (What a sure foe's death brings counts in FightNeed; the line names it.)
        for (std::size_t i = 0; i < foes.size() && i < weights.size(); ++i)
            if (weights[i] >= 1.0f)
                if (auto const [summon, count] = sWorldIndex.DeathSummonOf(foes[i]->GetEntry()); summon)
                    if (CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(summon))
                    {
                        joiners += Acore::StringFormat("{}{}{} {} at the death of {}", joiners.empty() ? "" : ", ",
                            count > 1 ? Acore::StringFormat("{} x ", uint32(count)) : std::string(), info->Name, uint32(info->maxlevel),
                            foes[i]->GetName());
                        break;
                    }
        for (std::size_t i = 0; i < foes.size() && i < weights.size(); ++i)
            if (weights[i] >= 1.0f && !foes[i]->IsInCombat())
                if (auto const [summon, count] = sWorldIndex.CombatSummonOf(foes[i]->GetEntry()); summon)
                    if (CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(summon))
                    {
                        joiners += Acore::StringFormat("{}{}{} {} that {} summons in the fight", joiners.empty() ? "" : ", ",
                            count > 1 ? Acore::StringFormat("{} x ", uint32(count)) : std::string(), info->Name, uint32(info->maxlevel),
                            foes[i]->GetName());
                        break;
                    }
        verdict.need = FightNeed(player, foes, CostOf(rt, player->GetLevel()), &weights, &rt);
        verdict.limit = sBotSettings.GetFloat(player, Setting::RiskMaxFightCostPercent);
        verdict.budget = PullBudget(player);
        verdict.together = sSquadService.CountTogether(player, SquadPartnerYards);
        uint32 serious = 0;
        for (std::size_t i = 0; i < foes.size(); ++i)
            if (weights[i] >= 1.0f && foes[i]->GetLevel() + 1 >= player->GetLevel())
                ++serious;
        uint32 const maxFoes = uint32(sBotSettings.Get(player, Setting::EscapeMaxFoes)) * verdict.together;
        verdict.tooMany = sure >= 2 && maxFoes && serious >= maxFoes;
        verdict.fine = !verdict.tooMany && !verdict.strong && verdict.need <= verdict.budget * verdict.limit;
        // Three or more for certain: only when the bot could take one more of the target's kind as well. Counted on
        // the integration server over six and a half hours: of 25 fights a bot began with three foes expected it
        // left 6 and died in 1 (28 of 100; with two expected 6 of 100, with one 3 of 100), and of the nine own pulls
        // that had four or more on the bot it died in five.
        constexpr uint32 ReserveFromFoes = 3;
        if (verdict.fine && sure >= ReserveFromFoes && verdict.together <= 1)
        {
            std::vector<Creature*> more = foes;
            std::vector<float> moreWeights = weights;
            more.push_back(target);
            moreWeights.push_back(1.0f);
            float const reserve = FightNeed(player, more, CostOf(rt, player->GetLevel()), &moreWeights, &rt);
            if (reserve > verdict.budget * verdict.limit)
            {
                verdict.need = reserve;
                verdict.noReserve = true;
                verdict.fine = false;
            }
        }
        return verdict;
    }

    // How a creature is brought to the bot.
    struct PullPlan
    {
        bool ranged = false;       // a ranged spell or weapon opens; else the bot walks into the creature's sight
        uint32 spell = 0;
        float fromYards = 0.0f;    // how far from the creature the bot stands when it pulls
        float pullX = 0.0f, pullY = 0.0f, pullZ = 0.0f;
        float fightX = 0.0f, fightY = 0.0f, fightZ = 0.0f;
        bool side = false;         // pulled from another side than the bot came from: away from the creature's camp
        std::size_t spared = 0;    // how many fewer creatures count than at the creature's own place
        std::string there;         // who would join there
        std::string note;          // why an opener the bot has was not used
    };

    // The core's own rule for stances and forms (a bear casts no Moonfire, Taunt needs the defensive stance).
    bool StanceAllows(Player const* player, SpellInfo const* info)
    {
        return info->CheckShapeshift(player->GetShapeshiftForm()) == SPELL_CAST_OK;
    }

    // What the bot can open a fight with from a distance: the bow, gun, crossbow or thrown weapon in its hands, else
    // a spell of its class. 0: nothing (the pull is then a walk into the creature's sight).
    uint32 PullOpener(Player* player, Creature* target, float& range)
    {
        auto usable = [&](uint32 spell)
        {
            SpellInfo const* info = spell ? sSpellMgr->GetSpellInfo(spell) : nullptr;
            if (!info || !player->HasSpell(spell) || player->HasSpellCooldown(spell) || !StanceAllows(player, info))
                return false;
            if (info->PowerType == POWER_MANA || info->PowerType == POWER_RAGE || info->PowerType == POWER_ENERGY ||
                info->PowerType == POWER_RUNIC_POWER)
                if (info->CalcPowerCost(player, info->GetSchoolMask()) > int32(player->GetPower(Powers(info->PowerType))))
                    return false;
            float const max = player->GetSpellMaxRangeForTarget(target, info);
            if (max < PullMinRange)
                return false;
            range = max;
            return true;
        };

        if (Item const* weapon = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
            weapon && !weapon->IsBroken() && weapon->GetTemplate()->Class == ITEM_CLASS_WEAPON)
        {
            char const* name = nullptr;
            bool loaded = true;
            switch (weapon->GetTemplate()->SubClass)
            {
                case ITEM_SUBCLASS_WEAPON_THROWN:   name = "throw"; break;
                case ITEM_SUBCLASS_WEAPON_BOW:      name = "shoot bow"; loaded = Ammo::Count(player) > 0; break;
                case ITEM_SUBCLASS_WEAPON_GUN:      name = "shoot gun"; loaded = Ammo::Count(player) > 0; break;
                case ITEM_SUBCLASS_WEAPON_CROSSBOW: name = "shoot crossbow"; loaded = Ammo::Count(player) > 0; break;
                default: break;
            }
            if (name && loaded)
                if (uint32 const spell = BotDriver::SpellIdByName(player, name); usable(spell))
                    return spell;
        }

        // First ranks; the highest rank the bot knows is cast.
        // (Exorcism first: the owner, 2026-11-14: [quote left out].)
        static constexpr uint32 Paladin[] = { 879 /*Exorcism*/, 62124 /*Hand of Reckoning*/, 31935 /*Avenger's Shield*/ };
        static constexpr uint32 Shaman[] = { 8042 /*Earth Shock*/, 403 /*Lightning Bolt*/ };
        static constexpr uint32 Druid[] = { 16857 /*Faerie Fire (Feral)*/, 8921 /*Moonfire*/, 5176 /*Wrath*/ };
        static constexpr uint32 DeathKnight[] = { 49576 /*Death Grip*/, 45477 /*Icy Touch*/, 47541 /*Death Coil*/ };
        static constexpr uint32 Warrior[] = { 57755 /*Heroic Throw*/, 355 /*Taunt*/ };
        auto first = [&](uint32 const* list, std::size_t count) -> uint32
        {
            for (std::size_t i = 0; i < count; ++i)
                if (uint32 const spell = HighestKnownRank(player, list[i]); usable(spell))
                    return spell;
            return 0;
        };
        switch (player->getClass())
        {
            case CLASS_PALADIN:      return first(Paladin, std::size(Paladin));
            case CLASS_SHAMAN:       return first(Shaman, std::size(Shaman));
            case CLASS_DRUID:        return first(Druid, std::size(Druid));
            case CLASS_DEATH_KNIGHT: return first(DeathKnight, std::size(DeathKnight));
            case CLASS_WARRIOR:      return first(Warrior, std::size(Warrior));
            default:                 return 0;
        }
    }

    // Whether a spell cast by the bot from (x, y, z) has its line of sight to the target, measured as the core measures
    // it for a player caster (Spell::CheckCast, WorldObject::IsWithinLOSInMap): from the caster's eyes to the point of
    // the target's hit sphere towards them, doodads (M2) not counted. The target's own IsWithinLOS(x, y, z + 2) had
    // added its height to the point as well, so a place behind a rock or a low wall passed: 50 of the 75 openers that
    // did not go out in two days ended on "line of sight", 38 of them a paladin's Exorcism (2026-11-26/27).
    bool SightFrom(Player const* player, Unit const* target, float x, float y, float z)
    {
        float const eye = z + player->GetCollisionHeight();
        Position const hit = target->GetHitSpherePointFor({ x, y, eye });
        return target->GetMap()->isInLineOfSight(x, y, eye, hit.GetPositionX(), hit.GetPositionY(), hit.GetPositionZ(),
            target->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::M2);
    }

    // Where the bot pulls from and where the fight shall be. False: no careful pull (the bot is in the creature's
    // sight already, or there is no way to the place).
    // `side`: the direction from the creature in which the bot shall stand (else: where it comes from).
    bool PlanPull(Player* player, Creature* target, PullPlan& plan, float const* side = nullptr)
    {
        plan = PullPlan();
        float const dist = player->GetExactDist2d(target);
        float const aggro = target->GetAggroRange(player);
        float const angle = side ? *side : target->GetAngle(player); // from the creature towards the bot
        auto pointAt = [&](float yards, float& x, float& y, float& z)
        {
            x = target->GetPositionX() + std::cos(angle) * yards;
            y = target->GetPositionY() + std::sin(angle) * yards;
            z = player->GetPositionZ();
            player->UpdateAllowedPositionZ(x, y, z);
        };

        float range = 0.0f;
        if (uint32 const spell = PullOpener(player, target, range))
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
            // (For a bow, gun or thrown weapon the core adds the melee range of the two to the spell's least distance.)
            bool const weaponRange = info->RangeEntry && (info->RangeEntry->Flags & SPELL_RANGE_RANGED);
            float const least = player->GetSpellMinRangeForTarget(target, info) + (weaponRange ? player->GetMeleeRange(target) : 0.0f) + 3.0f;
            // (As far off as the opener reaches, two yards short: the creature has the longest way to run, away
            // from the others.)
            // (An opener with a cast time some yards further inside its range: a creature that walks about walked out
            // of it while a Lightning Bolt was cast from 28 yd, and the bolt failed at its end without a fight; 15 of 97
            // shamans' Lightning Bolt pulls in two days became a walk into the pack's sight that way.)
            bool const castTime = info->CalcCastTime(player) > 0 && !weaponRange;
            float const stand = std::max(std::max(std::min(range - (castTime ? 7.0f : 2.0f), PullMaxRange), least),
                castTime ? std::min(aggro + 3.0f, range - 2.0f) : 0.0f);
            // (Not from inside the creature's own sight: it would be on the bot before the cast is out.)
            if (stand <= range - 1.0f && dist >= std::max(least, aggro + 3.0f))
            {
                float x = player->GetPositionX(), y = player->GetPositionY(), z = player->GetPositionZ();
                bool reachable = true;
                if (dist > stand || side)
                {
                    pointAt(stand, x, y, z);
                    // (A few yards are walked whatever the path's length is against them. To another side: a whole
                    // way there that does not lead through the creature's own sight.)
                    reachable = (!side || SegmentDistance(target, player->GetPositionX(), player->GetPositionY(), x, y) >= aggro + 3.0f) &&
                        CanWalkTo(player, x, y, z, std::max(side ? player->GetExactDist2d(x, y) : dist - stand, 8.0f));
                }
                if (reachable && SightFrom(player, target, x, y, z))
                {
                    plan.ranged = true;
                    plan.side = side != nullptr;
                    plan.spell = spell;
                    plan.fromYards = side ? stand : std::min(dist, stand);
                    plan.pullX = plan.fightX = x;
                    plan.pullY = plan.fightY = y;
                    plan.pullZ = plan.fightZ = z;
                    return true;
                }
                plan.note = Acore::StringFormat("{}: {} {:.0f} yd from it", info->SpellName[0],
                    reachable ? "no line of sight" : "no way to a place", stand);
            }
            else
                plan.note = Acore::StringFormat("{}: the bot stands too near ({:.0f} yd)", info->SpellName[0], dist);
        }

        // Into its sight and a few steps back: only a creature that attacks on sight comes that way. (From the side
        // the bot stands on only.)
        if (side || !target->IsHostileTo(player) || aggro < 3.0f || dist < aggro + 3.0f)
            return false;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        pointAt(std::max(aggro - 1.5f, 2.0f), x, y, z);
        if (!CanWalkTo(player, x, y, z, std::max(dist - std::max(aggro - 1.5f, 2.0f), 8.0f)))
            return false;
        plan.fromYards = aggro;
        plan.pullX = x;
        plan.pullY = y;
        plan.pullZ = z;
        // Back: a few steps along the line the bot came by (a druid ran the 36 yards back to where it had started,
        // with the spider at its back). When the bot stands nearer than that already: where it stands, or the few
        // steps behind it when nothing else stands there.
        float bx = 0.0f, by = 0.0f, bz = 0.0f;
        pointAt(aggro + PullStepBackYards, bx, by, bz);
        plan.fightX = player->GetPositionX();
        plan.fightY = player->GetPositionY();
        plan.fightZ = player->GetPositionZ();
        if (dist >= aggro + PullStepBackYards)
        {
            plan.fightX = bx;
            plan.fightY = by;
            plan.fightZ = bz;
        }
        else if (dist < aggro + PullStepBackYards - 2.0f)
        {
            std::vector<Creature*> others = LiveHostiles(player, 60.0f, PackLevelsBelow);
            std::erase_if(others, [target](Creature* creature) { return creature == target || creature->IsInCombat(); });
            if (!CountAlong(others, player, bx, by) && CanWalkTo(player, bx, by, bz, aggro + PullStepBackYards - dist))
            {
                plan.fightX = bx;
                plan.fightY = by;
                plan.fightZ = bz;
            }
        }
        return true;
    }

    // The sides to pull a creature from so that its run to the bot leads away from the others of its camp (the ones
    // that would help it, within 30 yards of it): straight away from where they stand, and a little to either side of
    // that. None when it stands alone, or when the bot stands on that side already.
    std::vector<float> AwaySides(Player* player, Creature* target)
    {
        constexpr float CampYards = 30.0f;
        constexpr float SearchYards = 75.0f;
        float sumX = 0.0f, sumY = 0.0f;
        uint32 count = 0;
        for (Creature* creature : LiveHostiles(player, SearchYards, PackLevelsBelow))
            if (creature != target && !creature->IsInCombat() && creature->GetExactDist2d(target) <= CampYards &&
                Escape::WouldAssist(creature, target, player))
            {
                sumX += creature->GetPositionX();
                sumY += creature->GetPositionY();
                ++count;
            }
        std::vector<float> sides;
        if (!count)
            return sides;
        float const dx = target->GetPositionX() - sumX / float(count);
        float const dy = target->GetPositionY() - sumY / float(count);
        if (dx * dx + dy * dy < 1.0f)
            return sides; // in the middle of them: no side is away
        float const away = std::atan2(dy, dx);
        float const now = Position::NormalizeOrientation(target->GetAngle(player));
        for (float const turn : { 0.0f, 0.8f, -0.8f })
        {
            float const side = Position::NormalizeOrientation(away + turn);
            float diff = std::fabs(side - now);
            if (diff > Pi)
                diff = 2.0f * Pi - diff;
            if (diff > 0.3f)
                sides.push_back(side);
        }
        return sides;
    }

    bool HasPetOut(Player const* player)
    {
        if (Pet const* pet = player->GetPet(); pet && pet->IsAlive())
            return true;
        Guardian const* guardian = player->GetGuardianPet();
        return guardian && guardian->IsAlive();
    }

    // The verdict for a target, with the best way to fight it the bot has. In melee the fight is at the target; a
    // caster without a pet stands at its distance and the creature comes to it (a pet runs up to the target and
    // brings what stands there). `planned`: the verdict is for a careful pull, by `plan`.
    PullVerdict BestVerdict(Player* player, BotRuntime& rt, Creature* target, bool careful, PullPlan& plan, bool& planned)
    {
        planned = false;
        bool const melee = BotDriver::FightsInMelee(player);
        float toX = target->GetPositionX();
        float toY = target->GetPositionY();
        if (careful && !melee && !HasPetOut(player))
        {
            float const stand = std::clamp(BotDriver::StandRange(player), 15.0f, 35.0f) - 2.0f;
            if (player->GetExactDist2d(target) <= stand)
            {
                toX = player->GetPositionX();
                toY = player->GetPositionY();
            }
            else
            {
                float const angle = target->GetAngle(player);
                toX += std::cos(angle) * stand;
                toY += std::sin(angle) * stand;
            }
        }
        PullVerdict verdict = JudgePull(player, rt, target, toX, toY);
        if (!careful || !melee || (verdict.fine && verdict.foes.size() < 2))
            return verdict;
        if (target->GetGUID() == rt.pullFailedTarget && getMSTime() < rt.pullFailedUntil)
            return verdict;
        // A creature that casts does not come to the bot: the fight is at its place (a paladin opened on a Skeletal
        // Mage from 20 yards, it stood and cast, the paladin went to it and a Skeletal Warrior joined).
        if (target->GetCreatureTemplate()->unit_class == 8 /*the casters of creature_template.unit_class*/ ||
            StandingCasters().count(target->GetEntry()))
            return verdict;
        // In melee, with company at the target's place: what a pull to the bot's side would bring.
        if (!PlanPull(player, target, plan))
            return verdict;
        float const fightAt[2] = { plan.fightX, plan.fightY };
        PullVerdict here = JudgePull(player, rt, target, plan.pullX, plan.pullY, fightAt);
        // From the side away from the others (the owner, 2026-11-14: [quote left out]): when the run
        // to where the bot stands would bring company, the places on the far side of the creature are looked at, and
        // the best one is taken when the bot has a way there that passes nobody.
        if (plan.ranged && (!here.fine || here.foes.size() > 1))
            for (float const side : AwaySides(player, target))
            {
                PullPlan other;
                if (!PlanPull(player, target, other, &side))
                    continue;
                float const at[2] = { other.fightX, other.fightY };
                PullVerdict there = JudgePull(player, rt, target, other.pullX, other.pullY, at);
                if (there.fine && (!here.fine || there.need < here.need - 0.01f))
                {
                    here = std::move(there);
                    plan = other;
                }
            }
        if (!here.fine || (here.foes.size() >= verdict.foes.size() && here.need >= verdict.need - 0.01f))
            return verdict;
        plan.spared = verdict.foes.size() > here.foes.size() ? verdict.foes.size() - here.foes.size() : 0;
        plan.there = verdict.joiners;
        planned = true;
        return here;
    }

    // A creature of the target's kind near the bot that would come alone (the owner: [quote left out]).
    Creature* SingleOfKind(Player* player, BotRuntime& rt, Creature* target, PullVerdict const& current)
    {
        std::list<Creature*> found;
        player->GetCreatureListWithEntryInGrid(found, target->GetEntry(), PullSwitchYards);
        std::vector<Creature*> kind;
        for (Creature* creature : found)
            if (creature != target && creature->IsAlive() && !creature->IsInCombat() && !creature->IsInEvadeMode() &&
                player->IsValidAttackTarget(creature) && player->IsWithinLOSInMap(creature) &&
                !rt.IsInAvoidedArea(player->GetMapId(), creature->GetPositionX(), creature->GetPositionY(), player->GetLevel()))
                kind.push_back(creature);
        std::sort(kind.begin(), kind.end(), [player](Creature const* a, Creature const* b)
        {
            return player->GetExactDist2d(a) < player->GetExactDist2d(b);
        });
        if (kind.size() > PullSwitchLooked)
            kind.resize(PullSwitchLooked);
        for (Creature* creature : kind)
        {
            PullPlan plan;
            bool planned = false;
            PullVerdict const verdict = BestVerdict(player, rt, creature, true, plan, planned);
            if (verdict.fine && verdict.sure == 1 && verdict.foes.size() < current.foes.size())
                return creature;
        }
        return nullptr;
    }

    void StartPull(Player* player, BotRuntime& rt, Creature* target, PullPlan const& plan, PullVerdict const& here, bool forGroup)
    {
        uint32 const now = getMSTime();
        BotRuntime::CarefulPull& pull = rt.pull;
        pull = BotRuntime::CarefulPull();
        pull.active = true;
        pull.ranged = plan.ranged;
        pull.forGroup = forGroup;
        pull.target = target->GetGUID();
        pull.spell = plan.spell;
        pull.mapId = player->GetMapId();
        pull.startedAt = now;
        pull.phaseAt = now;
        pull.pullX = plan.pullX;
        pull.pullY = plan.pullY;
        pull.pullZ = plan.pullZ;
        pull.fightX = plan.fightX;
        pull.fightY = plan.fightY;
        pull.fightZ = plan.fightZ;
        bool const there = player->GetExactDist2d(plan.pullX, plan.pullY) <= PullArriveYards;
        pull.phase = plan.ranged ? (there ? PullCast : PullToPlace) : PullWalkIn;

        // mod-playerbots lets go of the target until the creature is with the bot (it would run up to it).
        BotDriver::DropTarget(player);
        BotDriver::PauseAI(player, PullPauseMs);
        // Out of stealth: a creature does not come for what it cannot see (a rogue stood seven yards from a skeleton
        // for ten seconds).
        if (!plan.ranged)
            player->RemoveAurasByType(SPELL_AURA_MOD_STEALTH);
        if (pull.phase != PullCast)
            player->GetMotionMaster()->MovePoint(0, plan.pullX, plan.pullY, plan.pullZ, FORCED_MOVEMENT_RUN);

        SpellInfo const* info = plan.spell ? sSpellMgr->GetSpellInfo(plan.spell) : nullptr;
        LOG_INFO(ActivityLog, "activity: {} pulls {} (level {}) to it {}{}{}", rt.name, target->GetName(), uint32(target->GetLevel()),
            plan.ranged ? Acore::StringFormat("with {} from {:.0f} yd{}", info ? info->SpellName[0] : "a shot", plan.fromYards,
                    plan.side ? ", from the side away from the others" : "") :
                Acore::StringFormat("by walking into its sight ({:.0f} yd) and {:.0f} yd back", plan.fromYards,
                    std::hypot(plan.fightX - plan.pullX, plan.fightY - plan.pullY)),
            plan.spared ? Acore::StringFormat(": {} fewer would join than at its place ({})", plan.spared, plan.there) : std::string(),
            (here.foes.size() > 1 ? Acore::StringFormat("; {} may still come ({})", here.foes.size() - 1, here.joiners) : std::string()) +
                (plan.ranged || plan.note.empty() ? std::string() : " [" + plan.note + "]"));
    }

    void EndPull(Player* player, BotRuntime& rt, Creature* target, char const* how, bool attack, bool failed)
    {
        BotRuntime::CarefulPull const pull = rt.pull;
        rt.pull = BotRuntime::CarefulPull();
        if (failed)
        {
            rt.pullFailedTarget = pull.target;
            rt.pullFailedUntil = getMSTime() + PullFailedMs;
        }
        player->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
        BotDriver::PauseAI(player, 0);
        bool const attacks = attack && target && target->IsAlive() && BotDriver::Attack(player, target);
        LOG_INFO(ActivityLog, "activity: {} pull of {}: {} after {:.1f} s{}", rt.name, target ? target->GetName() : "a creature", how,
            float(getMSTimeDiff(pull.startedAt, getMSTime())) / float(IN_MILLISECONDS),
            target && target->IsAlive() ? Acore::StringFormat(", {:.0f} yd from where it stood{}", std::hypot(
                player->GetPositionX() - rt.pullOkX, player->GetPositionY() - rt.pullOkY), attacks ? "" : " (it does not attack it)") : std::string());
    }
}

// A creature without a spawn that something the bot did just brought (a summon of its quest item, or of the trap of
// an object its quest key opened) and that a quest in its log wants killed: the quest's own creature, not one the bot
// picks. It is not judged here: refusing it leaves it standing where it came and the quest gets nowhere (the
// observer's "Spirits of Stonemaul Hold": a Stonemaul Spirit that attacks nobody by itself). What an item summons is
// judged before the item is used (Risk::JudgeSummon); a creature four levels or more above the bot (an elite three
// more) is still judged.
static bool QuestBroughtIt(Player* player, Creature const* target)
{
    TempSummon const* summon = target->ToTempSummon();
    if (!summon || target->GetSpawnId())
        return false;
    uint32 const worth = target->GetLevel() + (target->isElite() ? 3u : 0u);
    if (worth >= player->GetLevel() + 4)
        return false;
    CreatureTemplate const* info = target->GetCreatureTemplate();
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = player->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest || player->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;
        for (int32 const wanted : quest->RequiredNpcOrGo)
            if (wanted > 0 && (uint32(wanted) == target->GetEntry() ||
                (info && (uint32(wanted) == info->KillCredit[0] || uint32(wanted) == info->KillCredit[1]))))
                return true;
    }
    return false;
}

// Whether a quest in the character's log still needs this creature: a kill of it (or of its kill credit), or an item
// it drops.
// A quest the bot has set aside (until a level, or for a while: BotRuntime::IsDangerous) wants nothing of it now: the
// priest of the Dreadbone Sentinels had left "Spinal Dust" until level 25 and still fought the Sentinels beside its
// way, for that quest.
static bool QuestWants(Player* player, Creature const* target, BotRuntime const* rt = nullptr)
{
    CreatureTemplate const* info = target->GetCreatureTemplate();
    uint32 const entry = target->GetEntry();
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = player->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest || player->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;
        if (rt && rt->IsDangerous(questId, player->GetLevel()))
            continue;
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            int32 const wanted = quest->RequiredNpcOrGo[i];
            if (wanted > 0 && (uint32(wanted) == entry ||
                (info && (uint32(wanted) == info->KillCredit[0] || uint32(wanted) == info->KillCredit[1]))))
                return true;
        }
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (uint32 const item = quest->RequiredItemId[i]; item && player->GetItemCount(item, true) < quest->RequiredItemCount[i])
            {
                std::vector<uint32> const sources = sWorldIndex.GetItemSourceCreatures(item);
                if (std::find(sources.begin(), sources.end(), entry) != sources.end())
                    return true;
            }
    }
    return false;
}

// On its way to its work the bot fights what its work wants, what attacks it and what stands in its way, nothing else
// (the gaps thread's reading of the live log: a level-47 rogue on its way to the Dark Iron Slavers of "Culling the
// Competition" fought six Greater Lava Spiders in a row, one of them by a careful pull with Throw from 28 yards, never
// got nearer, and the stuck rule ended its task after 184 seconds). What "in its way" is: the creature's reach (its
// aggro range and a margin) covers where the bot stands or the next 45 yards of its way. Not for a grind (any target
// there is its work), an escort or a quest event (it defends), a squad member (it fights beside its leader), a
// dungeon.
static bool OffItsWork(Player* player, BotRuntime& rt, Creature* target)
{
    switch (rt.task.kind)
    {
        case TaskKind::Objective:
        case TaskKind::Interact:
        case TaskKind::Travel:
        case TaskKind::Voyage:
        case TaskKind::TurnIn:
        case TaskKind::Pickup:
        case TaskKind::Vendor:
        case TaskKind::Train:
        case TaskKind::Fish:
        case TaskKind::Discover:
        case TaskKind::SafeSpot:
            break;
        default:
            return false;
    }
    if (rt.squad.following || player->GetMap()->IsDungeon() || target->GetVictim() == player || target->IsInCombatWith(player))
        return false;
    if (!rt.task.spot.gameObject && rt.task.spot.entry && target->GetEntry() == rt.task.spot.entry)
        return false;
    constexpr float ReachMargin = 5.0f;
    constexpr float MinReach = 12.0f;
    float const reach = std::max(MinReach, target->GetAggroRange(player) + ReachMargin);
    if (target->GetExactDist2d(player) <= reach)
        return false;
    std::vector<std::pair<float, float>> const way = WayAhead(player, 45.0f, 0.0f);
    if (way.size() >= 2 && WayDistanceAlong(target, way) <= reach)
        return false;
    // (Looked at last: it walks the quest log.)
    static std::unordered_map<ObjectGuid, std::pair<ObjectGuid, bool>> wanted;
    std::pair<ObjectGuid, bool>& known = wanted[player->GetGUID()];
    if (known.first != target->GetGUID())
        known = { target->GetGUID(), QuestWants(player, target, &rt) };
    return !known.second;
}

// A bystander: a creature that does not fight the bot and is nobody's to fight, a civilian or one with a service (a
// flight master, a vendor, a trainer) that the bot's side may attack all the same (the Horde's flight master Georgia,
// level 65, in the neutral town of Light's Hope, attackable for the Alliance). mod-playerbots turns to her like to any
// hostile creature beside its way: Leel pulled her ("its own pull (1 expected)"), and Bommbang, Priestyo and Enhancer
// died to her with her at full health, round 147. Unless a quest of the bot wants her.
static bool IsBystander(Player* player, Creature* target)
{
    if (!target || target->GetVictim() == player || target->IsInCombatWith(player))
        return false;
    bool const civilian = target->GetCreatureTemplate() && (target->GetCreatureTemplate()->flags_extra & CREATURE_FLAG_EXTRA_CIVILIAN);
    constexpr uint32 Services = UNIT_NPC_FLAG_GOSSIP | UNIT_NPC_FLAG_QUESTGIVER | UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_VENDOR |
        UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_BANKER | UNIT_NPC_FLAG_REPAIR | UNIT_NPC_FLAG_AUCTIONEER |
        UNIT_NPC_FLAG_STABLEMASTER | UNIT_NPC_FLAG_SPIRITHEALER | UNIT_NPC_FLAG_BATTLEMASTER;
    // (A service counts only on a creature some players' side may deal with: a Dredge Crusher of Silithus, a monster
    // hostile to every player, carries a quest giver's flag in the world database; it was left alone as a bystander,
    // came at the bot all the same, and a grind spot with it "counted as empty" after a minute of no fight, round 231.)
    FactionTemplateEntry const* faction = target->GetFactionTemplateEntry();
    bool const serves = (target->GetNpcFlags() & Services) && faction && !faction->IsHostileToPlayers();
    if (!civilian && !serves)
        return false;
    return !QuestWants(player, target);
}

static void NoteBystander(Player* player, BotRuntime& rt, Creature* target, bool fighting)
{
    constexpr uint32 NoteAgainMs = 10 * MINUTE * IN_MILLISECONDS;
    static std::unordered_map<ObjectGuid, std::unordered_map<ObjectGuid, uint32>> noted;
    std::unordered_map<ObjectGuid, uint32>& mine = noted[player->GetGUID()];
    uint32 const nowMs = getMSTime();
    auto known = mine.find(target->GetGUID());
    if (known != mine.end() && getMSTimeDiff(known->second, nowMs) < NoteAgainMs)
        return;
    mine[target->GetGUID()] = nowMs;
    LOG_INFO(ActivityLog, "activity: {} leaves {} (level {}{}, {:.0f} yd) alone{}: it does not fight the bot, and it is a civilian "
        "or serves the other side", rt.name, target->GetName(), uint32(target->GetLevel()), RankName(target), player->GetExactDist(target),
        fighting ? " in its fight" : "");
}

bool Escape::StepStarfall(Player* player, BotRuntime& rt)
{
    // Starfall's ranks (the druid's own aura; its periodic effect triggers the star spell, whose radius is the reach).
    static constexpr uint32 StarfallRanks[] = { 48505, 53199, 53200, 53201 };
    if (player->getClass() != CLASS_DRUID || !player->IsAlive())
        return false;
    Aura* aura = nullptr;
    for (uint32 rank : StarfallRanks)
        if ((aura = player->GetAura(rank)))
            break;
    if (!aura)
        return false;
    // Its fight is over: what the stars still fall on is in no fight with it. (A druid's first Waywatcher died, its
    // rest walked it 22 yd on, and the next pulse of its Starfall reached a second Waywatcher, which attacked it at
    // once: the pulses come every second, between two looks of this check.)
    if (!player->IsInCombat())
    {
        player->RemoveAurasDueToSpell(aura->GetId());
        if (GetSettings().debug)
            LOG_INFO(ActivityLog, "activity: {} ends its Starfall: its fight is over", rt.name);
        return true;
    }
    float reach = 30.0f;
    for (SpellEffectInfo const& effect : aura->GetSpellInfo()->Effects)
        if (effect.TriggerSpell)
            if (SpellInfo const* stars = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                for (SpellEffectInfo const& starEffect : stars->Effects)
                    if (starEffect.HasRadius())
                        reach = std::max(reach, starEffect.CalcRadius(player));
    std::list<Unit*> units;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, reach);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, units, check);
    Cell::VisitObjects(player, searcher, reach);
    for (Unit* unit : units)
    {
        Creature* creature = unit->ToCreature();
        if (!creature || !creature->IsAlive() || creature->IsInCombat() || creature->IsCritter() || creature->IsTotem() ||
            !player->IsValidAttackTarget(creature) || !player->IsWithinLOSInMap(creature))
            continue;
        player->RemoveAurasDueToSpell(aura->GetId());
        LOG_INFO(ActivityLog, "activity: {} ends its Starfall: its stars would reach {} (level {}, {:.0f} yd away), which is in no fight",
            rt.name, creature->GetName(), uint32(creature->GetLevel()), player->GetExactDist(creature));
        return true;
    }
    return false;
}

Creature* Escape::BystanderInReach(Player* player, float yards)
{
    constexpr float ChainYards = 6.0f; // a melee chain jumps 5 yd from the creature it hits (Spell::SearchChainTargets)
    constexpr float SwingYards = 10.0f;
    // (Only a victim it swings at: a paladin's seal came off for a Sand Skitterer 148 yd away beside a victim as far.)
    Unit const* victim = player->GetVictim();
    if (victim && player->GetExactDist2d(victim) > SwingYards)
        victim = nullptr;
    float const look = std::max(yards, victim ? player->GetExactDist2d(victim) + ChainYards : 0.0f);
    std::list<Unit*> units;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, look);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, units, check);
    Cell::VisitObjects(player, searcher, look);
    Creature* nearest = nullptr;
    float closest = 0.0f;
    for (Unit* unit : units)
    {
        Creature* creature = unit->ToCreature();
        if (!creature || !creature->IsAlive() || creature->IsInCombat() || creature->IsPet() || creature->IsTotem() ||
            creature->IsCritter() || !player->IsValidAttackTarget(creature))
            continue;
        float const here = player->GetExactDist2d(creature);
        if (here > yards && (!victim || creature->GetExactDist2d(victim) > ChainYards))
            continue;
        if (!nearest || here < closest)
        {
            nearest = creature;
            closest = here;
        }
    }
    return nearest;
}

bool Escape::StepSealOfCommand(Player* player, BotRuntime& rt)
{
    // Liriel (32, paladin of the test guild) fought a Highland Strider in Arathi, "1 expected": Fozruk (a neutral
    // level-42 giant) and Sleeby, Znort and Feeboz, his companions on a round of 235 waypoints, walked past the fight,
    // and two seconds later all four were on it; it died. They attack nobody by themselves (neutral: the pull check
    // rightly left them out); its Seal of Command, which chains every swing to two more creatures beside the target,
    // hit Fozruk, and his formation came with him (round 242).
    constexpr uint32 SealOfCommand = 20375;
    if (player->getClass() != CLASS_PALADIN || !player->IsAlive() || !player->IsInCombat() || !player->HasAura(SealOfCommand))
        return false;
    Creature* bystander = BystanderInReach(player, 5.0f);
    if (!bystander)
        return false;
    player->RemoveAurasDueToSpell(SealOfCommand);
    constexpr uint32 NoteAgainMs = 10 * MINUTE * IN_MILLISECONDS;
    static std::unordered_map<ObjectGuid, std::unordered_map<ObjectGuid, uint32>> noted;
    std::unordered_map<ObjectGuid, uint32>& mine = noted[player->GetGUID()];
    uint32 const nowMs = getMSTime();
    if (mine.size() > 64)
        for (auto it = mine.begin(); it != mine.end();)
            it = getMSTimeDiff(it->second, nowMs) >= NoteAgainMs ? mine.erase(it) : std::next(it);
    auto known = mine.find(bystander->GetGUID());
    if (known == mine.end() || getMSTimeDiff(known->second, nowMs) >= NoteAgainMs)
    {
        mine[bystander->GetGUID()] = nowMs;
        LOG_INFO(ActivityLog, "activity: {} takes its Seal of Command off: {} (level {}{}, {:.0f} yd away) is in no fight and its "
            "swings would chain to it", rt.name, bystander->GetName(), uint32(bystander->GetLevel()),
            bystander->IsHostileTo(player) ? "" : ", neutral", player->GetExactDist(bystander));
    }
    return true;
}

namespace
{
    // The spell `unit` casts that an interrupt stops, as the core's Spell::EffectInterruptCast decides it (a cast with a
    // cast time, or a channel, of the "silence" prevention type with the interrupt flag), and only while there is time
    // left to stop it.
    Spell* InterruptibleCast(Unit* unit)
    {
        constexpr int32 LeastLeftMs = 300;
        for (uint32 i = CURRENT_FIRST_NON_MELEE_SPELL; i < CURRENT_AUTOREPEAT_SPELL; ++i)
        {
            Spell* spell = unit->GetCurrentSpell(CurrentSpellTypes(i));
            if (!spell)
                continue;
            SpellInfo const* info = spell->GetSpellInfo();
            bool const state = spell->getState() == SPELL_STATE_CASTING ||
                (spell->getState() == SPELL_STATE_PREPARING && spell->GetCastTime() > 0);
            bool const flag = (i == CURRENT_GENERIC_SPELL && (info->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT)) ||
                (i == CURRENT_CHANNELED_SPELL && (info->ChannelInterruptFlags & CHANNEL_INTERRUPT_FLAG_INTERRUPT));
            if (!state || !flag || info->PreventionType != SPELL_PREVENTION_TYPE_SILENCE)
                continue;
            if (i == CURRENT_GENERIC_SPELL && spell->getState() == SPELL_STATE_PREPARING && spell->GetCastTimeRemaining() < LeastLeftMs)
                continue;
            return spell;
        }
        return nullptr;
    }

    // The rage a warrior keeps through a change of stance (the core's HandleAuraModShapeshift: Stance Mastery and Tactical
    // Mastery, both spells of the warrior family with icon 139), in the core's tenths.
    uint32 RageKeptThroughStance(Player* player)
    {
        uint32 kept = 0;
        for (auto const& [spellId, spell] : player->GetSpellMap())
        {
            if (!spell || spell->State == PLAYERSPELL_REMOVED || !spell->IsInSpec(player->GetActiveSpec()))
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (info && info->SpellFamilyName == SPELLFAMILY_WARRIOR && info->SpellIconID == 139)
                kept += player->CalculateSpellDamage(player, info, 0) * 10;
        }
        for (auto const& [spellId, talent] : player->GetTalentMap())
        {
            if (!talent || talent->State == PLAYERSPELL_REMOVED || !talent->IsInSpec(player->GetActiveSpec()))
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (info && info->SpellFamilyName == SPELLFAMILY_WARRIOR && info->SpellIconID == 139)
                kept += player->CalculateSpellDamage(player, info, 0) * 10;
        }
        return kept;
    }

    // In reach of an interrupt: melee spells by the melee reach, the others by their range and the sight line.
    bool InInterruptReach(Player* player, Unit* target, SpellInfo const* info)
    {
        if (info->RangeEntry && (info->RangeEntry->Flags & SPELL_RANGE_MELEE))
            return player->IsWithinMeleeRange(target);
        return player->IsWithinCombatRange(target, info->GetMaxRange(false, player)) && player->IsWithinLOSInMap(target);
    }
}

bool Escape::StepInterrupt(Player* player, BotRuntime& rt)
{
    constexpr uint32 BerserkerStance = 2458;
    constexpr uint32 PummelWaitMs = 1500;
    if (!player->IsAlive() || !player->IsInCombat() || player->IsMounted() || player->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED |
        UNIT_STATE_FLEEING) || player->HasUnitFlag(UNIT_FLAG_SILENCED) || player->HasUnitFlag(UNIT_FLAG_PACIFIED) ||
        !sBotSettings.IsOn(player, Setting::FightInterrupt))
        return false;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    // The foes of its fight: what it attacks and what attacks it.
    std::vector<Unit*> foes;
    if (Unit* victim = player->GetVictim())
        foes.push_back(victim);
    if (Unit* intended = BotDriver::IntendedTarget(player); intended && std::find(foes.begin(), foes.end(), intended) == foes.end())
        foes.push_back(intended);
    for (Unit* attacker : player->getAttackers())
        if (std::find(foes.begin(), foes.end(), attacker) == foes.end())
            foes.push_back(attacker);
    // Which interrupts its class has, in the order they are tried (a warrior's Shield Bash needs a shield and Battle or
    // Defensive Stance, Pummel Berserker Stance; a druid's Bash its bear form).
    std::vector<char const*> ways;
    switch (player->getClass())
    {
        case CLASS_ROGUE: ways = { "kick" }; break;
        case CLASS_WARRIOR: ways = { "shield bash", "pummel" }; break;
        case CLASS_MAGE: ways = { "counterspell" }; break;
        case CLASS_SHAMAN: ways = { "wind shear" }; break;
        case CLASS_DEATH_KNIGHT: ways = { "mind freeze" }; break;
        case CLASS_PRIEST: ways = { "silence" }; break;
        case CLASS_HUNTER: ways = { "silencing shot" }; break;
        case CLASS_DRUID: ways = { "bash" }; break;
        default: break; // a paladin's Hammer of Justice and a warlock's felhunter are mod-playerbots' own
    }
    if (ways.empty())
        return false;
    ShapeshiftForm const form = player->GetShapeshiftForm();
    for (Unit* foe : foes)
    {
        if (!foe || !foe->IsAlive() || !foe->IsInWorld() || foe->GetMapId() != player->GetMapId() || !player->IsValidAttackTarget(foe))
            continue;
        Spell* cast = InterruptibleCast(foe);
        if (!cast)
            continue;
        for (char const* way : ways)
        {
            uint32 const spellId = BotDriver::SpellIdByName(player, way);
            SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
            if (!info || !player->HasSpell(spellId) || player->HasSpellCooldown(spellId) || !InInterruptReach(player, foe, info))
                continue;
            bool const pummel = player->getClass() == CLASS_WARRIOR && std::string(way) == "pummel";
            if (player->getClass() == CLASS_WARRIOR && !pummel &&
                (!player->HasItemFitToSpellRequirements(info) || (form != FORM_BATTLESTANCE && form != FORM_DEFENSIVESTANCE)))
                continue;
            if (player->getClass() == CLASS_DRUID && form != FORM_BEAR && form != FORM_DIREBEAR)
                continue;
            if (pummel && form != FORM_BERSERKERSTANCE)
            {
                // The change of stance first: only when the rage it keeps pays for the Pummel (10 rage) and it has that
                // much now. The rest of its rage is lost, as for a player who does the same.
                uint32 const cost = uint32(info->CalcPowerCost(player, info->GetSchoolMask()));
                if (!player->HasSpell(BerserkerStance) || player->HasSpellCooldown(BerserkerStance) ||
                    player->GetPower(POWER_RAGE) < int32(cost) || RageKeptThroughStance(player) < cost)
                    continue;
                if (player->CastSpell(player, BerserkerStance, false) != SPELL_CAST_OK)
                    continue;
                rt.pummelTarget = foe->GetGUID();
                rt.pummelUntil = now + PummelWaitMs;
                if (player->GetShapeshiftForm() != FORM_BERSERKERSTANCE)
                    return true;
            }
            std::string const foeSpell = cast->GetSpellInfo()->SpellName[0];
            // (Its own cast gives way: the interrupt matters more.)
            if (player->IsNonMeleeSpellCast(false, false, true))
                player->InterruptNonMeleeSpells(false);
            if (player->CastSpell(foe, spellId, false) != SPELL_CAST_OK)
                continue;
            if (rt.fight.active && rt.fight.interrupts < 255)
                ++rt.fight.interrupts;
            rt.pummelTarget.Clear();
            if (GetSettings().debug)
                LOG_INFO(ActivityLog, "activity: {} interrupts {}'s {} with {} ({:.0f} yd away)", rt.name, foe->GetName(), foeSpell,
                    info->SpellName[0], player->GetExactDist(foe));
            return true;
        }
    }
    // A warrior that changed its stance for a Pummel the foe gave no occasion for any more: nothing more to do (its
    // rotation changes back).
    if (!rt.pummelTarget.IsEmpty() && now >= rt.pummelUntil)
        rt.pummelTarget.Clear();
    return false;
}

bool Escape::StepStopRunner(Player* player, BotRuntime& rt)
{
    constexpr float FleeAssistYards = 30.0f;  // the core sends a runner to a creature of its kind this near
    constexpr uint32 AgainMs = 4 * IN_MILLISECONDS; // another try at the same runner after this
    if (!player->IsAlive() || !player->IsInCombat() || player->IsMounted() || rt.escape.active ||
        player->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING) || player->HasUnitFlag(UNIT_FLAG_SILENCED) ||
        player->HasUnitFlag(UNIT_FLAG_PACIFIED))
        return false;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    std::vector<Unit*> foes;
    if (Unit* victim = player->GetVictim())
        foes.push_back(victim);
    if (Unit* intended = BotDriver::IntendedTarget(player); intended && std::find(foes.begin(), foes.end(), intended) == foes.end())
        foes.push_back(intended);
    for (Unit* attacker : player->getAttackers())
        if (std::find(foes.begin(), foes.end(), attacker) == foes.end())
            foes.push_back(attacker);
    Creature* runner = nullptr;
    for (Unit* foe : foes)
    {
        Creature* creature = foe ? foe->ToCreature() : nullptr;
        if (!creature || !creature->IsAlive() || !creature->IsInWorld() || creature->GetMapId() != player->GetMapId() ||
            !player->IsValidAttackTarget(creature))
            continue;
        MovementGeneratorType const type = creature->GetMotionMaster()->GetCurrentMovementGeneratorType();
        if (type == ASSISTANCE_MOTION_TYPE || type == ASSISTANCE_DISTRACT_MOTION_TYPE)
        {
            runner = creature;
            break;
        }
    }
    if (!runner || (rt.runnerStopped == runner->GetGUID() && getMSTimeDiff(rt.runnerStopAt, now) < AgainMs))
        return false;
    // Only when it would bring someone: a creature of its faction in no fight within the core's reach of it.
    Creature* helper = nullptr;
    for (Creature* creature : LiveHostiles(player, 70.0f, PackLevelsBelow))
        if (creature != runner && !creature->IsInCombat() && creature->GetFaction() == runner->GetFaction() &&
            creature->GetExactDist(runner) <= FleeAssistYards + creature->GetObjectSize() + runner->GetObjectSize() &&
            (!helper || creature->GetExactDist(runner) < helper->GetExactDist(runner)))
            helper = creature;
    if (!helper)
        return false;
    // What stops a runner, in the order tried: stuns and roots, then slows, then what may finish it (it runs below a
    // fifth of its health). The core's own rules decide the form or stance (a bear's Bash, a druid's Entangling Roots in
    // its caster form only), the range and the cost.
    std::vector<char const*> ways;
    switch (player->getClass())
    {
        case CLASS_DRUID: ways = { "bash", "maim", "entangling roots", "moonfire" }; break;
        case CLASS_WARRIOR: ways = { "hamstring", "piercing howl", "heroic throw" }; break;
        case CLASS_ROGUE: ways = { "kidney shot", "gouge", "deadly throw" }; break;
        case CLASS_PALADIN: ways = { "hammer of justice", "judgement of justice" }; break;
        case CLASS_SHAMAN: ways = { "frost shock", "earth shock" }; break;
        case CLASS_MAGE: ways = { "fire blast", "ice lance", "frostbolt" }; break;
        case CLASS_HUNTER: ways = { "concussive shot", "wing clip", "arcane shot" }; break;
        case CLASS_PRIEST: ways = { "mind flay", "shadow word: death" }; break;
        case CLASS_WARLOCK: ways = { "curse of exhaustion", "shadowburn" }; break;
        case CLASS_DEATH_KNIGHT: ways = { "chains of ice", "death grip", "icy touch" }; break;
        default: break;
    }
    rt.runnerStopped = runner->GetGUID();
    rt.runnerStopAt = now;
    for (char const* way : ways)
    {
        uint32 const spellId = BotDriver::SpellIdByName(player, way);
        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!info || !player->HasSpell(spellId) || player->HasSpellCooldown(spellId) || !StanceAllows(player, info) ||
            !InInterruptReach(player, runner, info))
            continue;
        if (info->PowerType == POWER_MANA || info->PowerType == POWER_RAGE || info->PowerType == POWER_ENERGY ||
            info->PowerType == POWER_RUNIC_POWER)
            if (info->CalcPowerCost(player, info->GetSchoolMask()) > int32(player->GetPower(Powers(info->PowerType))))
                continue;
        if (player->IsNonMeleeSpellCast(false, false, true))
            player->InterruptNonMeleeSpells(false);
        if (player->CastSpell(runner, spellId, false) != SPELL_CAST_OK)
            continue;
        LOG_INFO(ActivityLog, "activity: {} stops {} (level {}, {:.0f}% health), which runs for help to {} ({:.0f} yd from it), with {}",
            rt.name, runner->GetName(), uint32(runner->GetLevel()), runner->GetHealthPct(), helper->GetName(), helper->GetExactDist(runner),
            info->SpellName[0]);
        return true;
    }
    return false;
}

bool Escape::DropBehind(Player* player, float x, float y, float z, float fromX, float fromY)
{
    constexpr float DropYards = 6.0f;
    float dx = x - fromX;
    float dy = y - fromY;
    float const length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.5f)
    {
        float const o = player->GetOrientation() + float(M_PI);
        dx = std::cos(o);
        dy = std::sin(o);
    }
    else
    {
        dx /= length;
        dy /= length;
    }
    Map* map = player->GetMap();
    uint32 const phase = player->GetPhaseMask();
    for (float const d : { 3.0f, 6.0f, 9.0f, 12.0f })
    {
        float const px = x + dx * d;
        float const py = y + dy * d;
        if (map->IsInWater(phase, px, py, z - 1.0f, player->GetCollisionHeight()))
            continue;
        float const floor = map->GetHeight(phase, px, py, z + 2.0f, true, 50.0f);
        if (floor <= INVALID_HEIGHT || z - floor >= DropYards)
            return true;
    }
    return false;
}

bool Escape::StepKnockbackSide(Player* player, BotRuntime& rt)
{
    constexpr uint32 AgainMs = 3 * IN_MILLISECONDS;
    constexpr float MaxHeight = 3.0f;
    constexpr float MaxWalk = 15.0f;
    if (!player->IsAlive() || !player->IsInCombat() || player->IsMounted() || rt.escape.active || !player->FindMap() ||
        player->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING) ||
        player->IsNonMeleeSpellCast(false) || !BotDriver::FightsInMelee(player))
        return false;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (rt.knockSideAt && getMSTimeDiff(rt.knockSideAt, now) < AgainMs)
        return false;
    Creature* knocker = nullptr;
    for (Unit* attacker : player->getAttackers())
        if (Creature* creature = attacker ? attacker->ToCreature() : nullptr)
            if (creature->IsAlive() && creature->GetVictim() == player && sWorldIndex.KnocksBack(creature->GetEntry()) &&
                player->IsWithinDistInMap(creature, player->GetMeleeRange(creature) + 5.0f))
            {
                knocker = creature;
                break;
            }
    if (!knocker || !DropBehind(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), knocker->GetPositionX(),
            knocker->GetPositionY()))
        return false;
    rt.knockSideAt = now;
    // Its other side, from the bot through the creature, and then a quarter and a half round either way.
    Map* map = player->GetMap();
    uint32 const phase = player->GetPhaseMask();
    float const kx = knocker->GetPositionX();
    float const ky = knocker->GetPositionY();
    float const kz = knocker->GetPositionZ();
    float const toward = std::atan2(ky - player->GetPositionY(), kx - player->GetPositionX());
    float const reach = (knocker->GetCombatReach() + player->GetCombatReach()) * 0.8f;
    for (float const turn : { 0.0f, 0.785f, -0.785f, 1.571f, -1.571f })
    {
        float const angle = toward + turn;
        float const x = kx + std::cos(angle) * reach;
        float const y = ky + std::sin(angle) * reach;
        float const z = map->GetHeight(phase, x, y, kz + 2.0f, true, MaxHeight + 2.0f);
        if (z <= INVALID_HEIGHT || std::fabs(z - kz) > MaxHeight || DropBehind(player, x, y, z, kx, ky))
            continue;
        PathGenerator path(player);
        path.CalculatePath(x, y, z, false);
        if (!(path.GetPathType() & PATHFIND_NORMAL) || path.getPathLength() > MaxWalk)
            continue;
        player->GetMotionMaster()->MovePoint(0, x, y, z);
        BotDriver::PauseAI(player, 1500);
        if (rt.knockSideFor != knocker->GetGUID())
        {
            rt.knockSideFor = knocker->GetGUID();
            LOG_INFO(ActivityLog, "activity: {} steps round {} (level {}) to its other side: it knocks back, and the ground drops "
                "behind the bot at ({:.0f}, {:.0f}, {:.0f})", rt.name, knocker->GetName(), uint32(knocker->GetLevel()),
                player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
        }
        return true;
    }
    return false;
}

bool Escape::StepChaseRunner(Player* player, BotRuntime& rt)
{
    constexpr float ChaseYards = 18.0f;               // how far from where the run began a runner is followed
    constexpr uint32 PatienceMs = 20 * IN_MILLISECONDS; // a runner that keeps running is left to mod-playerbots after this
    if (!player->IsAlive() || !player->IsInCombat() || player->IsMounted() || rt.escape.active || !player->FindMap() ||
        player->FindMap()->IsDungeon() || !BotDriver::FightsInMelee(player))
        return false;
    Unit* victim = player->GetVictim();
    if (!victim)
        victim = BotDriver::IntendedTarget(player);
    Creature* runner = victim ? victim->ToCreature() : nullptr;
    if (!runner || !runner->IsAlive() || !runner->IsInWorld() || runner->GetMapId() != player->GetMapId())
        return false;
    MovementGeneratorType const type = runner->GetMotionMaster()->GetCurrentMovementGeneratorType();
    bool const running = runner->HasUnitState(UNIT_STATE_FLEEING) || type == FLEEING_MOTION_TYPE || type == ASSISTANCE_MOTION_TYPE ||
        type == ASSISTANCE_DISTRACT_MOTION_TYPE;
    if (!running)
        return false;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (rt.runnerSeen != runner->GetGUID())
    {
        rt.runnerSeen = runner->GetGUID();
        rt.runnerSeenAt = now;
        rt.runnerFromX = player->GetPositionX();
        rt.runnerFromY = player->GetPositionY();
        if (GetSettings().debug)
            LOG_INFO(ActivityLog, "activity: {} sees {} (level {}, {:.0f}% health) run away ({}), {:.0f} yd from it", rt.name,
                runner->GetName(), uint32(runner->GetLevel()), runner->GetHealthPct(),
                type == ASSISTANCE_MOTION_TYPE || type == ASSISTANCE_DISTRACT_MOTION_TYPE ? "for help" : "in fear", player->GetExactDist(runner));
    }
    if (getMSTimeDiff(rt.runnerSeenAt, now) > PatienceMs)
        return false;
    // What stands where it runs: a creature in no fight that would have the bot in its reach at the runner's place.
    Creature* inTheWay = nullptr;
    for (Creature* other : LiveHostiles(player, 70.0f))
        if (other != runner && !other->IsInCombat() && other->IsWithinDistInMap(runner, other->GetAggroRange(player) + 6.0f) &&
            (!inTheWay || other->GetExactDist(runner) < inTheWay->GetExactDist(runner)))
            inTheWay = other;
    float const dx = runner->GetPositionX() - rt.runnerFromX;
    float const dy = runner->GetPositionY() - rt.runnerFromY;
    bool const ranFar = dx * dx + dy * dy > ChaseYards * ChaseYards;
    if (!inTheWay && !ranFar)
        return false;
    // It lets the runner go: another foe on it if there is one, else it waits where it stands (the runner comes back
    // when its run ends, or brings its help either way).
    Creature* instead = nullptr;
    for (Unit* attacker : player->getAttackers())
        if (Creature* creature = attacker->ToCreature(); creature && creature != runner && creature->IsAlive() &&
            !creature->HasUnitState(UNIT_STATE_FLEEING) && player->IsWithinDistInMap(creature, 20.0f) &&
            (!instead || player->GetExactDist(creature) < player->GetExactDist(instead)))
            instead = creature;
    bool const turned = instead && (player->GetVictim() == instead || BotDriver::Attack(player, instead));
    if (!turned)
    {
        player->AttackStop();
        player->StopMoving();
        BotDriver::PauseAI(player, 1500);
    }
    if (rt.runnerLetGo != runner->GetGUID())
    {
        rt.runnerLetGo = runner->GetGUID();
        LOG_INFO(ActivityLog, "activity: {} lets {} (level {}, {:.0f}% health) run and does not follow it: {}; {}", rt.name,
            runner->GetName(), uint32(runner->GetLevel()), runner->GetHealthPct(),
            inTheWay ? Acore::StringFormat("{} (level {}) stands where it runs to", inTheWay->GetName(), uint32(inTheWay->GetLevel())) :
                std::string("it has run too far from where the fight was"),
            turned ? Acore::StringFormat("it fights {} meanwhile", instead->GetName()) : std::string("it waits where it stands"));
    }
    return true;
}

bool Escape::DropBystander(Player* player, BotRuntime& rt)
{
    Unit* unit = BotDriver::IntendedTarget(player);
    Creature* target = unit ? unit->ToCreature() : nullptr;
    if (!target || !target->IsAlive() || target->IsFriendlyTo(player) || !IsBystander(player, target))
        return false;
    BotDriver::LetGo(player, target);
    NoteBystander(player, rt, target, true);
    return true;
}

Escape::Pull Escape::CheckPull(Player* player, BotRuntime& rt, float& x, float& y)
{
    constexpr float MaxPullYards = 45.0f;
    constexpr uint32 NoteGapMs = 20 * IN_MILLISECONDS;
    constexpr uint32 RestWantMs = 3 * MINUTE * IN_MILLISECONDS;

    if (!player->IsAlive() || player->IsInCombat() || rt.escape.active || rt.pull.active || !sBotSettings.IsOn(player, Setting::RiskEnable))
        return Pull::Fine;
    Unit* unit = BotDriver::IntendedTarget(player);
    Creature* target = unit ? unit->ToCreature() : nullptr;
    if (!target || !target->IsAlive() || target->IsInCombat() || target->IsPet() || target->IsTotem() || target->IsFriendlyTo(player) ||
        player->GetExactDist(target) > MaxPullYards)
        return Pull::Fine;
    // The quest's own creature that the bot's item or key just brought (QuestBroughtIt): no verdict, no careful pull.
    if (QuestBroughtIt(player, target))
        return Pull::Fine;
    // The fight of an escort beside its NPC: the escort is over when the bot does not take it (the guild leader called
    // off its attack on Lieutenant Caldwell's ambush of "Questioning Reethe" as a pull it could not afford, and the
    // escort ended, 2026-11-25). Ogron fights it too.
    if (rt.task.kind == TaskKind::Escort && rt.task.escortEntry && target->IsHostileTo(player))
        if (Creature* ward = player->FindNearestCreature(rt.task.escortEntry, WardBesideYards, true);
            ward && ward->GetExactDist(target) <= WardBesideYards)
            return Pull::Fine;
    // A bystander (IsBystander) is not fought at all.
    if (IsBystander(player, target))
    {
        BotDriver::LetGo(player, target);
        NoteBystander(player, rt, target, false);
        return Pull::Held;
    }
    // A rest is due and there is no place for it (in the water, no dry ground in reach): no fight until its health and
    // mana are back above the leader's thresholds where it stands (OrderService::StepRest). Mahiri, a level-21 shaman,
    // pulled two naga of the Zoram Strand at 4 % mana with its rest due and no dry ground, and died, round 242.
    if (getMSTime() < rt.restNoPlaceUntil && target->GetVictim() != player && !target->IsInCombatWith(player))
    {
        BotDriver::LetGo(player, target);
        uint32 const nowMs = getMSTime();
        if (!rt.restNoPlaceNoteAt || getMSTimeDiff(rt.restNoPlaceNoteAt, nowMs) >= 30 * IN_MILLISECONDS)
        {
            rt.restNoPlaceNoteAt = nowMs;
            LOG_INFO(ActivityLog, "activity: {} does not pull {} (level {}): it needs a rest (health {:.0f}%, mana {:.0f}%) and has no place "
                "to take one here; it waits for its health and mana where it stands", rt.name, target->GetName(), uint32(target->GetLevel()),
                player->GetHealthPct(), UsesMana(player) ? float(player->GetPower(POWER_MANA)) * 100.0f / float(player->GetMaxPower(POWER_MANA)) : 100.0f);
        }
        return Pull::Held;
    }
    // Between two tasks it starts no fight (a task that ended for a pack on its way waits where the bot stands before it
    // is tried again): a shaman stood at the edge of the Twilight camp of Silithus for the 25 seconds its hand-in waited,
    // pulled an Avenger and a Geolord at 59 and 39 % mana, was attacked twice while it rested and died, round 152.
    if (rt.task.kind == TaskKind::None && target->GetVictim() != player && !target->IsInCombatWith(player))
    {
        BotDriver::LetGo(player, target);
        return Pull::Held;
    }
    // A creature the module ended a fight with because it did not die (TrackFight): left alone for a while, unless it
    // is on the bot (then the bot walks away from it, as the module started it to).
    if (auto undying = rt.undyingFoes.find(target->GetGUID()); undying != rt.undyingFoes.end())
    {
        if (getMSTime() >= undying->second)
            rt.undyingFoes.erase(undying);
        else if (target->GetVictim() != player)
        {
            // (LetGo, not DropTarget: that one stops every movement, and mod-playerbots picks the creature again at
            // every look while it is in sight. A warrior whose AI kept picking a Dust Stormer 20 yd under the sand of
            // Silithus, a creature the pull check had left alone as one no way leads to, stood 270 s with its walk
            // stopped four times a second, 2026-11-26.)
            BotDriver::LetGo(player, target);
            return Pull::Held;
        }
    }
    // A melee fighter's target well above or below it with no way on the ground to it (the path finder gives a straight
    // line or nothing): no pull. Shamanheal's AI pulled a Noxious Plaguebat hanging on the rock beside Light's Hope, 8
    // yd above the rock's foot, fought it for minutes with every step towards it refused, and its chase got it onto the
    // rock's slope, round 221. The creature is left alone as one that does not die (15 minutes); a ranged fighter may
    // shoot it from where it stands. (Once per target: a path search.)
    constexpr float NoWayHeight = 4.0f;
    // (Not a creature that flies: it comes down to whoever pulls it.)
    if (rt.reachLooked != target->GetGUID() && BotDriver::FightsInMelee(player) && !IsSwimming(player) && !target->CanFly() &&
        std::fabs(target->GetPositionZ() - player->GetPositionZ()) >= NoWayHeight)
    {
        PathGenerator path(player);
        bool const found = path.CalculatePath(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), false);
        G3D::Vector3 const end = path.GetPath().empty() ? G3D::Vector3(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ())
            : path.GetPath().back();
        float const endDz = std::fabs(end.z - target->GetPositionZ());
        float const endDx = std::sqrt((end.x - target->GetPositionX()) * (end.x - target->GetPositionX()) +
            (end.y - target->GetPositionY()) * (end.y - target->GetPositionY()));
        bool const noWay = !found || (path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH)) || endDz > 3.0f || endDx > 5.0f;
        if (noWay)
        {
            constexpr uint32 AsideMs = 15 * MINUTE * IN_MILLISECONDS;
            rt.undyingFoes[target->GetGUID()] = getMSTime() + AsideMs;
            LOG_INFO(ActivityLog, "activity: {} does not pull {} (level {}): it stands {:.0f} yd {} it and no way on the ground leads to it "
                "({}); left alone for {} min", rt.name, target->GetName(), uint32(target->GetLevel()),
                std::fabs(target->GetPositionZ() - player->GetPositionZ()), target->GetPositionZ() > player->GetPositionZ() ? "above" : "below",
                DescribePathType(path.GetPathType()), AsideMs / (MINUTE * IN_MILLISECONDS));
            BotDriver::LetGo(player, target);
            return Pull::Held;
        }
        rt.reachLooked = target->GetGUID();
    }
    if (OffItsWork(player, rt, target))
    {
        // (Not DropTarget: it stops every movement, and this runs four times a second while mod-playerbots picks the
        // creature again at every look; the walk to the bot's work went on not at all. BotDriver::LetGo.)
        BotDriver::LetGo(player, target);
        // One line per creature in ten minutes: mod-playerbots picks the same creature again at every look while the
        // bot does something else beside it (a druid crafting at an object's spot named the same Steeljaw Snapper
        // seven times in three minutes).
        constexpr uint32 NoteAgainMs = 10 * MINUTE * IN_MILLISECONDS;
        static std::unordered_map<ObjectGuid, std::unordered_map<ObjectGuid, uint32>> noted;
        std::unordered_map<ObjectGuid, uint32>& mine = noted[player->GetGUID()];
        uint32 const nowMs = getMSTime();
        if (mine.size() > 64)
            for (auto it = mine.begin(); it != mine.end();)
                it = getMSTimeDiff(it->second, nowMs) >= NoteAgainMs ? mine.erase(it) : std::next(it);
        auto known = mine.find(target->GetGUID());
        if (known == mine.end() || getMSTimeDiff(known->second, nowMs) >= NoteAgainMs)
        {
            mine[target->GetGUID()] = nowMs;
            LOG_INFO(ActivityLog, "activity: {} leaves {} (level {}, {:.0f} yd) alone on its way: nothing its work wants, and it does "
                "not stand in its way", rt.name, target->GetName(), uint32(target->GetLevel()), player->GetExactDist(target));
        }
        return Pull::Held;
    }
    // A target that passed is looked at again every two seconds (something may walk up to it), not at every call.
    constexpr uint32 RecheckMs = 2 * IN_MILLISECONDS;
    if (target->GetGUID() == rt.pullOkTarget && getMSTimeDiff(rt.pullOkAt, getMSTime()) < RecheckMs &&
        (player->isMoving() || player->GetExactDist(target) <= player->GetMeleeRange(target) + 5.0f))
        return Pull::Fine;

    // Who comes with the target (JudgePull), for the way the bot would fight it: at the target's place in melee, at
    // its own for a caster, or, with company there, by a careful pull to the bot's side (PlanPull).
    uint32 const now = getMSTime();
    bool const careful = sBotSettings.IsOn(player, Setting::PullCareful);
    PullPlan plan;
    bool planned = false;
    PullVerdict const verdict = BestVerdict(player, rt, target, careful, plan, planned);
    // (A target that passed is not looked at again for two seconds; for a bot that does not get to it, it is.)
    std::vector<Creature*> const& foes = verdict.foes;
    float const need = verdict.need;
    float const limit = verdict.limit;
    float const budget = verdict.budget;
    uint32 const together = verdict.together;

    // A single ordinary creature less than ThreatLevels above the bot is pulled, whatever the estimate says: no
    // refusal and no rest first for it (the owner, 2026-11-14: a detour or a refusal only for elites, for creatures four or
    // more levels above the bot and for groups it cannot take). The rest between fights keeps its own thresholds.
    bool const single = verdict.sure <= 1 && !verdict.strong && !target->isElite() &&
        target->GetLevel() < player->GetLevel() + ThreatLevels;
    // And the target itself four or more levels above the bot (by its level, an elite by its health and damage) is not
    // pulled, whatever the estimate says: the spot verdict leaves such targets alone (Risk.MaxFoeLevelAbove), and the AI
    // turns to them all the same where they stand among the quest's targets. Rhazko (31, shaman) pulled a level-35
    // Sparkleshell Snapper among the tortoises of "Hardened Shells" with "1 expected" by the estimate, a Crystalhide and a
    // Reaver came, and it died, round 257.
    bool const farAbove = StrengthOf(target) >= uint32(player->GetLevel()) + ThreatLevels;

    // One of the kind that stands alone comes first (the owner, 2026-11-13: [quote left out]).
    if (careful && (verdict.sure >= 2 || (!verdict.fine && !single)) && now >= rt.nextPullSwitch)
    {
        rt.nextPullSwitch = now + PullSwitchLookMs;
        if (Creature* single = SingleOfKind(player, rt, target, verdict))
            if (BotDriver::Attack(player, single))
            {
                LOG_INFO(ActivityLog, "activity: {} turns to a {} that stands alone ({:.0f} yd) instead of the one with {} beside it ({})",
                    rt.name, single->GetName(), player->GetDistance(single), foes.size() - 1, verdict.joiners);
                return Pull::Held;
            }
    }

    if ((verdict.fine || single) && !farAbove)
    {
        // A melee bot that was let at a target and has not got under way to it for a few seconds is pulled to it:
        // its opener from range when it has one (a paladin's Exorcism), else a walk into the creature's sight. Never
        // standing still with a target: a level-41 paladin stood 205 s with "You are too far away!" at a creature 42
        // yards off that a quest interaction set as its target once a second (mod-playerbots' approach did not run
        // between the calls).
        constexpr uint32 StallMs = 3 * IN_MILLISECONDS;
        if (target->GetGUID() != rt.pullOkTarget || !rt.pullOkSince || getMSTimeDiff(rt.pullOkAt, now) > StallMs)
            rt.pullOkSince = now;
        else if (careful && !planned && getMSTimeDiff(rt.pullOkSince, now) >= StallMs && BotDriver::FightsInMelee(player) &&
            !player->isMoving() && !rt.rest.active && !player->IsNonMeleeSpellCast(false) &&
            player->GetExactDist(target) > player->GetMeleeRange(target) + 5.0f)
        {
            PullPlan push;
            // A creature that knocks back is not pulled to a place with a drop behind the bot: the blow throws it over the
            // edge (a death knight pulled a Stitched Horror with Death Grip to the rim of the plateau above the Infectis Scar
            // and was knocked into the hollow below, round 272). It walks up to the creature, and the fight is at its place.
            if (sWorldIndex.KnocksBack(target->GetEntry()) &&
                DropBehind(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), target->GetPositionX(),
                    target->GetPositionY()))
            {
                float x, y, z;
                target->GetNearPoint(player, x, y, z, player->GetCombatReach(), 1.0f, target->GetAngle(player));
                player->GetMotionMaster()->MovePoint(0, x, y, z);
                BotDriver::PauseAI(player, 2 * IN_MILLISECONDS);
                LOG_INFO(ActivityLog, "activity: {} has not got under way to {} (level {}, {:.0f} yd) for {} s: it walks up to it (it "
                    "knocks back, and the ground drops behind the bot)", rt.name, target->GetName(), uint32(target->GetLevel()),
                    player->GetExactDist(target), getMSTimeDiff(rt.pullOkSince, now) / IN_MILLISECONDS);
                rt.pullOkSince = now;
            }
            else if (PlanPull(player, target, push))
            {
                LOG_INFO(ActivityLog, "activity: {} has not got under way to {} (level {}, {:.0f} yd) for {} s: it pulls it",
                    rt.name, target->GetName(), uint32(target->GetLevel()), player->GetExactDist(target),
                    getMSTimeDiff(rt.pullOkSince, now) / IN_MILLISECONDS);
                plan = push;
                planned = true;
            }
        }
        // For the fight line: this fight was the bot's own pull, and so many foes were to be expected.
        rt.pullOkTarget = target->GetGUID();
        rt.pullOkAt = getMSTime();
        rt.pullOkFoes = uint8(std::min<uint32>(verdict.sure, 255));
        rt.pullOkMaybe = uint8(std::min<std::size_t>(foes.size() - verdict.sure, 255));
        rt.pullOkX = target->GetPositionX();
        rt.pullOkY = target->GetPositionY();
        if (planned)
        {
            StartPull(player, rt, target, plan, verdict, false);
            return Pull::Fine;
        }
        // A warlock's pet goes first. mod-playerbots never sends a pet at a target by itself (its "pet attack" is a
        // chat command); the pet only joins once its master is hit, so the creature was on the warlock from the
        // first cast and the pet stood at full health through every fight. It is sent now, and the warlock gives
        // it the time to get there before its own first cast. (A hunter's pet goes with its first shot already.)
        constexpr uint32 PetLeadMaxMs = 4 * IN_MILLISECONDS;
        if (player->getClass() == CLASS_WARLOCK && rt.petLeadTarget != target->GetGUID())
            if (Guardian* pet = player->GetGuardianPet(); pet && pet->IsAlive() && !pet->HasReactState(REACT_PASSIVE) &&
                pet->GetVictim() != target && player->IsValidAttackTarget(target))
            {
                rt.petLeadTarget = target->GetGUID();
                pet->ClearUnitState(UNIT_STATE_FOLLOW);
                pet->AttackStop();
                pet->SetTarget(target->GetGUID());
                if (CharmInfo* charm = pet->GetCharmInfo())
                {
                    charm->SetIsCommandAttack(true);
                    charm->SetIsAtStay(false);
                    charm->SetIsFollowing(false);
                    charm->SetIsCommandFollow(false);
                    charm->SetIsReturning(false);
                }
                pet->AI()->AttackStart(target);
                float const speed = std::max(pet->GetSpeed(MOVE_RUN), 1.0f);
                BotDriver::PauseAI(player, std::min<uint32>(PetLeadMaxMs, uint32(pet->GetExactDist(target) / speed * float(IN_MILLISECONDS)) + 700));
            }
        return Pull::Fine;
    }

    BotDriver::DropTarget(player);
    bool const note = rt.pullNoteTarget != target->GetGUID() || getMSTimeDiff(rt.pullNoteAt, now) >= NoteGapMs;
    rt.pullNoteTarget = target->GetGUID();
    rt.pullNoteAt = now;
    // A fight the rested bot could afford: a rest is the answer, not another target. For one foe from the start; for
    // a target with company too since the bots drink only below the leader's mana threshold (before, mod-playerbots
    // topped them up after every fight): a hunter at 82 % was refused a pull that would cost 73 % (limit 80 %) and
    // its task ended; in that round the old nine lost ten times as much task time to refused pulls as before.
    // "Rested" is what a rest gives by the leader's rules (the owner, 2026-11-14: [quote left out]): the rest for a pull drinks, and it
    // eats only below his health threshold, as every rest. A pull that such a rest does not make affordable is not
    // waited for (below).
    float const afterRest = budget - OwnBudget(player) + RestedOwnBudget(player);
    (void)together;
    if (need <= limit * afterRest && afterRest > budget + 0.03f && !verdict.strong && !farAbove)
    {
        rt.restWant = std::min(0.98f, need / limit);
        rt.restWantUntil = now + RestWantMs;
        if (note)
            LOG_INFO(ActivityLog, "activity: {} waits with {} (level {}): {}the fight would cost it about {:.0f}% and it has {:.0f}%: "
                "it rests first", rt.name, target->GetName(), uint32(target->GetLevel()),
                foes.size() > 1 ? Acore::StringFormat("{} more would join ({}); ", foes.size() - 1, verdict.joiners) : std::string(),
                need * 100.0f, budget * 100.0f);
        return Pull::Rest;
    }
    // Whether one more guild character beside it would make this pull affordable: its budget adds up with the bot's
    // (Budget counts squad and quest partners), and a creature the bot cannot beat stays a creature it cannot beat.
    // Then the quest the pull was for can wait for a quest partner (OrderService::StepPullCheck).
    rt.pullPartnerHelps = !verdict.strong && !farAbove && need <= limit * (afterRest + PullPartnerShare * RestedOwnBudget(player));
    x = target->GetPositionX();
    y = target->GetPositionY();
    if (!note)
        return Pull::Held;
    if (farAbove)
    {
        LOG_INFO(ActivityLog, "activity: {} does not pull {} (level {}{}): it is {} or more levels above it", rt.name, target->GetName(),
            uint32(target->GetLevel()), target->isElite() ? " elite" : "", uint32(ThreatLevels));
        return Pull::Refused;
    }
    LOG_INFO(ActivityLog, "activity: {} does not pull {} (level {}): {}the fight would cost it about {:.0f}% {}and it has {:.0f}% "
        "(limit {:.0f}%)", rt.name, target->GetName(), uint32(target->GetLevel()),
        foes.size() > 1 ? Acore::StringFormat("{} more would join ({}{}); ", foes.size() - 1, verdict.joiners, foes.size() > 7 ? ", ..." : "") :
            std::string(), need * 100.0f, verdict.noReserve ? "with one more of them (three or more come for certain) " : "",
        budget * 100.0f, limit * 100.0f);
    return Pull::Refused;
}

bool Escape::BeginPull(Player* player, BotRuntime& rt, Creature* target, float const* fightAt)
{
    if (!player || !target || rt.pull.active || rt.escape.active || !player->IsAlive() || player->IsInCombat() || !target->IsAlive() ||
        target->IsInCombat() || !player->IsValidAttackTarget(target))
        return false;
    PullPlan plan;
    if (!PlanPull(player, target, plan))
        return false;
    if (fightAt)
    {
        plan.fightX = fightAt[0];
        plan.fightY = fightAt[1];
        plan.fightZ = fightAt[2];
    }
    PullVerdict const here = JudgePull(player, rt, target, plan.pullX, plan.pullY);
    rt.pullOkX = target->GetPositionX();
    rt.pullOkY = target->GetPositionY();
    StartPull(player, rt, target, plan, here, true);
    return true;
}

void Escape::CancelPull(Player* player, BotRuntime& rt, char const* why)
{
    if (!rt.pull.active)
        return;
    Creature* target = player ? ObjectAccessor::GetCreature(*player, rt.pull.target) : nullptr;
    if (!player)
    {
        rt.pull = BotRuntime::CarefulPull();
        return;
    }
    EndPull(player, rt, target, why ? why : "called off", false, false);
}

namespace
{
    // A place that makes a creature that stands and casts come after the bot: out of its line of sight within 26 yards
    // of the bot (`outOfRange` false), or out of its spells' reach, 44 yards from it on the bot's side (true). Not
    // nearer to it than the bot stands, not past or into the reach of other hostile creatures, not into deep water,
    // with a whole path. The nearest such place.
    bool FindLureSpot(Player* player, Creature* target, bool outOfRange, float& x, float& y, float& z)
    {
        constexpr float Pi = 3.14159265f;
        constexpr float RangeYards = 44.0f;
        constexpr float DeepWater = 1.5f;
        std::vector<Creature*> others = LiveHostiles(player, 60.0f, PackLevelsBelow);
        others.erase(std::remove(others.begin(), others.end(), target), others.end());
        float const here = player->GetExactDist2d(target);
        float best = -1.0f;
        auto consider = [&](float px, float py)
        {
            float pz = player->GetPositionZ();
            player->UpdateAllowedPositionZ(px, py, pz);
            float const straight = player->GetExactDist2d(px, py);
            if ((best >= 0.0f && straight >= best) || target->GetExactDist2d(px, py) < here - 2.0f)
                return;
            if (!outOfRange && target->IsWithinLOS(px, py, pz + 1.5f))
                return;
            if (Escape::StepWakes(player, px, py) || Escape::WaterDepth(player, px, py, pz) > DeepWater)
                return;
            float cx = px, cy = py, cz = pz;
            if (!CanWalkTo(player, cx, cy, cz, std::max(straight, 5.0f), &others))
                return;
            best = straight;
            x = cx;
            y = cy;
            z = cz;
        };
        if (outOfRange)
        {
            float const away = target->GetAngle(player);
            for (int i = 0; i <= 6; ++i)
                for (int sign : { 1, -1 })
                    if (i || sign > 0)
                    {
                        float const a = away + float(sign * i) * Pi / 12.0f;
                        consider(target->GetPositionX() + std::cos(a) * RangeYards, target->GetPositionY() + std::sin(a) * RangeYards);
                    }
        }
        else
            for (float radius : { 8.0f, 14.0f, 20.0f, 26.0f })
                for (int i = 0; i < 16; ++i)
                {
                    float const a = 2.0f * Pi * float(i) / 16.0f;
                    consider(player->GetPositionX() + std::cos(a) * radius, player->GetPositionY() + std::sin(a) * radius);
                }
        return best >= 0.0f;
    }
}

bool Escape::StepPull(Player* player, BotRuntime& rt)
{
    BotRuntime::CarefulPull& pull = rt.pull;
    if (!pull.active)
        return false;
    if (!player->IsAlive() || player->GetMapId() != pull.mapId)
    {
        pull = BotRuntime::CarefulPull();
        BotDriver::PauseAI(player, 0);
        return false;
    }
    Creature* target = ObjectAccessor::GetCreature(*player, pull.target);
    if (rt.escape.active)
    {
        EndPull(player, rt, target, "called off (it runs)", false, false);
        return false;
    }
    if (!target || !target->IsAlive() || target->IsInEvadeMode())
    {
        EndPull(player, rt, target, "the creature is gone", false, false);
        return false;
    }
    bool const onIt = target->IsInCombat() && (target->GetVictim() == player || target->IsInCombatWith(player));
    if (target->IsInCombat() && !onIt)
    {
        EndPull(player, rt, target, "called off (it fights someone else)", false, true);
        return false;
    }
    // On its way back or while it waits the creature has turned to somebody else (a caster that stands and casts at
    // the healer): the fight is there now, and the bot goes to it. (Wartank, the tank of the owner's Dire Maul West run,
    // taunted a Petrified Treant to a camp 65 yd back; the Treant stood and cast at the priest, and the group stood
    // 16 s "too far away" before the tank went to it, round 257.)
    if (onIt && (pull.phase == PullBack || pull.phase == PullWait))
        if (Unit const* victim = target->GetVictim(); victim && victim != player && victim->GetOwnerGUID() != player->GetGUID())
        {
            EndPull(player, rt, target, Acore::StringFormat("the creature turned to {}, the bot goes to it", victim->GetName()).c_str(),
                true, true);
            return false;
        }
    // Something else is on the bot: the fight is here and now, mod-playerbots has it.
    for (Unit const* attacker : player->getAttackers())
        if (attacker != target && attacker->IsAlive())
        {
            EndPull(player, rt, target, "called off (something else attacked it)", onIt, true);
            return false;
        }

    uint32 const now = getMSTime();
    BotDriver::PauseAI(player, PullPauseMs);
    rt.pullOkAt = now; // the fight that follows is this pull's
    float const dist = player->GetExactDist(target);
    auto phase = [&](PullPhase next)
    {
        pull.phase = next;
        pull.phaseAt = now;
        pull.nextAct = 0;
    };
    auto moveTo = [&](float px, float py, float pz)
    {
        player->GetMotionMaster()->Clear();
        player->GetMotionMaster()->MovePoint(0, px, py, pz, FORCED_MOVEMENT_RUN);
    };
    auto toFight = [&]()
    {
        // Back to where the fight shall be, when that is not where the bot stands.
        if (player->GetExactDist2d(pull.fightX, pull.fightY) > PullArriveYards)
        {
            player->InterruptNonMeleeSpells(false);
            moveTo(pull.fightX, pull.fightY, pull.fightZ);
            phase(PullBack);
        }
        else
        {
            pull.waitFrom = dist;
            phase(PullWait);
        }
    };
    uint32 const inPhase = getMSTimeDiff(pull.phaseAt, now);

    // Out of the saddle before the opener and before the step into the creature's sight: a rider's cast fails
    // (SPELL_FAILED_NOT_MOUNTED). In one live round 6 of 21 ranged pulls ended "could not open on ... (the last cast
    // result 64)", all of bots that had ridden up: four seconds lost each, and a walk into the creature's sight.
    if (player->IsMounted() && !player->IsInFlight() &&
        (pull.phase == PullCast || pull.phase == PullBack || pull.phase == PullWait ||
         (pull.phase == PullWalkIn && dist <= target->GetAggroRange(player) + 12.0f)))
    {
        player->Dismount();
        player->RemoveAurasByType(SPELL_AURA_MOUNTED);
        if (GetSettings().debug)
            LOG_INFO(ActivityLog, "activity: {} gets off its mount for its pull of {}", rt.name, target->GetName());
    }

    switch (pull.phase)
    {
        case PullToPlace:
            if (onIt)
            {
                toFight();
                break;
            }
            if (player->GetExactDist2d(pull.pullX, pull.pullY) <= PullArriveYards)
            {
                player->StopMoving();
                phase(PullCast);
                break;
            }
            if (inPhase > PullToPlaceMs)
            {
                EndPull(player, rt, target, "called off (it did not get to its place)", false, true);
                return false;
            }
            if (!player->isMoving() && now >= pull.nextAct)
            {
                moveTo(pull.pullX, pull.pullY, pull.pullZ);
                pull.nextAct = now + IN_MILLISECONDS;
            }
            break;

        case PullCast:
            if (onIt)
            {
                player->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
                toFight();
                break;
            }
            // An opener with a cast time at a creature that walks about: cast when it stands, up to 3 s of waiting; a cast
            // under way whose creature has walked out of its range is broken off, and the next try steps nearer (a
            // Sprite Darter flew from 23 to 30 yd while Mojowind cast its Lightning Bolt, the bolt failed at its end
            // without a fight, and the shaman walked up to it instead, 2026-11-27; Ainuha had died that way an hour before).
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(pull.spell);
                info && info->CalcCastTime(player) > 0 && !(info->RangeEntry && (info->RangeEntry->Flags & SPELL_RANGE_RANGED)))
            {
                constexpr uint32 PullMoveWaitMs = 3 * IN_MILLISECONDS;
                float const reach = player->GetSpellMaxRangeForTarget(target, info) + player->GetCombatReach() + target->GetCombatReach();
                Spell const* current = player->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                if (current && current->m_spellInfo->Id == pull.spell && current->getState() == SPELL_STATE_PREPARING &&
                    player->GetExactDist(target) > reach - 0.5f)
                {
                    player->InterruptSpell(CURRENT_GENERIC_SPELL, false);
                    pull.nextAct = now;
                    if (GetSettings().debug)
                        LOG_INFO(ActivityLog, "activity: {} breaks off its opener on {}: the creature walked out of its range ({:.0f} yd, it "
                            "reaches {:.0f})", rt.name, target->GetName(), player->GetExactDist(target), reach);
                    break;
                }
                // (Each walk of the creature is waited out, 3 s at most, for 20 s after the pull began: a paladin cast
                // its Exorcism ten times at a scorpid that wandered off and broke it off each time, 2026-11-27.)
                if (!target->isMoving())
                    pull.moveWaitSince = 0;
                else if (!player->IsNonMeleeSpellCast(false) && getMSTimeDiff(pull.startedAt, now) < 20 * IN_MILLISECONDS)
                {
                    if (!pull.moveWaitSince)
                        pull.moveWaitSince = now;
                    if (now - pull.moveWaitSince < PullMoveWaitMs)
                    {
                        pull.phaseAt = now; // the opener's time starts when it casts
                        pull.nextAct = now + 250;
                        break;
                    }
                }
            }
            // Bladestorm (and any aura that allows only abilities) makes the core refuse every other spell with
            // "spell in progress" until it ends: the opener waits for it, 10 s after the pull began at most (Again's
            // Throw failed 7 times in the last 4 s of its Bladestorm, 2026-11-27, and walked into the bat's sight).
            if (player->HasPlayerFlag(PLAYER_ALLOW_ONLY_ABILITY) && getMSTimeDiff(pull.startedAt, now) < 10 * IN_MILLISECONDS)
            {
                pull.phaseAt = now;
                pull.nextAct = now + 250;
                break;
            }
            if (inPhase > PullCastMs)
            {
                // The opener did not go out (line of sight, a stance, the range): into its sight instead, when the
                // creature attacks on sight; else the bot simply goes to it.
                SpellInfo const* info = sSpellMgr->GetSpellInfo(pull.spell);
                LOG_INFO(ActivityLog, "activity: {} could not open on {} with {} ({} tries, the last cast result {}): {}", rt.name,
                    target->GetName(), info ? info->SpellName[0] : "its opener", uint32(pull.casts), uint32(pull.castResult),
                    target->IsHostileTo(player) ? "it walks into its sight instead" : "it goes to it");
                if (!target->IsHostileTo(player))
                {
                    EndPull(player, rt, target, "the opener did not go out", true, true);
                    return false;
                }
                pull.ranged = false;
                phase(PullWalkIn);
                break;
            }
            if (!player->IsNonMeleeSpellCast(false) && now >= pull.nextAct)
            {
                // What ended the opener before it went out (Ainuha's Lightning Bolt on a Sprite Darter went out four
                // times in 4 s with the result OK and never landed, round of 03:22 on 2026-11-27).
                SpellInfo const* opener = sSpellMgr->GetSpellInfo(pull.spell);
                if (GetSettings().debug && pull.casts && pull.castResult == SPELL_CAST_OK && opener && opener->CalcCastTime(player) > 0 &&
                    !(opener->RangeEntry && (opener->RangeEntry->Flags & SPELL_RANGE_RANGED))) // (an instant or a shot: its projectile may be in flight)
                    LOG_INFO(ActivityLog, "activity: {} opener on {} ended without a fight {} ms after its cast: moving {} (movement flags "
                        "0x{:X}, motion {}, spline {}), {:.1f} yd, line of sight {}, the creature moving {}", rt.name, target->GetName(),
                        getMSTimeDiff(pull.lastCastAt, now), player->isMoving() ? "yes" : "no", player->GetUnitMovementFlags(),
                        uint32(player->GetMotionMaster()->GetCurrentMovementGeneratorType()),
                        player->movespline->Finalized() ? "ended" : "running", dist, player->IsWithinLOSInMap(target) ? "yes" : "no",
                        target->isMoving() ? "yes" : "no");
                // The core's own cast (its result says why an opener does not go out).
                player->StopMoving();
                // A bot sends no movement opcodes: a flag of a direction or a fall can stay after its last spline has
                // ended, and the core then cancels an opener with a cast time at its first update as the cast of a
                // caster that moves (Spell::update). The flags go with no spline under way: a bot moves on splines only.
                if (player->movespline->Finalized() && player->isMoving())
                {
                    uint32 const stale = player->GetUnitMovementFlags() & MOVEMENTFLAG_MASK_MOVING;
                    player->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
                    if (!pull.staleCleared)
                    {
                        pull.staleCleared = true;
                        LOG_INFO(ActivityLog, "activity: {} still had the movement flags 0x{:X} of a move that has ended: cleared before "
                            "its opener on {}", rt.name, stale, target->GetName());
                    }
                }
                player->SetInFront(target);
                player->SetTarget(target->GetGUID());
                pull.castResult = uint16(player->CastSpell(target, pull.spell, false));
                pull.lastCastAt = now;
                if (pull.casts < 255)
                    ++pull.casts;
                pull.nextAct = now + 500;
                // Out of range by the core's measure: it measures in three dimensions (a creature up or down a slope
                // is further than the map says) and the creature may have walked on since the place was chosen.
                // Every failed opener of one evening that named the range ended as a walk into the pack's sight.
                // A few yards nearer, twice at most, while that stays outside the creature's sight.
                // (A spell with a cast time steps 7 yd inside its range, as its place was chosen: a creature that
                // walks on is still in reach when the cast ends.)
                if (pull.castResult == SPELL_FAILED_OUT_OF_RANGE && pull.steps < 2)
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(pull.spell))
                    {
                        bool const castTime = info->CalcCastTime(player) > 0 && !(info->RangeEntry && (info->RangeEntry->Flags & SPELL_RANGE_RANGED));
                        float const want = player->GetSpellMaxRangeForTarget(target, info) + player->GetCombatReach() +
                            target->GetCombatReach() - (castTime ? 7.0f : 3.0f);
                        float const dz = target->GetPositionZ() - player->GetPositionZ();
                        float const flat = want * want > dz * dz ? std::sqrt(want * want - dz * dz) - 0.5f : 0.0f;
                        if (flat >= target->GetAggroRange(player) + 3.0f && flat < player->GetExactDist2d(target))
                        {
                            float const angle = target->GetAngle(player);
                            float nx = target->GetPositionX() + std::cos(angle) * flat;
                            float ny = target->GetPositionY() + std::sin(angle) * flat;
                            float nz = player->GetPositionZ();
                            player->UpdateAllowedPositionZ(nx, ny, nz);
                            pull.pullX = pull.fightX = nx;
                            pull.pullY = pull.fightY = ny;
                            pull.pullZ = pull.fightZ = nz;
                            ++pull.steps;
                            pull.phaseAt = now; // the opener's time starts anew at the new place
                            pull.nextAct = now + 1200;
                            moveTo(nx, ny, nz);
                            if (GetSettings().debug)
                                LOG_INFO(ActivityLog, "activity: {} steps nearer for its opener on {}: out of range at {:.0f} yd ({:.0f} yd "
                                    "up or down), to {:.0f} yd", rt.name, target->GetName(), dist, std::fabs(dz), flat);
                        }
                    }
                // No line of sight from where it stands (the creature walked on behind a rock, or the place was chosen
                // where the old measure saw over the rock): a place that has it, chosen anew from where the creature is
                // now, twice at most; the walk into its sight only when there is none.
                if (pull.castResult == SPELL_FAILED_LINE_OF_SIGHT && pull.steps < 2)
                {
                    PullPlan again;
                    bool found = false;
                    float const toBot = target->GetAngle(player);
                    for (float turn : { 0.0f, 0.45f, -0.45f, 0.9f, -0.9f })
                    {
                        float const angle = Position::NormalizeOrientation(toBot + turn);
                        if (PlanPull(player, target, again, turn == 0.0f ? nullptr : &angle) && again.ranged &&
                            player->GetExactDist2d(again.pullX, again.pullY) > PullArriveYards)
                        {
                            found = true;
                            break;
                        }
                    }
                    if (found)
                    {
                        pull.spell = again.spell;
                        pull.pullX = again.pullX;
                        pull.pullY = again.pullY;
                        pull.pullZ = again.pullZ;
                        pull.fightX = again.fightX;
                        pull.fightY = again.fightY;
                        pull.fightZ = again.fightZ;
                        ++pull.steps;
                        phase(PullToPlace);
                        moveTo(again.pullX, again.pullY, again.pullZ);
                        LOG_INFO(ActivityLog, "activity: {} has no line of sight to {} from where it stands: it pulls from ({:.0f}, {:.0f}) "
                            "instead, {:.0f} yd from it", rt.name, target->GetName(), again.pullX, again.pullY, again.fromYards);
                    }
                }
            }
            break;

        case PullWalkIn:
        {
            if (onIt || player->IsInCombat())
            {
                toFight();
                break;
            }
            // It does not attack on sight after all (three seconds well inside its sight): the fight is at its place.
            bool const inSight = dist <= std::max(target->GetAggroRange(player) - 1.0f, 3.0f);
            if (inSight && !pull.inSightSince)
                pull.inSightSince = now;
            if (inPhase > PullWalkInMs || dist <= 4.0f || (inSight && pull.inSightSince && getMSTimeDiff(pull.inSightSince, now) > 3 * IN_MILLISECONDS))
            {
                EndPull(player, rt, target, "the creature did not come for it", true, true);
                return false;
            }
            if (now >= pull.nextAct)
            {
                // Towards the creature as it stands now, to just inside its sight.
                float const aggro = std::max(target->GetAggroRange(player) - 1.5f, 2.0f);
                float const angle = target->GetAngle(player);
                float px = target->GetPositionX() + std::cos(angle) * aggro;
                float py = target->GetPositionY() + std::sin(angle) * aggro;
                float pz = target->GetPositionZ();
                player->UpdateAllowedPositionZ(px, py, pz);
                moveTo(px, py, pz);
                pull.nextAct = now + 700;
            }
            break;
        }

        case PullBack:
            if (player->GetExactDist2d(pull.fightX, pull.fightY) <= PullArriveYards || inPhase > PullBackMs)
            {
                player->StopMoving();
                pull.waitFrom = dist;
                phase(PullWait);
                break;
            }
            if (!player->isMoving() && now >= pull.nextAct)
            {
                moveTo(pull.fightX, pull.fightY, pull.fightZ);
                pull.nextAct = now + 700;
            }
            break;

        case PullWait:
        {
            player->SetFacingToObject(target);
            if (player->IsWithinMeleeRange(target) || dist <= 6.0f)
            {
                EndPull(player, rt, target, "the creature came to it", true, false);
                return false;
            }
            // It stays where it is: a creature that casts or shoots. The fight is at its place then, when that brings
            // no group the bot cannot take. Else it is made to come: the bot steps out of its sight, then out of its
            // spells' reach (a level-44 warrior pulled a Skullsplitter Mystic with Throw, the Mystic stood and cast,
            // the warrior went to it and had its three neighbours on it: dead in 35 seconds). A creature that comes
            // for neither is left alone: the bot walks away from it.
            uint32 const standStill = pull.lures ? PullLureStandStillMs : PullStandStillMs;
            if (inPhase > PullWaitMs + (pull.lures ? PullLureStandStillMs : 0) || (inPhase > standStill && dist > pull.waitFrom - 3.0f))
            {
                StandingCasters().insert(target->GetEntry());
                PullVerdict const there = JudgePull(player, rt, target, target->GetPositionX(), target->GetPositionY());
                if (there.fine)
                {
                    EndPull(player, rt, target, "the creature fights from a distance, the bot goes to it", true, true);
                    return false;
                }
                if (pull.lures < 2)
                {
                    float lx = 0.0f, ly = 0.0f, lz = 0.0f;
                    bool outOfRange = false;
                    bool found = !pull.lures && FindLureSpot(player, target, false, lx, ly, lz);
                    if (!found)
                    {
                        outOfRange = true;
                        found = FindLureSpot(player, target, true, lx, ly, lz);
                    }
                    if (found)
                    {
                        pull.lures = outOfRange ? 2 : 1;
                        pull.fightX = lx;
                        pull.fightY = ly;
                        pull.fightZ = lz;
                        player->InterruptNonMeleeSpells(false);
                        moveTo(lx, ly, lz);
                        phase(PullLure);
                        LOG_INFO(ActivityLog, "activity: {} pull of {}: it stands and casts among {} more; the bot steps {} ({:.0f} yd) "
                            "to make it come", rt.name, target->GetName(), there.foes.size() > 1 ? there.foes.size() - 1 : 0,
                            outOfRange ? "out of the reach of its spells" : "out of its sight", player->GetExactDist2d(lx, ly));
                        break;
                    }
                }
                EndPull(player, rt, target, "the creature fights from a distance among its group: left alone", false, true);
                BotDriver::DropTarget(player);
                if (player->IsInCombat() && player->getAttackers().size() <= 1)
                {
                    player->CombatStop(true);
                    WalkAway(player, rt, target);
                }
                return false;
            }
            break;
        }

        case PullLure:
            if (player->IsWithinMeleeRange(target) || dist <= 6.0f)
            {
                EndPull(player, rt, target, "the creature came to it", true, false);
                return false;
            }
            if (player->GetExactDist2d(pull.fightX, pull.fightY) <= PullArriveYards || inPhase > PullLureMs)
            {
                player->StopMoving();
                pull.waitFrom = dist;
                phase(PullWait);
                break;
            }
            if (!player->isMoving() && now >= pull.nextAct)
            {
                moveTo(pull.fightX, pull.fightY, pull.fightZ);
                pull.nextAct = now + 700;
            }
            break;

        default:
            EndPull(player, rt, target, "called off", false, false);
            return false;
    }
    return rt.pull.active;
}

bool Escape::BackOut(Player* player, BotRuntime& rt, char const* why)
{
    // A level-20 warrior had fought its way into a gnoll camp, called its next pull off ("3 more would join"), was
    // given a task 515 yards away, turned to walk there through the rest of the camp and had three gnolls on it four
    // seconds later; no direction was open any more, and it died.
    constexpr float BackOutYards = 28.0f;
    BotRuntime::Escape& escape = rt.escape;
    if (escape.active || !player->IsAlive() || player->IsInCombat() || !sBotSettings.IsOn(player, Setting::EscapeEnable) ||
        !CanRun(player, true))
        return false;
    Creature* nearest = nullptr;
    uint32 count = 0;
    for (Creature* creature : LiveHostiles(player, BackOutYards, PackLevelsBelow))
    {
        if (creature->IsInCombat())
            continue;
        ++count;
        if (!nearest || creature->GetExactDist(player) < nearest->GetExactDist(player))
            nearest = creature;
    }
    if (!nearest)
        return false;
    escape = BotRuntime::Escape();
    escape.active = true;
    escape.aside = true;
    escape.pack = true;
    escape.startedAt = getMSTime();
    escape.mapId = player->GetMapId();
    escape.fromX = nearest->GetPositionX();
    escape.fromY = nearest->GetPositionY();
    escape.healthStart = uint8(player->GetHealthPct());
    escape.foes = uint8(std::min<uint32>(count, 255));
    escape.foeEntry = nearest->GetEntry();
    escape.foeLevel = nearest->GetLevel();
    if (!StartLeg(player, rt, escape, { nearest }))
    {
        escape = BotRuntime::Escape();
        return false;
    }
    BotDriver::PauseAI(player, PauseMs);
    LOG_INFO(ActivityLog, "activity: {} backs out of the place {}: {} creature{} within {:.0f} yd (the nearest: {} "
        "level {}, {:.0f} yd away)", rt.name, why, count, count == 1 ? "" : "s", BackOutYards, nearest->GetName(), uint32(nearest->GetLevel()),
        player->GetDistance(nearest));
    return true;
}

bool Escape::BackOutAfterFight(Player* player, BotRuntime& rt)
{
    // Stonehorn (43, warrior) went around a Bloodsail camp of the Southern Savage Coast, was attacked by a warlock and
    // its succubus, and chased the warlock when it ran for help towards three swashbucklers and a warlock that stand
    // together; it killed it and stood 7 yd from them when its fight ended, and they came on it five seconds later (4
    // foes, "it would not live through the run"; dead, 2026-11-26). Now a fight that ends within the reach of creatures
    // the bot cannot take together sends it back the way it came before anything else.
    constexpr float AroundYards = 30.0f;
    constexpr float ReachMargin = 10.0f;
    if (rt.escape.active || !player->IsAlive() || player->IsInCombat() || player->GetMap()->IsDungeon() || player->GetTransport() ||
        !sBotSettings.IsOn(player, Setting::EscapeEnable))
        return false;
    std::vector<Creature*> pack;
    for (Creature* creature : LiveHostiles(player, AroundYards, uint8(GroupLevelsBelow + 1)))
        if (!creature->IsInCombat() && player->GetExactDist(creature) <= creature->GetAggroRange(player) + ReachMargin)
            pack.push_back(creature);
    if (pack.empty())
        return false;
    // (And who comes with them: what stands within the core's call for help of one of them.)
    constexpr float CallMarginYards = 2.0f;
    float const assistYards = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS) + CallMarginYards;
    std::size_t const inReach = pack.size();
    for (Creature* creature : LiveHostiles(player, AroundYards + assistYards, uint8(GroupLevelsBelow + 1)))
    {
        if (creature->IsInCombat() || std::find(pack.begin(), pack.end(), creature) != pack.end())
            continue;
        for (std::size_t i = 0; i < inReach; ++i)
            if (creature->GetExactDist(pack[i]) <= assistYards && Escape::WouldAssist(creature, pack[i], player))
            {
                pack.push_back(creature);
                break;
            }
    }
    if (pack.size() < 2)
        return false;
    float const need = FightNeed(player, pack, CostOf(rt, player->GetLevel()), nullptr, &rt) * CrowdTail(float(pack.size()));
    if (need <= Budget(player) * sBotSettings.GetFloat(player, Setting::RiskMaxFightCostPercent))
        return false;
    std::string const why = Acore::StringFormat("after its fight, beside {} creatures it cannot take together (the fight about {:.0f}% "
        "of its {:.0f}%)", pack.size(), need * 100.0f, Budget(player) * 100.0f);
    return BackOut(player, rt, why.c_str());
}

uint8 Escape::WaterAt(Player const* player, float x, float y, float z)
{
    z = GuildAdventure::WaterProbeZ(player->GetMap(), x, y, z);
    LiquidData const liquid = player->GetMap()->GetLiquidData(player->GetPhaseMask(), x, y, z, player->GetCollisionHeight(), {});
    if (liquid.Status == LIQUID_MAP_UNDER_WATER)
        return 2;
    return liquid.Status == LIQUID_MAP_IN_WATER ? 1 : 0;
}

float Escape::WaterDepth(Player const* player, float x, float y, float z)
{
    Map* map = player->GetMap();
    z = GuildAdventure::WaterProbeZ(map, x, y, z);
    LiquidData const liquid = map->GetLiquidData(player->GetPhaseMask(), x, y, z, player->GetCollisionHeight(), {});
    if (liquid.Status == LIQUID_MAP_NO_WATER || liquid.Level <= INVALID_HEIGHT)
        return 0.0f;
    // The floor it stands on or floats above: the highest ground at or just above its feet (a pier is ground; the
    // terrain under a pier is not).
    constexpr float FloorSearchYards = 100.0f;
    float floor = map->GetHeight(player->GetPhaseMask(), x, y, z + 1.5f, true, FloorSearchYards);
    if (floor <= INVALID_HEIGHT)
        floor = liquid.DepthLevel;
    if (floor <= INVALID_HEIGHT)
        return liquid.Status == LIQUID_MAP_IN_WATER || liquid.Status == LIQUID_MAP_UNDER_WATER ? FloorSearchYards : 0.0f;
    return std::max(0.0f, liquid.Level - floor);
}

bool Escape::TooWetToRest(Player const* player, float x, float y, float z)
{
    return WaterDepth(player, x, y, z) > RestWaterYards;
}

bool Escape::RoamerNear(uint32 mapId, float x, float y, uint8 level, std::string* who, bool* stays)
{
    uint32 const now = getMSTime();
    bool found = false;
    for (auto itr = roamerSights.begin(); itr != roamerSights.end();)
    {
        RoamerSight const& sight = itr->second;
        if (getMSTimeDiff(sight.seenAt, now) > RoamerMemoryMs)
        {
            itr = roamerSights.erase(itr);
            continue;
        }
        // (One that only swims is no matter for a point on dry land: the sharks off Theramore closed the pier.)
        if (!found && sight.mapId == mapId && sight.worth >= uint32(level) + ThreatLevels && InRoamerZone(sight, x, y, now) &&
            (!sight.swimOnly || WaterPoint(mapId, x, y)))
        {
            found = true;
            if (who)
                *who = sight.name;
            if (stays)
                *stays = sight.stays;
        }
        ++itr;
    }
    return found;
}

bool Escape::RoamerOnLeg(uint32 mapId, float fromX, float fromY, float toX, float toY, uint8 level, std::string* who, bool* stays)
{
    if (roamerSights.empty())
        return false;
    constexpr float StepYards = 15.0f;
    constexpr float LegYards = 300.0f;
    float const dx = toX - fromX;
    float const dy = toY - fromY;
    float const length = std::sqrt(dx * dx + dy * dy);
    for (float along = 0.0f; along <= std::min(length, LegYards) + 0.01f; along += StepYards)
    {
        float const share = length > 0.1f ? along / length : 0.0f;
        if (RoamerNear(mapId, fromX + dx * share, fromY + dy * share, level, who, stays))
            return true;
    }
    return false;
}

bool Escape::WalkAway(Player* player, BotRuntime& rt, Creature* from)
{
    BotRuntime::Escape& escape = rt.escape;
    if (escape.active || !from || !player->IsAlive() || player->IsInCombat() || !CanRun(player, true))
        return false;
    escape = BotRuntime::Escape();
    escape.active = true;
    escape.aside = true;
    escape.walkAway = true;
    escape.startedAt = getMSTime();
    escape.mapId = player->GetMapId();
    escape.fromX = from->GetPositionX();
    escape.fromY = from->GetPositionY();
    escape.healthStart = uint8(player->GetHealthPct());
    escape.foes = 1;
    escape.foeEntry = from->GetEntry();
    escape.foeLevel = from->GetLevel();
    rt.threatSeen = from->GetGUID();
    rt.threatGap = from->GetExactDist(player);
    rt.threatSeenAt = escape.startedAt;
    if (!StartLeg(player, rt, escape, { from }))
    {
        escape = BotRuntime::Escape();
        return false;
    }
    BotDriver::PauseAI(player, PauseMs);
    return true;
}

bool Escape::WalksLastYards(Player* player, BotRuntime& rt)
{
    constexpr float OnFootYards = 60.0f;
    constexpr float BackOnYards = 100.0f;
    constexpr float AroundYards = 40.0f;
    BotTask const& task = rt.task;
    bool const kind = task.kind == TaskKind::Objective || task.kind == TaskKind::Grind || task.kind == TaskKind::Interact ||
        task.kind == TaskKind::TurnIn || task.kind == TaskKind::Pickup || task.kind == TaskKind::Event || task.kind == TaskKind::Vendor ||
        task.kind == TaskKind::Train || task.kind == TaskKind::Discover || task.kind == TaskKind::Fish;
    if (!kind || !task.hasSpot || task.spot.mapId != player->GetMapId() || !player->IsAlive() || player->IsInFlight() ||
        player->GetTransport() || player->GetMap()->IsDungeon() || rt.squad.following || !sBotSettings.IsOn(player, Setting::PullCareful) ||
        !sBotSettings.IsOn(player, Setting::RiskEnable))
    {
        rt.onFoot = false;
        rt.onFootTask = 0;
        return false;
    }
    float const distance = player->GetDistance2d(task.spot.x, task.spot.y);
    if (distance > BackOnYards)
    {
        rt.onFoot = false;
        return false;
    }
    uint64 const key = TaskKeyOf(task);
    if (key != rt.onFootTask)
    {
        rt.onFootTask = key;
        rt.onFoot = false;
        rt.onFootHostile = !Risk::HostileSpawnsNear(player, task.spot.x, task.spot.y, AroundYards, PackLevelsBelow).empty();
    }
    bool const onFoot = rt.onFootHostile && (distance <= OnFootYards || rt.onFoot);
    if (onFoot && !rt.onFoot && player->IsMounted())
        LOG_INFO(ActivityLog, "activity: {} gets off its mount {:.0f} yd before {} and walks the rest (creatures that are no friends "
            "of it stand there)", rt.name, distance, task.targetName.empty() ? "its target" : task.targetName);
    rt.onFoot = onFoot;
    return onFoot;
}

bool Escape::FindRestSpot(Player* player, BotRuntime const& rt, float maxYards, float& x, float& y, float& z, uint32& here,
    uint32& there, bool clearOnly, std::vector<G3D::Vector3>* way)
{
    // What could attack the bot there: the creatures that are around now (a patrol, a wanderer) and the spawns
    // (a camp whose mobs are dead for the moment).
    constexpr float CampYards = 30.0f;
    constexpr float RestRoadYards = 40.0f;
    std::list<Unit*> units;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, maxYards + CampYards);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, units, check);
    Cell::VisitObjects(player, searcher, maxYards + CampYards);
    std::vector<Creature*> hostiles;
    for (Unit* unit : units)
    {
        Creature* creature = unit->ToCreature();
        if (creature && !creature->IsPet() && !creature->IsTotem() && creature->IsHostileTo(player) &&
            !creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_IMMUNE_TO_PC) &&
            creature->GetLevel() + PackLevelsBelow > player->GetLevel())
            hostiles.push_back(creature);
    }
    auto danger = [&](float px, float py)
    {
        uint32 count = Risk::HostilesNear(player, px, py, CampYards);
        for (Creature const* creature : hostiles)
        {
            float const dx = creature->GetPositionX() - px;
            float const dy = creature->GetPositionY() - py;
            if (dx * dx + dy * dy <= CampYards * CampYards)
                ++count;
        }
        return count;
    };

    // (Wet: in water too deep to sit down in. By the water's depth, not by the bot's feet: it floats at the surface.)
    bool const wet = TooWetToRest(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
    here = danger(player->GetPositionX(), player->GetPositionY());
    if (!here && !wet)
        return false;

    struct Spot
    {
        uint32 danger;
        float distance;
        float x;
        float y;
        float z;
    };
    std::vector<Spot> spots;
    // A bot in the water: its rings are looked at on the ground under them, and a bot under the surface walks from the
    // surface above it. Mahiri, a level-21 shaman, worked the naga of the Zoram Strand 12 yd under the sea at 25 % mana
    // and "found no dry ground within 150 yd": every spot of its rings lay 12 yd under the beach, where the path finder
    // finds no ground (it looks 5 yd up and down), and from under the surface it has no whole path anyway; it went on,
    // pulled two naga at 4 % mana and died, round 242.
    Map* map = player->GetMap();
    float surface[3] = { player->GetPositionX(), player->GetPositionY(), player->GetPositionZ() };
    bool fromSurface = false;
    float waterTop = player->GetPositionZ();
    if (wet)
    {
        LiquidData const liquid = map->GetLiquidData(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ() + 0.5f, player->GetCollisionHeight(), {});
        if (liquid.Level > INVALID_HEIGHT)
        {
            waterTop = std::max(waterTop, liquid.Level);
            // (A hair over the surface: from under it the path finder takes the floor below.)
            if (liquid.Level - player->GetPositionZ() > 1.5f)
            {
                surface[2] = liquid.Level + 0.3f;
                fromSurface = true;
            }
        }
    }
    auto groundUnder = [&](float sx, float sy, float sz)
    {
        if (!wet)
            return sz;
        float const ground = map->GetHeight(player->GetPhaseMask(), sx, sy, waterTop + 40.0f, true, 80.0f);
        return ground > INVALID_HEIGHT ? ground : sz;
    };
    // Worth the walk: nothing hostile there, or clearly less than here (a spot with one creature fewer is no gain); a
    // wet bot takes any dry spot.
    auto add = [&](float sx, float sy, float sz, float distance)
    {
        sz = groundUnder(sx, sy, sz);
        uint32 const count = danger(sx, sy);
        // (Not a gap in the middle of a camp: nothing stands within 30 yards of it, and everything around it. Not
        // where a strong creature is walking right now.)
        // (Nor on the road of a creature that walks a round and kills it: the Nerubian Overseer came by a level-60
        // shaman's quiet place in the Eastern Plaguelands a second after its rest, round 172.)
        if ((wet || !count || (!clearOnly && count + 2 <= here)) && !Risk::InCampItCannotTake(player, rt, player->GetMapId(), sx, sy, sz) &&
            !Escape::RoamerNear(player->GetMapId(), sx, sy, player->GetLevel()) &&
            !Risk::OnStrongRoad(player, rt, player->GetMapId(), sx, sy, sz, RestRoadYards))
            spots.push_back({ count, distance, sx, sy, sz });
    };
    // The way it came (a place it walked through unharmed a moment ago), then three rings around it.
    for (BotRuntime::TrailPoint const& point : rt.trail)
    {
        float const distance = player->GetDistance2d(point.x, point.y);
        if (point.mapId == player->GetMapId() && distance >= 20.0f && distance <= maxYards * 1.5f)
            add(point.x, point.y, point.z, distance);
    }
    for (float distance : { maxYards * 0.6f, maxYards, maxYards * 1.5f })
        for (int i = 0; i < 8; ++i)
        {
            float const heading = float(i) * Pi / 4.0f;
            add(player->GetPositionX() + std::cos(heading) * distance, player->GetPositionY() + std::sin(heading) * distance,
                player->GetPositionZ(), distance);
        }
    std::sort(spots.begin(), spots.end(), [](Spot const& a, Spot const& b)
    {
        return a.danger < b.danger || (a.danger == b.danger && a.distance < b.distance);
    });

    for (Spot spot : spots)
    {
        // A whole path on the navmesh; the spot's height is the path's end (never the bot's own height).
        // (No spot on the water: the path's end over deep water lies at the surface, which WaterAt calls dry.)
        // (The rise to the surface is part of the way's length.)
        float const rise = fromSurface ? std::max(0.0f, surface[2] - player->GetPositionZ()) : 0.0f;
        if (!CanWalkTo(player, spot.x, spot.y, spot.z, spot.distance + rise, &hostiles, fromSurface ? surface : nullptr,
                fromSurface ? way : nullptr) ||
            TooWetToRest(player, spot.x, spot.y, spot.z))
            continue;
        x = spot.x;
        y = spot.y;
        z = spot.z;
        there = spot.danger;
        return true;
    }
    return false;
}

Escape::Result Escape::Step(Player* player, BotRuntime& rt, bool followsMaster)
{
    Settings const settings = sBotSettings.For(player);
    BotRuntime::Escape& escape = rt.escape;
    uint32 const now = getMSTime();

    if (escape.active && escape.aside)
    {
        if (!player->IsAlive())
        {
            EndAside(player, rt, "died");
            return Result::None;
        }
        // A strong creature on its round that comes near while the bot keeps away from a pack (or goes around one, or
        // out of the reach of one that keeps to its place) is the one to keep away from now: the look-out for strong
        // creatures did not run during such a keep-away, and its legs led on wherever the pack was not. Stormstrikey,
        // a level-60 shaman, kept away from four ghouls in the Eastern Plaguelands and two seconds later had a Nerubian
        // Overseer on it (a rare elite of six times the health and four times the damage on its round through there),
        // was webbed and died, round 194. (Looked for every second and a little farther than a walker's look: the legs
        // are run, and a creature on its round walks towards them.)
        constexpr float AsideWatchYards = 50.0f;
        if (!player->IsInCombat() && (escape.pack || escape.detour || escape.standing) && now >= rt.nextThreatWatch &&
            CanRun(player, true))
        {
            rt.nextThreatWatch = now + WatchMs / 2;
            Creature* roamer = FindThreat(player, player->IsMounted() ? WatchYards * 1.8f : AsideWatchYards);
            if (roamer && escape.standing && roamer->GetGUID() == rt.threatSeen)
                roamer = nullptr;
            if (float wander = 0.0f; roamer && KeepsToItsPlace(roamer, &wander) &&
                roamer->GetExactDist(player) > StandReach(roamer, player, wander))
                roamer = nullptr;
            if (roamer)
            {
                LOG_INFO(ActivityLog, "activity: {} keeps away from {} (level {}{}), {:.0f} yd away, which came near while it {}", rt.name,
                    roamer->GetName(), uint32(roamer->GetLevel()), RankName(roamer), player->GetDistance(roamer),
                    escape.standing ? "kept out of the reach of another" : (escape.detour ? "went around creatures on its way" :
                        "kept away from creatures on its way"));
                escape.pack = false;
                escape.detour = false;
                escape.standing = false;
                escape.camp = false;
                escape.waiting = false;
                escape.startedAt = now;
                escape.fromX = roamer->GetPositionX();
                escape.fromY = roamer->GetPositionY();
                escape.foes = 1;
                escape.foeEntry = roamer->GetEntry();
                escape.foeLevel = roamer->GetLevel();
                rt.threatSeen = roamer->GetGUID();
                rt.threatGap = roamer->GetExactDist(player);
                rt.threatSeenAt = now;
                if (IsRoamer(roamer))
                    NoteRoamer(roamer, player);
                BotDriver::PauseAI(player, PauseMs);
                if (!StartLeg(player, rt, escape, { roamer }))
                    escape.nextLegAt = now + IN_MILLISECONDS;
                return Result::Aside;
            }
        }
        if (escape.detour && !player->IsInCombat())
        {
            constexpr uint32 DetourMaxMs = 20 * IN_MILLISECONDS;
            float const dx = player->GetPositionX() - escape.legX;
            float const dy = player->GetPositionY() - escape.legY;
            bool const there = dx * dx + dy * dy < 6.0f * 6.0f;
            if (there || now >= escape.startedAt + DetourMaxMs || !CanRun(player, true))
            {
                uint32 const foes = escape.foes;
                bool const standing = escape.standing;
                uint32 const seconds = getMSTimeDiff(escape.startedAt, now) / IN_MILLISECONDS;
                rt.escape = BotRuntime::Escape();
                BotDriver::PauseAI(player, 0);
                LOG_INFO(ActivityLog, "activity: {} {} {} creatures on its way after {} s", rt.name,
                    there ? "went around" : "did not get around", foes, seconds);
                // (Not around one that keeps to its place: the task ends and its target counts, no wait for it to pass.)
                if (!there && standing)
                {
                    rt.asideStood = true;
                    return Result::Clear;
                }
                return there ? Result::WentAround : Result::ClearOfPack;
            }
            // The look-out for a strong roamer goes on during the walk around (it returned here before the look): one that
            // comes near ends the walk around, and the next step keeps away from it as from any.
            if (now >= rt.nextThreatWatch)
            {
                rt.nextThreatWatch = now + WatchMs;
                constexpr float BreakOffYards = 20.0f;
                if (Creature* threat = FindThreat(player, WatchYards);
                    threat && threat->GetExactDist(player) <= threat->GetAggroRange(player) + BreakOffYards)
                {
                    uint32 const seconds = getMSTimeDiff(escape.startedAt, now) / IN_MILLISECONDS;
                    LOG_INFO(ActivityLog, "activity: {} breaks off its way around {} creatures after {} s: {} (level {}) is {:.0f} yd away",
                        rt.name, uint32(escape.foes), seconds, threat->GetName(), uint32(threat->GetLevel()), player->GetDistance(threat));
                    rt.escape = BotRuntime::Escape();
                    rt.nextThreatWatch = 0;
                    BotDriver::PauseAI(player, 0);
                    return Result::None;
                }
            }
            BotDriver::PauseAI(player, PauseMs);
            if (!player->isMoving())
                player->GetMotionMaster()->MovePoint(0, escape.legX, escape.legY, escape.legZ, FORCED_MOVEMENT_RUN);
            return Result::Aside;
        }
        if (player->IsInCombat())
        {
            // Caught, or something else attacked. What it can take, it fights: a rogue that backed out of the Raven
            // Hill cemetery after a pull it had called off was attacked by one skeleton three levels below it, "left"
            // that fight at 93 % health with Vanish and Sprint, ran for 35 seconds through the cemetery and died with
            // four on it. The usual rule decides (ShouldLeave), and a creature far above it is never fought.
            {
                std::vector<Creature*> const on = FoesOf(player);
                bool strongOn = false;
                // (Nor one the module ended a fight with because it did not die, TrackFight: the bot goes on away
                // from it, it would only be the same fight again.)
                for (Creature* creature : on)
                    strongOn = strongOn || (on.size() == 1 ? StrongByLevel(player, creature) :
                        StrengthOf(creature) >= uint32(player->GetLevel()) + ThreatLevels) ||
                        (rt.undyingFoes.count(creature->GetGUID()) && getMSTime() < rt.undyingFoes[creature->GetGUID()]);
                float need = 0.0f;
                float rate = 0.0f;
                bool byCount = false;
                bool byFight = false;
                if (!on.empty() && !strongOn && !ShouldLeave(player, rt, on, need, byCount, byFight, rate))
                {
                    EndAside(player, rt, "attacked on its way, it fights");
                    return Result::None;
                }
                // In combat with nothing on it yet (a wasp that has seen it and flies at it): the keep-away goes on a
                // moment until what attacks is known, not straight into a run. A mage kept away from three Hive'Zora
                // Wasps, ran for 36 s from the one that came, down to 22 % health, and died against five, round 184.
                constexpr uint32 SeeAttackerMs = 2 * IN_MILLISECONDS;
                if (on.empty())
                {
                    if (!escape.combatSeenAt)
                        escape.combatSeenAt = std::max<uint32>(now, 1);
                    if (getMSTimeDiff(escape.combatSeenAt, now) < SeeAttackerMs)
                        return Result::Aside;
                }
            }
            // From here on it is a run from a fight (below, next tick).
            escape.fromAside = true;
            escape.nextRecheck = now + 2 * IN_MILLISECONDS;
            escape.aside = false;
            escape.detour = false;
            escape.startedAt = now;
            escape.healthStart = uint8(player->GetHealthPct());
            // (Its line counts what is on the bot, not the camp it was leaving: "left a fight against 17 foes".)
            if (std::vector<Creature*> const on = FoesOf(player); !on.empty())
            {
                escape.foes = uint8(std::min<std::size_t>(on.size(), 255));
                escape.foeEntry = on.front()->GetEntry();
                escape.foeLevel = on.front()->GetLevel();
            }
            StopFighting(player);
            return Result::Running;
        }
        // From a pack it keeps away until none of it is within PackClearYards (the look-out for one strong roamer
        // does not see ordinary creatures: a first build was "clear of it after 1 s" and rode on into the pack).
        constexpr float PackClearYards = 30.0f;
        Creature* threat = nullptr;
        if (escape.pack)
        {
            for (Creature* creature : LiveHostiles(player, PackClearYards, PackLevelsBelow))
                if (!creature->IsInCombat() && (!threat || creature->GetExactDist(player) < threat->GetExactDist(player)))
                    threat = creature;
        }
        else if (escape.standing)
        {
            // From one that keeps to its place the bot only has to be out of its reach: it does not follow. (Sixty yards
            // and the look for other strong creatures are for one on a round.) One that sets off after all (a script,
            // something attacked it) is kept away from as a roamer from here on.
            float wander = 0.0f;
            if (Creature* seen = ObjectAccessor::GetCreature(*player, rt.threatSeen); seen && seen->IsAlive() && seen->IsHostileTo(player))
            {
                if (!KeepsToItsPlace(seen, &wander))
                {
                    escape.standing = false;
                    threat = seen;
                }
                else if (seen->GetExactDist(player) < StandReach(seen, player, wander) + StandAwayYards)
                    threat = seen;
            }
        }
        else
        {
            threat = FindThreat(player, ClearYards);
            // The one it keeps away from stays one while it is alive and within ClearYards, whatever the look makes
            // of it at this moment (two keep-aways from Stitches ended "clear of it after 0 s" at 26 and 35 yards).
            if (!threat)
                if (Creature* seen = ObjectAccessor::GetCreature(*player, rt.threatSeen); seen && seen->IsAlive() &&
                    seen->IsHostileTo(player) && seen->GetExactDist(player) <= ClearYards)
                    threat = seen;
            if (threat)
            {
                rt.threatSeen = threat->GetGUID();
                rt.threatGap = threat->GetExactDist(player);
                rt.threatSeenAt = now;
                // (It came near again while the bot waited: the run begins anew, three times at most.)
                if (escape.waiting && escape.reruns < 3)
                {
                    escape.waiting = false;
                    ++escape.reruns;
                    escape.startedAt = now;
                }
            }
            // Sixty yards off is not clear of one that is on its way: a rider is that far after a second, the task
            // ends, the next one leads back onto the road, and the roamer is there (four bots kept away from
            // Stitches fifteen times in a quarter of an hour, "clear of it after 0-10 s" each time; two of them died).
            // The bot stands where it got to until the roamer has moved on, or has stopped walking and fighting.
            else if (Creature* seen = ObjectAccessor::GetCreature(*player, rt.threatSeen); seen && seen->IsAlive() &&
                seen->IsHostileTo(player) && (seen->isMoving() || seen->IsInCombat()) && seen->GetExactDist(player) <= RoamerWaitYards &&
                now < escape.startedAt + RoamerWaitMs && CanRun(player, true) && !StaysAround(player, seen) && !escape.walkAway)
            {
                if (!escape.waiting)
                {
                    escape.waiting = true;
                    player->StopMoving();
                    player->GetMotionMaster()->Clear();
                    LOG_INFO(ActivityLog, "activity: {} waits {:.0f} yd from {} (level {}) until it has moved on", rt.name,
                        seen->GetExactDist(player), seen->GetName(), uint32(seen->GetLevel()));
                }
                NoteRoamer(seen, player);
                BotDriver::PauseAI(player, PauseMs);
                return Result::Aside;
            }
        }
        // Clear of the pack is not out of the camp: with creatures on every side the next task would start in the
        // middle of them. A rogue that had ridden into the Kurzen compound was "clear of it after 12 s" in a gap
        // between the packs, started its next task there, and every way out led past creatures: four escapes in
        // four minutes, the last one into its death. It goes on until one side is open.
        if (!threat && escape.pack && CanRun(player, true))
        {
            Creature* nearest = nullptr;
            uint32 around = 0;
            bool inside = Surrounded(player, &nearest, &around, escape.leaving ? LeaveLevelsBelow : 3) && nearest;
            // (On its way out of a camp it had no business in, the camp is also what the spawn data calls one: the
            // creatures it killed on its way in are back in minutes.)
            if (!inside && escape.leaving && nearest)
                inside = Risk::InCampItCannotTake(player, rt, player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
                    player->GetPositionZ());
            if (inside)
            {
                threat = nearest;
                if (!escape.camp)
                {
                    escape.camp = true;
                    LOG_INFO(ActivityLog, "activity: {} stands in the middle of a camp ({} creatures within {:.0f} yd, on every side): it goes "
                        "on until it is out of it", rt.name, around, CampAroundYards);
                }
            }
        }
        if (!threat || now >= escape.startedAt + (escape.camp ? CampMaxMs : (escape.waiting ? RoamerWaitMs : AsideMaxMs)) ||
            !CanRun(player, true))
        {
            bool const pack = escape.pack;
            bool const leaving = escape.leaving;
            bool const standing = escape.standing;
            // The place is set aside after a pack or a camp. Not after one strong creature: a creature on a round is
            // somewhere else in a minute (its sights keep spots and ways on its round out for that long: RoamerNear),
            // and one that keeps to its place reaches only so far. A hundred yards around Jin'Zallah, who stands
            // still 72 yards from the water hole of a Tanaris quest, set the pool aside for ten minutes: the paladin
            // that had kept away from it "had nothing in Tanaris it could safely get to" and set out across the sea.
            if (pack || escape.camp)
                rt.AddAvoidArea(escape.mapId, escape.fromX, escape.fromY, player->GetLevel(), AsideAvoidMs, false);
            EndAside(player, rt, threat ? (escape.camp ? "it is still in the camp, goes on" : "it is still near, goes on") :
                (escape.camp ? "out of the camp" : standing ? "out of its reach" : "clear of it"));
            // (Out of a camp it had no business in: its task, if it has one, goes on from where it stands now.)
            if (leaving)
                return Result::WentAround;
            // Away from what held a fight the module ended (WalkAway: a creature that did not die, one that held the bot
            // in combat doing nothing): no roamer to wait for and no strong creature on its way, the task goes on from
            // here. (After the walk away from a Servant of Grol held at one health point, the bot waited 61 yards from it
            // "until it has moved on" and then its voyage waited for it as "on its way".)
            if (escape.walkAway)
                return Result::WentAround;
            // Out of the reach of one that keeps to its place: the task goes on (the next look goes around it). Still in
            // it after the time: the task ends and its target counts.
            if (standing)
            {
                if (!threat)
                    return Result::WentAround;
                rt.asideStood = true;
            }
            // One that stays around the place (a town's guard, a short round): it will be there again, so the task's
            // target counts as one behind it and is set aside, at once and without a wait (OrderService), and the
            // place around it is kept out of the next ways for a while.
            if (!pack && !escape.camp)
                if (Creature* seen = ObjectAccessor::GetCreature(*player, rt.threatSeen); seen && seen->IsAlive() && StaysAround(player, seen))
                {
                    rt.asideStood = true;
                    rt.AddAvoidArea(seen->GetMapId(), seen->GetPositionX(), seen->GetPositionY(), player->GetLevel(), AsideAvoidMs, false);
                    LOG_INFO(ActivityLog, "activity: {} does not wait for {} (level {}) to move on: it stays about the place ({}); the bot "
                        "turns to other work", rt.name, seen->GetName(), uint32(seen->GetLevel()), seen->IsGuard() ? "a guard" :
                        (sRoadMap.InHostileTown(seen->GetMapId(), seen->GetPositionX(), seen->GetPositionY(), player->GetTeamId()) ?
                            "in a town of the other side" : "its round is short or lies around the bot"));
                }
            return pack ? Result::ClearOfPack : Result::Clear;
        }
        BotDriver::PauseAI(player, PauseMs);
        float const dx = player->GetPositionX() - escape.legX;
        float const dy = player->GetPositionY() - escape.legY;
        if (now >= escape.nextLegAt || dx * dx + dy * dy < 8.0f * 8.0f)
            if (!StartLeg(player, rt, escape, { threat }))
                escape.nextLegAt = now + IN_MILLISECONDS;
        return Result::Aside;
    }

    if (escape.active)
    {
        if (!player->IsAlive())
        {
            LearnFromRun(player, rt, true);
            // (Dead on a run from a creature it had not fought: the fight with it teaches nothing, OnFightEnd. Shamanheal,
            // caught on its keep-away by a Crimson Bodyguard, ran at once and died, and "learned" that the kind costs
            // it 150 %, round 226: from then on it would only have run from them.)
            if (escape.fromAside || escape.threatEntry)
                rt.runDeathUnfoughtAt = std::max<uint32>(getMSTime(), 1);
            End(player, rt, "died on the run");
            return Result::None;
        }
        // Out of the fight is not out of the place: after a Vanish or a Feign Death, or when the foes turn back
        // after two steps, they stand where they stood, and a bot that sits down to eat there is attacked again
        // (a rogue "got away after 1 s" with Vanish, began its rest at 35 % health among four orcs and died eight
        // seconds later). It keeps going until nothing hostile is near, or it has put some distance behind it.
        constexpr float ClearOfFoesYards = 25.0f;
        constexpr float ClearDistance = 45.0f;
        constexpr uint32 ClearMaxMs = 12 * IN_MILLISECONDS;
        if (!player->IsInCombat())
        {
            if (!escape.clearSince)
                escape.clearSince = now;
            // A hunter that is lying in its Feign Death stays down while its foes are near: a step stands it up, and the
            // foes, on their way home or still about, see it again. A hunter "used Feign Death, Disengage" against five
            // Hive'Zora Wasps at 83 % health, ran on and died on the run, round 191.
            constexpr uint32 LieStillMs = 20 * IN_MILLISECONDS;
            if (player->HasAura(FeignDeathSpell) && getMSTimeDiff(escape.clearSince, now) < LieStillMs &&
                !LiveHostiles(player, ClearOfFoesYards, PackLevelsBelow, true).empty())
            {
                BotDriver::PauseAI(player, PauseMs);
                if (player->isMoving())
                    player->StopMoving();
                return Result::Running;
            }
            if (getMSTimeDiff(escape.clearSince, now) < ClearMaxMs && CanRun(player, true) &&
                std::hypot(player->GetPositionX() - escape.fromX, player->GetPositionY() - escape.fromY) < ClearDistance)
            {
                // (Down to ten levels below it: a level-24 rogue "got away after 1 s" from level-18 murlocs with a
                // Vanish, sat down among them and was dead ten seconds later.)
                std::vector<Creature*> const around = LiveHostiles(player, ClearOfFoesYards, PackLevelsBelow, true);
                if (!around.empty())
                {
                    BotDriver::PauseAI(player, PauseMs);
                    float const dx = player->GetPositionX() - escape.legX;
                    float const dy = player->GetPositionY() - escape.legY;
                    if (now >= escape.nextLegAt || dx * dx + dy * dy < 8.0f * 8.0f)
                        if (!StartLeg(player, rt, escape, around))
                            escape.nextLegAt = now + IN_MILLISECONDS;
                    return Result::Running;
                }
            }
            // ... and out of the fight with nothing near is not out of the camp (a rogue got away with a Vanish at
            // 10 % health, rested in a gap of the compound, set out from there and was dead 23 seconds later).
            if (getMSTimeDiff(escape.clearSince, now) < CampMaxMs && CanRun(player, true) &&
                std::hypot(player->GetPositionX() - escape.fromX, player->GetPositionY() - escape.fromY) < CampMaxYards)
            {
                Creature* nearest = nullptr;
                uint32 around = 0;
                if (Surrounded(player, &nearest, &around) && nearest)
                {
                    if (!escape.camp)
                    {
                        escape.camp = true;
                        LOG_INFO(ActivityLog, "activity: {} is out of the fight but in the middle of a camp ({} creatures within {:.0f} yd, on "
                            "every side): it goes on until it is out of it", rt.name, around, CampAroundYards);
                    }
                    BotDriver::PauseAI(player, PauseMs);
                    float const dx = player->GetPositionX() - escape.legX;
                    float const dy = player->GetPositionY() - escape.legY;
                    if (now >= escape.nextLegAt || dx * dx + dy * dy < 8.0f * 8.0f)
                        if (!StartLeg(player, rt, escape, { nearest }))
                            escape.nextLegAt = now + IN_MILLISECONDS;
                    return Result::Running;
                }
            }
        }
        else
            escape.clearSince = 0;
        if (!player->IsInCombat())
        {
            LearnFromRun(player, rt, false);
            // A place it leaves again and again is set aside longer each time.
            std::erase_if(rt.escapeMarks, [now](BotRuntime::EscapeMark const& mark) { return getMSTimeDiff(mark.at, now) > EscapeMemoryMs; });
            uint32 earlier = 0;
            for (BotRuntime::EscapeMark const& mark : rt.escapeMarks)
            {
                float const mx = mark.x - escape.fromX;
                float const my = mark.y - escape.fromY;
                if (mark.mapId == escape.mapId && mx * mx + my * my <= EscapePlaceYards * EscapePlaceYards)
                    ++earlier;
            }
            rt.escapeMarks.push_back({ escape.mapId, escape.fromX, escape.fromY, now });
            uint32 const avoidMs = std::min<uint32>(MaxAvoidMs, AvoidMs << std::min<uint32>(earlier, 3));
            // (A level-up frees it only when it puts the bot above the foes.)
            rt.AddAvoidArea(escape.mapId, escape.fromX, escape.fromY, std::max<uint8>(player->GetLevel(), escape.foeLevel), avoidMs, false);
            for (BotRuntime::Tally* tally : { &rt.window, &rt.total })
                ++tally->escapes;
            End(player, rt, earlier ? "got away again (the place is set aside longer)" : "got away");
            return Result::GotAway;
        }
        if (!CanRun(player))
        {
            // A net or a root: with something that frees it the run goes on (the next leg at once).
            if (BreakFree(player, escape))
            {
                escape.nextLegAt = now;
                return Result::Running;
            }
            // A stun or root of a few seconds is waited out, and the run goes on when it ends: what made the run the
            // better choice still holds (the AI is not held meanwhile, so a rooted bot fights where it stands).
            std::string holdName;
            if (uint32 const holdMs = ShortHold(player, &holdName);
                holdMs && now + holdMs < escape.startedAt + settings.escapeMaxSeconds * IN_MILLISECONDS)
            {
                if (escape.heldBy.find(holdName) == std::string::npos)
                    escape.heldBy += (escape.heldBy.empty() ? "" : ", ") + holdName;
                escape.nextLegAt = now + holdMs;
                return Result::Running;
            }
            rt.noEscapeUntil = now + HeldPauseMs;
            End(player, rt, "held fast, fights on");
            return Result::Failed;
        }
        if (now >= escape.startedAt + settings.escapeMaxSeconds * IN_MILLISECONDS)
        {
            rt.noEscapeUntil = now + FailedPauseMs;
            End(player, rt, "could not shake them off, fights on");
            return Result::Failed;
        }
        // A run that began as a keep-away it was caught on: what is on it now is looked at again every two seconds, and
        // when the usual rule says it can take that fight, it fights (the mage of round 184 ran from one wasp).
        if (escape.fromAside && now >= escape.nextRecheck)
        {
            escape.nextRecheck = now + 2 * IN_MILLISECONDS;
            std::vector<Creature*> const on = FoesOf(player);
            bool strongOn = false;
            for (Creature* creature : on)
                strongOn = strongOn || (on.size() == 1 ? StrongByLevel(player, creature) :
                    StrengthOf(creature) >= uint32(player->GetLevel()) + ThreatLevels) ||
                    (rt.undyingFoes.count(creature->GetGUID()) && now < rt.undyingFoes.at(creature->GetGUID()));
            float need = 0.0f;
            float rate = 0.0f;
            bool byCount = false;
            bool byFight = false;
            if (!on.empty() && !strongOn && !ShouldLeave(player, rt, on, need, byCount, byFight, rate))
            {
                rt.noEscapeUntil = now + HeldPauseMs;
                End(player, rt, "the foes on it are ones it can take, fights on");
                return Result::Failed;
            }
        }

        BotDriver::PauseAI(player, PauseMs);
        StopFighting(player);
        if (now < escape.startedAt + HelpMs)
            CastHelp(player, escape, FoesOf(player));
        // Lying in its Feign Death: no step until the foes have let go (the branch above, out of combat). Still in the
        // fight a moment after it (resisted): it gets up and runs.
        constexpr uint32 FeignTakesMs = 1500;
        if (player->HasAura(FeignDeathSpell))
        {
            if (escape.feignAt && getMSTimeDiff(escape.feignAt, now) < FeignTakesMs)
            {
                player->GetMotionMaster()->Clear();
                player->StopMoving();
                return Result::Running;
            }
            player->RemoveAurasDueToSpell(FeignDeathSpell);
        }
        float const dx = player->GetPositionX() - escape.legX;
        float const dy = player->GetPositionY() - escape.legY;
        if (now >= escape.nextLegAt || dx * dx + dy * dy < 8.0f * 8.0f)
            if (!StartLeg(player, rt, escape, FoesOf(player)))
                escape.nextLegAt = now + IN_MILLISECONDS; // boxed in for the moment: look again next tick
        return Result::Running;
    }

    if (player->IsAlive() && !player->IsInCombat())
        RecordTrail(player, rt);

    // Out of a camp first. A bot that stands in the middle of a camp it cannot take and has no fight there goes out
    // of it before anything else: with no task (a task ended there, it entered the world there, it waits), while it
    // waits for its squad, and when its next task lies elsewhere. Along the way it came, else to the emptiest side
    // (the legs of a keep-away). A squad leader had left the Kurzen compound, went back in for its member, and then
    // stood four yards from the member's corpse in the middle of 18 creatures, "Squad: waiting"; its next task, 505
    // yards away, began there, and it fought its way out. Three bots died in that compound in one night, two of them
    // "during no task". A member follows its leader; a character that follows its owner is its owner's matter.
    // The camp: by the creatures alive around it (Surrounded) when four of them at once are more than it can take,
    // or by the spawn data (Risk::InCampItCannotTake) with one of them alive within 55 yards.
    if (settings.escapeEnable && player->IsAlive() && !player->IsInCombat() && !rt.pull.active && !rt.squad.following && !followsMaster)
    {
        constexpr uint32 CampLeaveLookMs = 2 * IN_MILLISECONDS;
        constexpr uint32 CampLeaveAgainMs = 30 * IN_MILLISECONDS;
        constexpr float CampBusinessYards = 70.0f;
        BotTask const& task = rt.task;
        uint64 const key = TaskKeyOf(task);
        bool const fresh = key != rt.campLeaveTask;
        rt.campLeaveTask = key;
        char const* why = nullptr;
        if (task.kind == TaskKind::None)
            why = "with nothing to do there";
        else if (!rt.squad.waitFor.empty())
            why = "while it waits for its squad";
        else if (fresh && task.kind != TaskKind::Voyage && (!task.hasSpot || task.spot.mapId != player->GetMapId() ||
            player->GetDistance2d(task.spot.x, task.spot.y) > CampBusinessYards))
            why = "and its next task lies elsewhere";
        if (why && (fresh || now >= rt.nextCampLeave) && CanRun(player, true))
        {
            rt.nextCampLeave = now + CampLeaveLookMs;
            Creature* nearest = nullptr;
            uint32 around = 0;
            Risk::CampAt camp;
            bool const surrounded = Surrounded(player, &nearest, &around, LeaveLevelsBelow) && nearest &&
                !Risk::CanFight(player, rt, nearest->GetLevel(), std::min<uint32>(around, 4));
            bool const inCamp = nearest && Risk::InCampItCannotTake(player, rt, player->GetMapId(), player->GetPositionX(),
                player->GetPositionY(), player->GetPositionZ(), &camp);
            if (nearest && (surrounded || inCamp))
            {
                escape = BotRuntime::Escape();
                escape.active = true;
                escape.aside = true;
                escape.pack = true;
                escape.camp = true;
                escape.leaving = true;
                escape.startedAt = now;
                escape.mapId = player->GetMapId();
                escape.fromX = nearest->GetPositionX();
                escape.fromY = nearest->GetPositionY();
                escape.healthStart = uint8(player->GetHealthPct());
                escape.foes = uint8(std::min<uint32>(surrounded ? around : camp.count, 255));
                escape.foeEntry = nearest->GetEntry();
                escape.foeLevel = nearest->GetLevel();
                if (StartLeg(player, rt, escape, { nearest }))
                {
                    rt.nextCampLeave = now + CampLeaveAgainMs;
                    BotDriver::PauseAI(player, PauseMs);
                    LOG_INFO(ActivityLog, "activity: {} stands in the middle of a camp it cannot take ({} creatures within {:.0f} yd; the "
                        "nearest: {} level {}, {:.0f} yd away) {}: it goes out of it first", rt.name, uint32(escape.foes), CampAroundYards,
                        nearest->GetName(), uint32(nearest->GetLevel()), player->GetDistance(nearest), why);
                    return Result::Aside;
                }
                escape = BotRuntime::Escape();
            }
        }
    }

    // A roaming creature far above the bot comes near: out of combat the bot steps aside, in a fight it leaves at once.
    bool threatNear = false;
    uint32 threatEntry = 0;
    uint8 threatLevel = 0;
    ObjectGuid threatGuid;
    // (A squad member beside its leader looks out for a strong roamer itself: its leader may be 30 yards ahead and
    // past it. Stitches killed two members of one squad "during no task" within three seconds while their leader
    // waited for them. The packs on the way stay the leader's matter, and a character that follows its owner's.)
    bool const memberLooksOut = followsMaster && rt.squad.following;
    if (settings.escapeEnable && settings.escapeStepAside && (!followsMaster || memberLooksOut) && player->IsAlive() &&
        now >= rt.nextThreatWatch && CanRun(player, true))
    {
        // (A rider looks every second and twice as far: at mount speed towards a patrol that comes its way the
        // 35 yards of a walker are gone between two looks; a level-30 warlock rode into Stitches without a look.)
        bool const rides = player->IsMounted();
        rt.nextThreatWatch = now + (rides ? WatchMs / 2 : WatchMs);
        Creature* threat = FindThreat(player, rides ? WatchYards * 1.8f : WatchYards);
        // A roamer the bot is not getting nearer to is let be: it walks away, or the bot has passed it. A squad on a
        // road in Westfall kept away from Vultros (a level-26 rare on patrol there) 67 times in a row: each run ended
        // "it is still near, goes on" and the next look two seconds later began the next. Near its reach it counts
        // whatever the gap does.
        constexpr float ThreatNearYards = 5.0f;
        constexpr uint32 ThreatMemoryMs = 8 * IN_MILLISECONDS;
        Creature* roamer = nullptr;
        bool standing = false;
        float standWander = 0.0f;
        float standReach = 0.0f;
        // In a fight, one that keeps to its place counts only once the fight has drawn the bot into its reach: a level-47
        // warrior left a fight against one Sandfury troll at full health beside the water hole of a Tanaris quest
        // because Jin'Zallah, who never moves, stood 40 yards off ("was near"), and its quest task ended.
        if (threat && player->IsInCombat())
            if (float wander = 0.0f; KeepsToItsPlace(threat, &wander) && threat->GetExactDist(player) > StandReach(threat, player, wander))
                threat = nullptr;
        if (threat && !player->IsInCombat())
        {
            float const gap = threat->GetExactDist(player);
            bool const known = rt.threatSeen == threat->GetGUID() && getMSTimeDiff(rt.threatSeenAt, now) <= ThreatMemoryMs;
            bool const closing = !known || gap < rt.threatGap - 1.0f;
            bool const atHand = gap <= threat->GetAggroRange(player) + ThreatNearYards;
            rt.threatSeen = threat->GetGUID();
            rt.threatGap = gap;
            rt.threatSeenAt = now;
            standing = KeepsToItsPlace(threat, &standWander);
            if (standing)
            {
                // One that keeps to its place is no danger to a way that stays out of its reach, however near the way
                // leads: the bot rides past. A way into its reach goes around it; inside its reach the bot steps away.
                standReach = StandReach(threat, player, standWander);
                if (gap > standReach && WayClosest(player, threat, StandAheadYards) >= standReach)
                    threat = nullptr;
                else
                    roamer = threat;
            }
            else if (!atHand && !closing)
                threat = nullptr;
            else
                roamer = threat;
        }
        // ... or the bot is about to walk into creatures it cannot take together (a warrior on its way through an
        // orc camp was attacked by four to six of them five times in ten minutes).
        uint32 packAhead = 0;
        std::vector<Creature*> pack;
        float destX = 0.0f;
        float destY = 0.0f;
        if (!threat && !player->IsInCombat() && !memberLooksOut)
            threat = FindPackAhead(player, rt, packAhead, &pack, &destX, &destY);
        // ... or creatures on their round come at it where it stands.
        bool packComing = false;
        if (!threat && !player->IsInCombat())
        {
            threat = FindPackComing(player, rt, packAhead, &pack);
            packComing = threat != nullptr;
        }
        // ... or into the middle of a camp, seen from the spawn data before two of its creatures stand on the next
        // 45 yards: around its edge, or away, while the bot is still outside (a warrior walked to its quest objects
        // through the Mosshide camp of the Wetlands, five on it; a shaman on a travel three minutes later, three).
        bool campAhead = false;
        if (!threat && !player->IsInCombat() && !memberLooksOut)
        {
            threat = FindCampAhead(player, rt, packAhead, pack, destX, destY);
            campAhead = threat != nullptr;
        }
        // A pack on the way: around it, as a player would go, and on with the task. (Ending the task there made the
        // planner start it again, towards the same pack.) Three walks around within a minute and a half and it is
        // treated as before: away from it, and the task ends.
        // The same for one roamer that stands on the bot's way: around it and on (running from it on a road it
        // walks along leads nowhere).
        // (Not around one that is walking towards the bot: the way around is where it will be in a moment. A
        // level-25 warrior went around Stitches at 31 yards, was "around" after 3 s, and was dead ten seconds later.)
        // (Nor around one that walks or fights at all, or that is near: the circle around it is drawn where it stands
        // now, with eight yards to spare, and it does not stand. A level-29 warrior went around Stitches at 49, 19
        // and 12 yards within twelve seconds and was dead fifteen seconds later. From such a one the bot keeps away
        // and waits until it has moved on.)
        // (One that keeps to its place is gone around whenever the bot is outside its reach: it does not come.)
        constexpr float RoamerDetourMinYards = 40.0f;
        bool const comesAtIt = roamer && (standing ? roamer->GetExactDist(player) <= standReach : (roamer->isMoving() ||
            roamer->IsInCombat() || roamer->GetExactDist(player) < RoamerDetourMinYards));
        // What the keep-away below leads to for one that keeps to its place: the task goes on afterwards, unless the bot's
        // way ends inside its reach or it found no way around it (then the task's target counts as one it cannot get to).
        bool standBlocks = false;
        if (roamer && !comesAtIt && !packAhead && player->isMoving() && !player->movespline->Finalized())
        {
            constexpr uint32 DetourWindowMs = 90 * IN_MILLISECONDS;
            uint8 const maxDetours = standing ? StandMaxDetours : 3;
            if (getMSTimeDiff(rt.detoursSince, now) > DetourWindowMs)
            {
                rt.detours = 0;
                rt.detoursSince = now;
            }
            G3D::Vector3 const destination = player->movespline->FinalDestination();
            bool const endsInside = standing && roamer->GetExactDist2d(destination.x, destination.y) <= standReach;
            if (rt.detours < maxDetours && !endsInside && player->GetExactDist2d(destination.x, destination.y) > 20.0f &&
                StartDetour(player, rt, { roamer }, roamer, destination.x, destination.y, standing ? standWander : 0.0f))
            {
                ++rt.detours;
                rt.escape.standing = standing;
                BotDriver::PauseAI(player, PauseMs);
                LOG_INFO(ActivityLog, "activity: {} goes around {} (level {}{}) on its way, {:.0f} yd away{}{}", rt.name, roamer->GetName(),
                    uint32(roamer->GetLevel()), RankName(roamer), player->GetDistance(roamer), player->IsMounted() ? "; it was riding" : "",
                    standing ? Acore::StringFormat(" [it keeps to its place: the way keeps {:.0f} yd from it]", standReach) : std::string());
                return Result::Aside;
            }
            standBlocks = standing;
        }
        // (Not around a pack that walks: the circle around it is drawn where it stands now. A level-60 mage went around
        // a Crimson Courier and its four bodyguards, elites on their round through the Eastern Plaguelands, at 45 yards,
        // and the five were on it sixteen seconds later, round 145. From such a pack it keeps away, as from one roamer
        // that walks.)
        // (A camp stays where it is although one of it may walk a round in it: around a camp the bot still goes. A
        // pack walks when the nearest of it walks or half of it does: on a waypoint round, in a formation whose leader
        // walks one, or in a fight. Creatures that wander about their spawn stay where they are, as far as a way
        // around them goes: the first test turned four wandering Rock Stalkers into a pack that walks.)
        auto const walks = [](Creature const* creature)
        {
            auto const onRound = [](Creature const* one)
            {
                return one->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE ||
                    one->GetDefaultMovementType() == WAYPOINT_MOTION_TYPE;
            };
            if (creature->IsInCombat() || onRound(creature))
                return true;
            CreatureGroup const* group = creature->GetFormation();
            Creature const* leader = group ? group->GetLeader() : nullptr;
            return leader && leader != creature && onRound(leader);
        };
        bool const packWalks = packAhead && !campAhead && threat && !pack.empty() &&
            (walks(threat) || 2 * std::count_if(pack.begin(), pack.end(), walks) >= std::ptrdiff_t(pack.size()));
        if (threat && packAhead && !packWalks)
        {
            constexpr uint32 DetourWindowMs = 90 * IN_MILLISECONDS;
            constexpr uint8 MaxDetours = 3;
            if (getMSTimeDiff(rt.detoursSince, now) > DetourWindowMs)
            {
                rt.detours = 0;
                rt.detoursSince = now;
            }
            if (rt.detours < MaxDetours && StartDetour(player, rt, pack, threat, destX, destY))
            {
                ++rt.detours;
                BotDriver::PauseAI(player, PauseMs);
                LOG_INFO(ActivityLog, "activity: {} goes around {} creatures on its way that it cannot take together (the nearest: {} "
                    "level {}, {:.0f} yd away{}){}{}", rt.name, packAhead, threat->GetName(), uint32(threat->GetLevel()), player->GetDistance(threat),
                    player->IsMounted() ? "; it was riding" : "", rt.escape.foes > packAhead ?
                        Acore::StringFormat(" and the {} that stand with them", uint32(rt.escape.foes) - packAhead) : std::string(),
                    campAhead ? " [its way led into the middle of their camp]" : "");
                return Result::Aside;
            }
        }
        // (There was a pass of three minutes for a roamer the bot had just kept away from, so that its way past one
        // stayed open: a level-14 mage kept away from a level-26 rare beside a quest giver, started the same pickup
        // again, walked into the rare 44 seconds later and was dead in two hits. The planner counts the second
        // time against the task's target instead.)
        if (threat)
        {
            if (player->IsInCombat())
            {
                threatNear = true;
                threatEntry = threat->GetEntry();
                threatLevel = threat->GetLevel();
                threatGuid = threat->GetGUID();
            }
            else
            {
                // (One strong roamer is not stepped aside for again for a while, so a way past it stays open. A pack
                // gets no such pass: the bot rode into the same three murlocs thirteen seconds after it had kept
                // away from them. The planner counts the second time against the task's target instead.)
                escape = BotRuntime::Escape();
                escape.active = true;
                escape.aside = true;
                escape.pack = packAhead != 0;
                // From one that keeps to its place: out of its reach and on with the task; when the way to the task ends
                // inside its reach (or leads nowhere around it), the task ends and its target counts (asideStood).
                bool const fromStanding = standing && roamer == threat && !packAhead;
                // (The bot stood inside its reach again and again: Mahiri, a level-23 shaman, stepped out of a Scorched
                // Basilisk's (28) reach in Stonetalon 23 times in five minutes, 5 to 16 yd from it each time, while its
                // own steps to the spot of "New Life", 17 yd below it, led back past the basilisk, until the stuck rule
                // ended the task. The third step-out from the same creature within three minutes counts as a way that
                // ends inside its reach.)
                bool stoodAgain = false;
                if (fromStanding && !standBlocks)
                {
                    constexpr uint32 StandOutWindowMs = 3 * MINUTE * IN_MILLISECONDS;
                    constexpr uint8 StandOutsMax = 3;
                    if (rt.standOutFrom != threat->GetGUID() || getMSTimeDiff(rt.standOutSince, now) > StandOutWindowMs)
                    {
                        rt.standOutFrom = threat->GetGUID();
                        rt.standOuts = 0;
                        rt.standOutSince = now;
                    }
                    stoodAgain = ++rt.standOuts >= StandOutsMax;
                    if (stoodAgain)
                        rt.standOuts = 0;
                }
                escape.standing = fromStanding && !standBlocks && !stoodAgain;
                rt.asideStood = fromStanding && (standBlocks || stoodAgain);
                escape.startedAt = now;
                escape.mapId = player->GetMapId();
                escape.fromX = threat->GetPositionX();
                escape.fromY = threat->GetPositionY();
                // A pack or a camp: its place is set aside now, at its middle, not only when the keep-away ends. The
                // planner chooses the next task the moment this one ends, and with nothing set aside yet the next way
                // led into the same camp: a mage beside the Undead Ravager camp of Desolace ended seven tasks in a
                // minute and a half on it (round 112). Its middle: a way into the camp comes nearer to it, which is
                // what CrossesAvoidedArea asks.
                rt.packWalked = escape.pack && packWalks;
                // (Not for a pack that walks a round: it does not stay there, and its place set aside closed the way out
                // after it had passed. Northwatch Hold, 2026-11-24: a level-23 tauren kept away from six marines on their
                // round, its walk out was refused at their place, and it went home by hearthstone.)
                if (escape.pack && !pack.empty() && !packWalks)
                {
                    float cx = 0.0f;
                    float cy = 0.0f;
                    for (Creature const* creature : pack)
                    {
                        cx += creature->GetPositionX();
                        cy += creature->GetPositionY();
                    }
                    escape.fromX = cx / float(pack.size());
                    escape.fromY = cy / float(pack.size());
                    rt.AddAvoidArea(escape.mapId, escape.fromX, escape.fromY, player->GetLevel(), AsideAvoidMs, false);
                }
                escape.healthStart = uint8(player->GetHealthPct());
                escape.foes = 1;
                escape.foeEntry = threat->GetEntry();
                escape.foeLevel = threat->GetLevel();
                if (!StartLeg(player, rt, escape, { threat }))
                {
                    escape = BotRuntime::Escape();
                    return Result::None;
                }
                BotDriver::PauseAI(player, PauseMs);
                if (packAhead)
                    LOG_INFO(ActivityLog, "activity: {} keeps away from {} creatures {} that it cannot take together (the nearest: {} "
                        "level {}, {:.0f} yd away{}){}{}", rt.name, packAhead, packComing ? "that come at it on their round" : "on its way",
                        threat->GetName(), uint32(threat->GetLevel()), player->GetDistance(threat),
                        player->IsMounted() ? "; it was riding" : "", campAhead ? " [its way led into the middle of their camp]" : "",
                        packWalks ? " [they walk: no way around them]" : "");
                else
                    LOG_INFO(ActivityLog, "activity: {} keeps away from {} (level {}{}), {:.0f} yd away{}", rt.name, threat->GetName(),
                        uint32(threat->GetLevel()), RankName(threat), player->GetDistance(threat), escape.standing ?
                            " [it keeps to its place: out of its reach, then on with its task]" : stoodAgain ?
                            " [it keeps to its place, and the bot's task led it into its reach the third time in three minutes]" :
                            rt.asideStood ? " [it keeps to its place, and the bot's way ends inside its reach or finds no way around it]" : "");
                return Result::Aside;
            }
        }
    }

    if (!settings.escapeEnable || followsMaster || !player->IsAlive() || !player->IsInCombat() || now < rt.noEscapeUntil ||
        !CanRun(player))
    {
        escape.leaveTicks = 0;
        return Result::None;
    }

    std::vector<Creature*> foes = FoesOf(player);
    // Nothing is on the bot (the fight is over but for the game's own count of it): there is no fight to leave for
    // a strong creature near; out of combat it steps aside. (A warrior "left a fight against 0 foes at 55% health".)
    if (threatNear && foes.empty())
    {
        escape.leaveTicks = 0;
        return Result::None;
    }
    // The strong creature near is the one ordinary foe it fights: the fight's own numbers decide, not its being near.
    // (Thousand Needles, round 220: a level-30 shaman attacked by a level-34 Scorpid Terror left the fight at 87 to 99 %
    // health six times in twenty minutes and lost 6 to 50 % on the runs; the fights it stayed in against them it won at
    // 45 to 92 %.)
    // (And an elite that is strong only by its health and damage, StrongByLevel: Crimson Bodyguards, round 226.)
    if (threatNear && foes.size() == 1 && foes.front()->GetGUID() == threatGuid && !StrongByLevel(player, foes.front()))
        threatNear = false;
    // Beside an escort's NPC that fights the same fight the bot stays while it has more than a third of its health, a
    // strong creature near or not (WardFightsBeside: leaving ends the escort).
    if (player->GetHealthPct() > WardStayHealthPct && WardFightsBeside(player, rt))
    {
        escape.leaveTicks = 0;
        return Result::None;
    }
    // A foe that runs for help: what it fetches counts now, while the bot can still walk away from one fleeing foe.
    uint32 const fetched = AddFetched(player, foes);
    float need = 0.0f;
    bool byFight = false;
    if (!threatNear)
    {
        bool byCount = false;
        float measuredRate = 0.0f;
        if (!ShouldLeave(player, rt, foes, need, byCount, byFight, measuredRate))
        {
            escape.leaveTicks = 0;
            return Result::None;
        }
        // (By the number of foes it leaves at once: every second with three foes at its back is a tenth of its health.)
        if (++escape.leaveTicks < TicksToLeave && !byCount)
            return Result::None;
        // The creature it is about to kill comes first: with one or two on it and its target nearly dead, the kill
        // takes a foe out within seconds, and a run from there is the worse choice (a rogue at 39 % health left two
        // gnolls with its target at 5 % and died on the run ten seconds later; at 27 % it left a whelp at 8 % and
        // was dead after six. Counted over a morning: a fight against one foe that a bot stayed in below 40 % health
        // was won 11 times of 13; of 47 runs begun below 60 % health 14 ended in a death).
        constexpr float FinishBelowPct = 20.0f;
        if (!byCount && foes.size() <= 2)
        {
            Unit const* victim = player->GetVictim();
            if (!victim)
                victim = ObjectAccessor::GetUnit(*player, player->GetTarget());
            if (victim && victim->IsAlive() && victim->IsInCombat() && player->IsValidAttackTarget(victim) &&
                victim->GetHealthPct() <= FinishBelowPct && player->GetExactDist(victim) <= 30.0f)
            {
                // Only while the kill comes on: a target that does not lose health for some seconds is no kill within
                // seconds (a rogue held fast by a Frost Nova stood 28 s beside a hunter at 11-20 % that shot it from
                // out of reach, with two more on it, and died: "its target at 20% health").
                constexpr uint32 FinishStallMs = 6 * IN_MILLISECONDS;
                BotRuntime::Fight& fight = rt.fight;
                float const pct = victim->GetHealthPct();
                if (fight.finishGuid != victim->GetGUID())
                {
                    fight.finishGuid = victim->GetGUID();
                    fight.finishLow = pct;
                    fight.finishLowAt = now;
                    fight.finishLapsed = false;
                }
                else if (pct < fight.finishLow - 0.5f)
                {
                    fight.finishLow = pct;
                    fight.finishLowAt = now;
                }
                if (getMSTimeDiff(fight.finishLowAt, now) < FinishStallMs)
                {
                    escape.leaveTicks = 0;
                    return Result::None;
                }
                if (!fight.finishLapsed)
                {
                    fight.finishLapsed = true;
                    LOG_INFO(ActivityLog, "activity: {} no longer finishes {} first: it has stayed at {}% health for {} s", rt.name,
                        victim->GetName(), uint32(pct), FinishStallMs / IN_MILLISECONDS);
                }
            }
        }
        // What a foe takes per second: this fight's own number when it is the higher one.
        float const healthRate = std::max(rt.fightHealthRate, measuredRate);

        // The run itself is part of the estimate, and the bot runs only when that is better than fighting (a level-15
        // rogue with three gnolls at its back died within 6-10 seconds of every run, eight times in 22 minutes; a
        // level-16 rogue ran from two level-14 foes at full health and lost half of it on the way). It stays
        //   - when what protects it turns the fight (unless the number of foes alone says it is lost), or
        //   - when it would not live through the run and the run costs not clearly less than the fight;
        // then it casts what protects it and fights on.
        bool const getaway = GetawayHelps(player, foes);
        float const run = RunNeed(player, rt, foes, healthRate) * (getaway ? GetawayShare : 1.0f) * rt.runFactor;
        BotRuntime::Escape stand;
        // What protects it counts only when the cast goes out (a priest without mana has no shield).
        bool stay = false;
        // (Not when the run costs clearly less: a rogue stayed with three murlocs at 86 % health, "the fight would
        // cost it about 74%, the run about 9% (used Evasion)", was at 30 % when the Evasion ran out, and died.)
        if (!byCount && HasReadyHelp(player, HelpKind::Defense) && need * DefenseShare <= Budget(player) * settings.escapeMargin &&
            run >= need * DefenseShare * RunIfBelowFightShare)
        {
            CastHelp(player, stand, foes, true);
            stay = !stand.helps.empty();
        }
        float const fight = need * (stay ? DefenseShare : 1.0f);
        // It does not turn its back when it would not live through the run, whatever the fight costs: in one live
        // window all six deaths were bots on the run (a rogue at 46, 36, 32 and 26 % health with two or three
        // creatures at its back lived five to nine seconds each time). It fights on with what it has; what is
        // lost either way at least takes a creature with it, and a fight sometimes turns.
        constexpr float RunSurviveShare = 0.9f;
        bool const runLethal = run >= player->GetHealthPct() / 100.0f * RunSurviveShare;
        if (!stay && runLethal)
        {
            stay = true;
            CastHelp(player, stand, foes, true);
        }
        if (stay)
        {
            rt.noEscapeUntil = now + StandMs;
            escape.leaveTicks = 0;
            LOG_INFO(ActivityLog, "activity: {} stays in a fight against {} foe{} at {}% health: the fight would cost it about {:.0f}%{}, "
                "the run about {:.0f}%{}{}{}{}", rt.name, foes.size(), foes.size() == 1 ? "" : "s", uint32(player->GetHealthPct()),
                std::min(fight, 9.99f) * 100.0f, byFight ? " by how it goes" : "", std::min(run, 9.99f) * 100.0f,
                runLethal ? " (it would not live through it)" : "",
                getaway ? "" : ", nothing to get away with",
                stand.helps.empty() ? std::string() : " (used " + stand.helps + ")",
                fetched ? Acore::StringFormat(" ({} of the foes are what a fleeing one is fetching)", fetched) : std::string());
            return Result::None;
        }
    }

    escape = BotRuntime::Escape();
    escape.active = true;
    escape.startedAt = now;
    escape.mapId = player->GetMapId();
    escape.fromX = player->GetPositionX();
    escape.fromY = player->GetPositionY();
    escape.healthStart = uint8(player->GetHealthPct());
    escape.byFight = byFight;
    escape.threatEntry = threatNear ? threatEntry : 0;
    escape.threatLevel = threatLevel;
    escape.fetched = uint8(std::min<uint32>(fetched, 255));
    escape.runEstimate = RunNeed(player, rt, foes, rt.fightHealthRate) * (GetawayHelps(player, foes) ? GetawayShare : 1.0f);
    escape.foes = uint8(std::min<std::size_t>(foes.size(), 255));
    for (Creature const* foe : foes)
        if (foe->GetLevel() > escape.foeLevel || !escape.foeEntry)
        {
            escape.foeEntry = foe->GetEntry();
            escape.foeLevel = foe->GetLevel();
        }
    if (Pet* pet = player->GetPet(); pet && pet->GetReactState() == REACT_DEFENSIVE)
    {
        escape.petWasDefensive = true;
        pet->SetReactState(REACT_PASSIVE);
    }

    BotDriver::PauseAI(player, PauseMs);
    StopFighting(player);
    // The way first, then what helps on it. (The spells went out first: a paladin whose fight a level-32 elite stood
    // near "left" it four times in five minutes at 92 to 99 % health with one foe or none, cast Divine Protection,
    // Lay on Hands and Hammer of Justice, found "no open way" each time and fought on without them.)
    bool const hasGetaway = HasReadyHelp(player, HelpKind::Getaway);
    if (!StartLeg(player, rt, escape, foes))
    {
        // No way out: the fight goes on.
        rt.noEscapeUntil = now + FailedPauseMs;
        End(player, rt, "no way out, fights on");
        return Result::Failed;
    }
    if (!escape.firstPast || hasGetaway)
        CastHelp(player, escape, foes);
    // A Feign Death went out: the hunter lies down where it is, the leg it began would stand it up.
    if (player->HasAura(FeignDeathSpell))
    {
        player->GetMotionMaster()->Clear();
        player->StopMoving();
        return Result::Running;
    }
    // Every direction leads past further creatures and nothing was cast that shakes the foes off: running there
    // only adds to them (a level-12 paladin left two troggs at 34 % health, "of 7 directions 7 led past other
    // creatures", and was dead seven seconds later). It stays and fights with what it has: what protects it when
    // the fight is one to fear (two or more on it, or its health going), nothing otherwise.
    if (escape.firstPast && !escape.getawayCast)
    {
        constexpr float StandHelpBelowHealth = 60.0f;
        player->GetMotionMaster()->Clear();
        BotRuntime::Escape stand;
        if (foes.size() >= 2 || player->GetHealthPct() < StandHelpBelowHealth)
            CastHelp(player, stand, foes, true);
        rt.noEscapeUntil = now + StandMs;
        End(player, rt, "no open way, fights on");
        return Result::Failed;
    }
    LOG_DEBUG("module", "mod-guild-adventure: {} leaves a fight ({} foes, the rest of it would cost {:.0f}% of its health, it has {}%)",
        rt.name, foes.size(), need * 100.0f, uint32(escape.healthStart));
    return Result::Running;
}

float Escape::FoeCost(BotRuntime const& rt, uint8 botLevel, int32 levelsAbove, uint32 entry)
{
    int32 const diff = std::clamp<int32>(levelsAbove, -5, 5);
    return EntryCost(rt, botLevel, entry) * (diff >= 0 ? std::pow(CostPerLevelAbove, float(diff)) : std::pow(CostPerLevelBelow, float(-diff)));
}

std::string Escape::DescribeFoeCosts(BotRuntime const& rt)
{
    std::vector<std::pair<float, uint32>> costly;
    for (auto const& [entry, note] : rt.foeCost)
        if ((note.fights >= FoeNoteFights || note.cost >= FoeNoteAtOnce) && note.cost >= rt.fightCost * 1.3f && note.cost >= 0.3f)
            costly.push_back({ note.cost, entry });
    std::sort(costly.begin(), costly.end(), std::greater<>());
    std::string text;
    for (std::size_t i = 0; i < costly.size() && i < 4; ++i)
    {
        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(costly[i].second);
        text += Acore::StringFormat("{}{} {:.0f}%", text.empty() ? "" : ", ", info ? info->Name : "?", costly[i].first * 100.0f);
    }
    return text;
}

std::string Escape::StepDefend(Player* player, BotRuntime& rt)
{
    // A level-24 rogue's pulls of two orcs three levels below it took all its health twice in two minutes; its
    // Evasion went out when it turned to run, where it dodges nothing. With two creatures on it the bot casts what
    // protects it at the start of the fight.
    BotRuntime::Fight& fight = rt.fight;
    if (!fight.active || fight.defended || rt.escape.active || !player->IsAlive() || !player->IsInCombat() ||
        player->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING))
        return "";
    uint32 onIt = 0;
    for (Unit const* attacker : player->getAttackers())
        if (attacker->IsAlive() && attacker->IsCreature() && player->GetExactDist(attacker) <= 10.0f)
            ++onIt;
    if (onIt < 2)
        return "";
    static constexpr uint32 Early[] = { EvasionSpell, 22812 /*Barkskin*/, 498 /*Divine Protection*/, 871 /*Shield Wall*/, 11426 /*Ice Barrier*/ };
    for (uint32 const first : Early)
    {
        uint32 const spell = HighestKnownRank(player, first);
        if (!spell || player->HasSpellCooldown(spell) || (first == 498 && KeepsForbearanceFree(player)))
            continue;
        if (player->CastSpell(player, spell, false) != SPELL_CAST_OK)
            return ""; // the global cooldown, a form, a cast: the next look
        fight.defended = true;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        fight.defence = info ? info->SpellName[0] : "a spell";
        return fight.defence;
    }
    fight.defended = true; // nothing it has is ready
    return "";
}

// The core's Creature::CanAssistTo, check by check, but for CanSeeOrDetect: the call for help goes out when the creature
// engages, and by then the bot has attacked and is out of stealth. A cat druid prowls within 30 yd of its target before a
// fight (mod-playerbots' prowl trigger) and a rogue sneaks up, so asked while they still crept, the creatures beside
// the target did not count: Thunderhoof (49, feral) pulled a Southsea Dock Worker with Faerie Fire "1 may still come",
// two Dock Workers 5 yd from it were on it two seconds later, then six, and it died on the run (2026-11-26).
bool Escape::WouldAssist(Creature const* creature, Unit const* ally, Unit const* enemy)
{
    if (!creature || !ally || !enemy || !creature->HasReactState(REACT_AGGRESSIVE) || !creature->IsAlive() ||
        creature->IsInEvadeMode() || creature->IsCivilian())
        return false;
    if (enemy->IsCreature() && enemy->ToCreature()->IsInEvadeMode())
        return false;
    if (creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE) || creature->IsImmuneToNPC() ||
        creature->IsEngaged() || creature->GetCharmerOrOwnerGUID() ||
        creature->HasFlagsExtra(CREATURE_FLAG_EXTRA_IGNORE_ALL_ASSISTANCE_CALLS))
        return false;
    if (creature->GetFaction() != ally->GetFaction() || !creature->RespondsToCallForHelp())
        return false;
    return creature->IsHostileTo(enemy);
}

uint32 Escape::StrengthOfCreature(Creature const* creature)
{
    return StrengthOf(creature);
}

uint32 Escape::ThreatLevelOfKind(CreatureTemplate const* info, uint32 level)
{
    return ThreatLevelOf(info, level);
}

namespace
{
    bool KeepsForbearanceFree(Player const* player)
    {
        constexpr uint32 ForbearanceAura = 25771;
        if (player->getClass() != CLASS_PALADIN || player->HasAura(ForbearanceAura))
            return false;
        for (uint32 const first : { 642u /*Divine Shield*/, 633u /*Lay on Hands*/ })
            if (uint32 const spell = HighestKnownRank(player, first); spell && !player->HasSpellCooldown(spell))
                return true;
        return false;
    }
}

bool Escape::StepBack(Player* player, BotRuntime& rt)
{
    // mod-playerbots' mage casts Frost Nova when the creature is on it and then walks backwards five yards at a
    // time until it is out of melee reach, about ten yards; the creature is on it again a second and a half after
    // the root ends, and the rest of the fight is cast in melee (a level-23 mage lost 44 % of its health to one
    // creature a level above it; in a fight in which it could Blink it lost 19 %).
    constexpr float RangeYards = 24.0f;      // from the rooted creature: well inside the 30 yards of its spells
    constexpr float WhenWithinYards = 14.0f;
    constexpr int32 MinRootMs = 3 * IN_MILLISECONDS;
    constexpr uint32 LookAgainMs = 2 * IN_MILLISECONDS;
    uint32 const now = getMSTime();
    if (!player->IsAlive() || !player->IsInCombat() || rt.escape.active || now < rt.nextStepBack || player->IsMounted() || !CanRun(player))
        return false;
    switch (player->getClass())
    {
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        case CLASS_PRIEST:
        case CLASS_HUNTER:
            break;
        case CLASS_DRUID:
            if (player->GetShapeshiftForm() != FORM_NONE && player->GetShapeshiftForm() != FORM_MOONKIN)
                return false;
            break;
        default:
            return false;
    }

    // Everything that is on it and near must be held, for a few seconds more; one of them close. (Not a creature that
    // fights with spells: rooted or not, it casts at the bot from where it stands, and the run only costs the bot the
    // seconds of it and the cast it breaks off. A level-60 mage ran from a rooted Shadowmage to 24 yards, its bolts
    // went on, and its own first spell landed ten seconds into the fight, round 148.)
    Creature* held = nullptr;
    for (Unit* attacker : player->getAttackers())
    {
        Creature* creature = attacker->ToCreature();
        if (!creature || !creature->IsAlive())
            continue;
        if (creature->GetCreatureTemplate() && creature->GetCreatureTemplate()->unit_class == CLASS_MAGE)
            continue;
        float const distance = player->GetExactDist(creature);
        if (!creature->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED))
        {
            if (distance < RangeYards)
                return false; // it would simply follow
            continue;
        }
        int32 left = 0;
        for (AuraType type : { SPELL_AURA_MOD_ROOT, SPELL_AURA_MOD_STUN })
            for (AuraEffect const* effect : creature->GetAuraEffectsByType(type))
                left = std::max(left, effect->GetBase()->GetDuration());
        if (left < MinRootMs)
            return false;
        if (distance < WhenWithinYards && (!held || distance < player->GetExactDist(held)))
            held = creature;
    }
    rt.nextStepBack = now + LookAgainMs;
    if (!held)
        return false;

    float const need = RangeYards - player->GetExactDist(held);
    std::vector<Creature*> others = LiveHostiles(player, 60.0f, PackLevelsBelow);
    std::erase_if(others, [](Creature* creature) { return creature->IsInCombat(); });
    float const away = held->GetAngle(player);
    for (float const delta : { 0.0f, 0.5f, -0.5f, 1.0f, -1.0f })
    {
        float x = player->GetPositionX() + std::cos(away + delta) * need;
        float y = player->GetPositionY() + std::sin(away + delta) * need;
        float z = player->GetPositionZ();
        player->UpdateAllowedPositionZ(x, y, z);
        if (CountAlong(others, player, x, y) || Escape::WaterAt(player, x, y, z) || !CanWalkTo(player, x, y, z, need) ||
            !held->IsWithinLOS(x, y, z + 2.0f))
            continue;
        player->InterruptNonMeleeSpells(false);
        player->GetMotionMaster()->Clear();
        player->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_RUN);
        uint32 const ms = uint32(need / std::max(player->GetSpeed(MOVE_RUN), 1.0f) * float(IN_MILLISECONDS)) + 300;
        // mod-playerbots must not turn it round or start a cast on the way.
        BotDriver::PauseAI(player, ms);
        rt.nextStepBack = now + ms + 500;
        if (rt.fight.active && rt.fight.stepsBack < 255)
            ++rt.fight.stepsBack;
        return true;
    }
    return false;
}

bool Escape::StepWakes(Player* player, float x, float y, Creature** nearest)
{
    // The reach: the creature's aggro range (on this server half of the usual one) and a margin, for the creature's
    // own steps and the fight's: mod-playerbots backs off five yards at a time while its melee foe follows, and a
    // mage drifted 22 to 27 yards that way into three more Kurzen of their camp, 22 to 29 seconds into a fight it
    // had pulled alone (Mageyo, round 108).
    constexpr float MarginYards = 6.0f;
    constexpr float MinReachYards = 12.0f;
    constexpr float LookYards = 50.0f;
    if (nearest)
        *nearest = nullptr;
    float closest = 0.0f;
    for (Creature* creature : LiveHostiles(player, LookYards, PackLevelsBelow))
    {
        if (creature->IsInCombat())
            continue;
        float const reach = std::max(MinReachYards, creature->GetAggroRange(player) + MarginYards);
        float const there = creature->GetExactDist2d(x, y);
        if (there > reach || there >= creature->GetExactDist2d(player->GetPositionX(), player->GetPositionY()))
            continue;
        if (!nearest)
            return true;
        if (!*nearest || there < closest)
        {
            *nearest = creature;
            closest = there;
        }
    }
    // A strong civilian does not come by itself (LiveHostiles leaves civilians out), but a step to within an area
    // spell's reach of it lets the next nova or cone hit it, and it fights then (Twilight Marauder Morna, round 192).
    if (Creature* bystander = StrongBystanderWithin(player, x, y, MinReachYards);
        bystander && bystander->IsCivilian() &&
        bystander->GetExactDist2d(x, y) < bystander->GetExactDist2d(player->GetPositionX(), player->GetPositionY()))
    {
        if (!nearest)
            return true;
        if (!*nearest || bystander->GetExactDist2d(x, y) < closest)
            *nearest = bystander;
    }
    return nearest && *nearest;
}

Creature* Escape::StrongBystanderWithin(Player* player, float x, float y, float yards)
{
    return BystanderWithin(player, x, y, yards, uint32(player->GetLevel()) + ThreatLevels);
}

Creature* Escape::BystanderWithin(Player* player, float x, float y, float yards, uint32 minStrength)
{
    // Searched around the bot, wide enough to hold the area around (x, y).
    float const look = player->GetExactDist2d(x, y) + yards;
    std::list<Unit*> units;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, look);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, units, check);
    Cell::VisitObjects(player, searcher, look);
    Creature* nearest = nullptr;
    float closest = 0.0f;
    for (Unit* unit : units)
    {
        Creature* creature = unit->ToCreature();
        if (!creature || !creature->IsAlive() || creature->IsPet() || creature->IsTotem() || creature->IsInEvadeMode() ||
            creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_IMMUNE_TO_PC) ||
            // (Neutral creatures too: an area spell or a chain hits them as well, and they fight what hits them. Fozruk,
            // a neutral level-42 giant on a long round with three companions, walked past a level-32 paladin's fight
            // and five were on it two seconds later, round 242.)
            creature->IsFriendlyTo(player) || !player->IsValidAttackTarget(creature) || creature->IsInCombatWith(player) ||
            creature->IsCritter() || (minStrength && StrengthOf(creature) < minStrength))
            continue;
        float const there = creature->GetExactDist2d(x, y);
        if (there > yards)
            continue;
        if (!nearest || there < closest)
        {
            nearest = creature;
            closest = there;
        }
    }
    return nearest;
}

float Escape::OthersShareOf(Player const* player)
{
    float const learned = player ? Risk::PairShare(player->getClass()) : -1.0f;
    return learned >= 0.0f ? std::clamp(learned, 0.0f, 1.5f) : OthersShare;
}

float Escape::FetchedShare(Player const* player)
{
    switch (player->getClass())
    {
        case CLASS_HUNTER:
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        case CLASS_PRIEST:
            return 0.2f;
        default:
            return 0.35f;
    }
}

float Escape::FoeCostAlone(Player const* player, BotRuntime const& rt, uint8 botLevel, int32 levelsAbove, uint32 entry)
{
    float const cost = FoeCost(rt, botLevel, levelsAbove, entry);
    if (!OwnsPet(player))
        return cost;
    int32 const diff = std::clamp<int32>(levelsAbove, -5, 5);
    return std::max(cost, PriorCost(botLevel) * (diff >= 0 ? std::pow(CostPerLevelAbove, float(diff)) : std::pow(CostPerLevelBelow, float(-diff))));
}

void Escape::OnFightEnd(Player const* player, BotRuntime& rt, uint8 foes, uint8 foeLevel, uint8 healthStart, uint8 healthLow,
    uint8 manaStart, uint8 manaEnd, bool died, uint32 seconds, uint32 foeEntry, bool oneKind, bool foesLost,
    std::vector<std::pair<uint32, uint8>> const& kinds)
{
    if (!foeLevel)
        return;
    // A fight in which its foes lost nothing teaches nothing about what they cost: the bot could not get at them (a
    // warrior in a lake below a caster on the bank died with the caster at full health and "learned" that its kind
    // costs 130 %, the test guild, 2026-11-24).
    if (!foesLost)
        return;
    // A fight the bot left teaches nothing, unless it left it by how the fight went against the foes it had pulled (not
    // for a strong creature that came near, not a keep-away): then the rest of the fight would have cost more than it
    // had, and it counts as costing all of it, as a death counts. A level-17 priest pulled two Dreadbone Sentinels
    // ("2 expected"; one cost it 12 % on average), left at 27 % health with the first at 54 %, rested and pulled the next
    // two a minute later by the same estimate and died to them on the run (the test guild, 2026-11-24; neither the run
    // nor the death on it had taught it anything). A death on such a run counts as the death it is.
    if (died && rt.runDeathUnfoughtAt && getMSTimeDiff(rt.runDeathUnfoughtAt, getMSTime()) < 5 * IN_MILLISECONDS)
    {
        rt.runDeathUnfoughtAt = 0;
        return;
    }
    bool const left = rt.escape.active;
    if (left && (!rt.escape.byFight || rt.escape.threatEntry || rt.escape.aside || rt.escape.walkAway || !foeEntry ||
        rt.escape.foeEntry != foeEntry))
        return;
    float const lostSample = died ? 1.2f : 1.0f;
    // Two foes at once (ordinary ones) teach the bot's class what a second foe costs on top of two single fights, as a
    // share of the cheaper foe by what the bot knew of each kind before the fight (Escape::OthersShareOf; the owner's option
    // A, 2026-11-26). A fight it lost or left by how it went counts as costing all it had. Its own value: the mean of its
    // first five such fights, then a running average.
    if (foes == 2 && kinds.size() == 2)
    {
        uint8 const level = uint8(player->GetLevel());
        float costs[2] = { 0.0f, 0.0f };
        bool plain = true;
        for (std::size_t i = 0; i < 2; ++i)
        {
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(kinds[i].first);
            if (!info || (info->rank != CREATURE_ELITE_NORMAL && info->rank != CREATURE_ELITE_RARE))
                plain = false;
            costs[i] = EntryCost(rt, level, kinds[i].first) * LevelFactor(player, kinds[i].second);
        }
        float const cheaper = std::min(costs[0], costs[1]);
        if (plain && cheaper >= 0.02f)
        {
            float sample = lostSample;
            if (!died && !left)
            {
                sample = float(std::max<int32>(0, int32(healthStart) - int32(healthLow))) / 100.0f;
                if (UsesMana(player))
                    sample += ManaWeight * float(std::max<int32>(0, int32(manaStart) - int32(manaEnd))) / 100.0f;
            }
            float const share = std::clamp((sample - costs[0] - costs[1]) / cheaper, -0.5f, 3.0f);
            float const rate = rt.pairFights < 5 ? 1.0f / float(rt.pairFights + 1) : 0.15f;
            rt.pairShare += rate * (share - rt.pairShare);
            if (rt.pairFights < 65535)
                ++rt.pairFights;
            Risk::SavePairShare(player->GetGUID(), player->getClass(), rt.pairShare, rt.pairFights);
        }
    }
    // Two or three creatures of one kind at once teach what that kind costs, by the estimate's own sum (two of a kind
    // are 2.5 single fights, three 4.5: OthersShare): some kinds never stand alone. The Shadowmages of the Eastern
    // Plaguelands stand in pairs; no fight against one of them taught a bot anything, so each pair was judged by the
    // bot's average, about half of what it had; a level-60 warrior died to such pairs on 2026-11-18 twice and pulled
    // another pair at 95 % health four days later and died again, beside the place where another warrior of the guild
    // had died to a pair four minutes before, round 163. Its average is not touched by such a fight.
    if (foes >= 2 && foes <= 3 && oneKind && foeEntry)
    {
        float sample = lostSample;
        if (!died && !left)
        {
            sample = float(std::max<int32>(0, int32(healthStart) - int32(healthLow))) / 100.0f;
            if (UsesMana(player))
                sample += ManaWeight * float(std::max<int32>(0, int32(manaStart) - int32(manaEnd))) / 100.0f;
        }
        float const together = float(foes) + OthersShareOf(player) * float(foes) * float(foes - 1) / 2.0f;
        sample = sample / together / LevelFactor(player, foeLevel);
        BotRuntime::FoeNote& note = rt.foeCost[foeEntry];
        float const rate = note.fights < 3 ? 1.0f / float(note.fights + 1) : LearnRate;
        float const learned = std::clamp(note.cost + rate * (sample - note.cost), 0.0f, MaxFoeNoteCost);
        note.cost = died || left ? std::max(note.cost, std::min(sample, MaxFoeNoteCost)) : learned;
        if (note.fights < 65535)
            ++note.fights;
        if (died || left)
            CountSevere(note);
        Risk::SaveFoeCost(player->GetGUID(), foeEntry, note.cost, note.fights, player->getClass());
        if (died || left)
        {
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(foeEntry);
            LOG_INFO("module.ga.activity", "activity: {} {} {} x {} (level {}): from now on it counts {} of them at once as about "
                "{:.0f}% of what it has", player->GetName(), died ? "died to" : "had to leave a fight against", uint32(foes),
                info ? info->Name : "?", uint32(foeLevel), uint32(foes), note.cost * LevelFactor(player, foeLevel) * together * 100.0f);
        }
        return;
    }
    // Two or three creatures of different kinds at once teach each kind its share: the fight's cost against what the
    // estimate said (FightNeed's own sum, each kind by what the bot knows of it), the same factor for every kind of it.
    // A Shadowmage of the Eastern Plaguelands stands with a Dark Adept as often as with another Shadowmage: none of
    // those fights taught a bot anything, the pull check judged the pair by what a Shadowmage pair and the bot's
    // average said (about 64 % for a warrior that had lost all it had to two Shadowmages), and Warriorio (60) pulled
    // such a pair five times in forty minutes, went down 13 to 45 % in the fights it won and died in the sixth,
    // round 238. Its average is not touched by such a fight; an elite among them, whose share is not one of its kind's
    // single fights, makes the fight teach nothing.
    if (foes >= 2 && foes <= 3 && !oneKind && kinds.size() == foes)
    {
        uint8 const level = uint8(player->GetLevel());
        struct Share
        {
            uint32 entry;
            uint8 level;
            float estimate; // what the bot reckoned one of them costs it, with its level
        };
        std::vector<Share> shares;
        for (auto const& [entry, kindLevel] : kinds)
        {
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(entry);
            if (!info || (info->rank != CREATURE_ELITE_NORMAL && info->rank != CREATURE_ELITE_RARE))
                return;
            shares.push_back({ entry, kindLevel, EntryCost(rt, level, entry) * LevelFactor(player, kindLevel) });
        }
        std::sort(shares.begin(), shares.end(), [](Share const& a, Share const& b) { return a.estimate < b.estimate; });
        float estimate = 0.0f;
        float standing = float(shares.size());
        for (Share const& share : shares)
        {
            estimate += share.estimate * (1.0f + OthersShareOf(player) * (standing - 1.0f));
            standing -= 1.0f;
        }
        if (estimate <= 0.0f)
            return;
        float sample = lostSample;
        if (!died && !left)
        {
            sample = float(std::max<int32>(0, int32(healthStart) - int32(healthLow))) / 100.0f;
            if (UsesMana(player))
                sample += ManaWeight * float(std::max<int32>(0, int32(manaStart) - int32(manaEnd))) / 100.0f;
        }
        float const ratio = sample / estimate;
        std::vector<uint32> done;
        std::string names;
        for (Share const& share : shares)
        {
            if (std::find(done.begin(), done.end(), share.entry) != done.end())
                continue;
            done.push_back(share.entry);
            float const implied = std::min(share.estimate * ratio / LevelFactor(player, share.level), MaxFoeNoteCost);
            BotRuntime::FoeNote& note = rt.foeCost[share.entry];
            float const rate = note.fights < 3 ? 1.0f / float(note.fights + 1) : LearnRate;
            float const learned = std::clamp(note.cost + rate * (implied - note.cost), 0.0f, MaxFoeNoteCost);
            note.cost = died || left ? std::max(note.cost, implied) : learned;
            if (note.fights < 65535)
                ++note.fights;
            if (died || left)
                CountSevere(note);
            Risk::SaveFoeCost(player->GetGUID(), share.entry, note.cost, note.fights, player->getClass());
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(share.entry);
            names += (names.empty() ? "" : ", ") + std::string(info ? info->Name : "?");
        }
        if (died || left)
        {
            // What the same creatures together would count as now.
            float now = 0.0f;
            standing = float(shares.size());
            std::vector<float> parts;
            for (Share const& share : shares)
                parts.push_back(EntryCost(rt, level, share.entry) * LevelFactor(player, share.level));
            std::sort(parts.begin(), parts.end());
            for (float part : parts)
            {
                now += part * (1.0f + OthersShareOf(player) * (standing - 1.0f));
                standing -= 1.0f;
            }
            LOG_INFO("module.ga.activity", "activity: {} {} {} at once ({}): from now on it counts them together as about {:.0f}% "
                "of what it has (it had reckoned {:.0f}%)", player->GetName(), died ? "died to" : "had to leave a fight against",
                uint32(foes), names, now * 100.0f, estimate * 100.0f);
        }
        return;
    }
    // Only fights against one foe teach the cost of one foe.
    if (foes != 1)
        return;
    float sample = lostSample;
    if (!died && !left)
    {
        sample = float(std::max<int32>(0, int32(healthStart) - int32(healthLow))) / 100.0f;
        if (UsesMana(player))
            sample += ManaWeight * float(std::max<int32>(0, int32(manaStart) - int32(manaEnd))) / 100.0f;
    }
    sample /= LevelFactor(player, foeLevel);
    // What this kind of creature costs it: the mean of its first three fights with the kind, then a running average;
    // a death never lowers it.
    bool ownKind = false;
    if (foeEntry)
    {
        BotRuntime::FoeNote& note = rt.foeCost[foeEntry];
        float const rate = note.fights < 3 ? 1.0f / float(note.fights + 1) : LearnRate;
        float const learned = std::clamp(note.cost + rate * (sample - note.cost), 0.0f, MaxFoeNoteCost);
        note.cost = died || left ? std::max(note.cost, std::min(sample, MaxFoeNoteCost)) : learned;
        if (note.fights < 65535)
            ++note.fights;
        if (died || left)
            CountSevere(note);
        Risk::SaveFoeCost(player->GetGUID(), foeEntry, note.cost, note.fights, player->getClass());
        // (A fight it left alive teaches that kind, not its average or its pace.)
        if (left && !died)
            return;
        // A kind that costs it clearly more than its fights do speaks for itself from here on: it no longer raises
        // the average (a mage's average of 58 % was its whelps; its boars cost it 8 %).
        ownKind = (note.fights >= FoeNoteFights || note.cost >= FoeNoteAtOnce) && note.cost >= rt.fightCost * 1.5f && note.cost >= 0.5f;
        if (ownKind && !note.told)
        {
            note.told = true;
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(foeEntry);
            LOG_INFO("module.ga.activity", "activity: {} has learned that a fight with {} costs it about {:.0f}% (its fights cost {:.0f}% "
                "on average): it judges that kind by itself from now on", player->GetName(), info ? info->Name : "?", note.cost * 100.0f,
                rt.fightCost * 100.0f);
        }
    }
    if (!ownKind)
        rt.fightCost = std::clamp(rt.fightCost + LearnRate * (sample - rt.fightCost), MinFightCost, MaxFightCost);
    // The health alone, per second (a run costs no mana, and a foe at the bot's back hits as long as the run lasts).
    float const rate = float(std::max<int32>(0, int32(healthStart) - int32(healthLow))) / 100.0f / LevelFactor(player, foeLevel) /
        float(std::max<uint32>(seconds, 5));
    rt.fightHealthRate = std::clamp(rt.fightHealthRate + LearnRate * (rate - rt.fightHealthRate), MinHealthRate, MaxHealthRate);
    ++rt.fightsLearned;
    Risk::SaveFightStats(player->GetGUID(), rt);
}
