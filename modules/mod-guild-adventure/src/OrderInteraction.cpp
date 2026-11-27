/*
 * OrderService, quest interactions: the step of a TaskKind::Interact task (docs/orders.md "Quest interactions").
 * What to do is planned by QuestInteraction (QuestInteraction.cpp); this file walks the bot there, picks the target,
 * steps into range, acts and checks the progress. Kept apart from OrderService.cpp, which only plans the task.
 */

#include "OrderService.h"
#include "BotSettings.h"
#include "Creature.h"
#include "GameObject.h"
#include "GuildAdventureMgr.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "RiskModel.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "QuestEvents.h"
#include "QuestInteraction.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "LootService.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <list>

using namespace GuildAdventure;

namespace
{
    // The activity log (OrderService.cpp has the same logger).
    constexpr char const* ActivityLog = "module.ga.activity";
    // The planner takes over this close to the spot; NewRpg walks the long way.
    constexpr float InteractSpotDistance = 25.0f;
    // ... and only within this height of a spawn's spot: a bot on the hill above a mine is not at the crates inside it.
    constexpr float InteractClimb = 10.0f;
    constexpr float InteractDiveDistance = 60.0f;   // a spot under the water this near (2D) is dived to, not walked to

    // The bot is in the water and the point lies under its surface (a dive gets there straight through the water).
    bool SwimsAbove(Player* player, float x, float y, float z)
    {
        Map* map = player->GetMap();
        float const height = player->GetCollisionHeight();
        return map->IsUnderWater(player->GetPhaseMask(), x, y, z, height) &&
            map->IsInWater(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), height);
    }
    // Targets are looked for this far around the bot.
    constexpr float SightDistance = 60.0f;
    // After an act: this long for the credit (scripts react a few seconds after the cast).
    constexpr uint32 InteractResultMs = 6 * IN_MILLISECONDS;
    // This many acts in a row without progress are a failure (A21).
    constexpr uint8 InteractMaxMisses = 5;
    // A creature target that is not in sight: wait this long for a respawn before trying another spawn.
    constexpr uint32 TargetWaitMs = 45 * IN_MILLISECONDS;
    // The last yards to one more target after the first act: this long, then the next target.
    constexpr uint32 InteractApproachMs = MINUTE * IN_MILLISECONDS;
    // Upper bound for one interaction task.
    constexpr uint32 InteractTaskMaxMs = 15 * MINUTE * IN_MILLISECONDS;
    // Re-assert the walk at most this often.
    constexpr uint32 AssertGapMs = 5 * IN_MILLISECONDS;
    // A spot that did not work is skipped this long (one that cannot be reached: AvoidSpawnMs).
    constexpr uint32 RetryMs = 2 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 AvoidSpawnMs = 20 * MINUTE * IN_MILLISECONDS;
    // A follower's aid: the follower must need it this long (a short pause between two follow moves is no need) ...
    constexpr uint32 AidNeedMs = 2 * IN_MILLISECONDS;
    // ... it is used again after this long if the follower did not recover (the spell has a cast time of up to 4.5 s) ...
    constexpr uint32 AidRetryMs = 8 * IN_MILLISECONDS;
    // ... and this many uses without recovery are enough: the lead's own time limits take over.
    constexpr uint8 AidMaxUses = 6;
    // Its chest is opened at most this often.
    constexpr uint8 AidMaxFetches = 3;

    float Dist2d(Player const* player, SpawnPoint const& spot)
    {
        float const dx = player->GetPositionX() - spot.x;
        float const dy = player->GetPositionY() - spot.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    // A spawn is set aside by its id; a POI spot has none, so the area around it is.
    void AvoidSpot(BotRuntime& rt, SpawnPoint const& spot, uint32 durationMs)
    {
        if (spot.spawnId)
            rt.Avoid(spot, durationMs);
        else
            rt.AddAvoidArea(spot.mapId, spot.x, spot.y, 255, durationMs, false);
    }

    // Off the mount for a use: item spells, casts and opening a chest fail while mounted (SPELL_FAILED_NOT_MOUNTED).
    void DismountForUse(Player* player)
    {
        if (!player->IsMounted())
            return;
        player->Dismount();
        player->RemoveAurasByType(SPELL_AURA_MOUNTED);
    }

    std::string QuestTitle(uint32 questId)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        return quest ? quest->GetTitle() : Acore::StringFormat("quest {}", questId);
    }
}

bool OrderService::TrapRefused(Player* player, BotRuntime const& rt, GameObject const* object) const
{
    GameObjectTemplate const* info = object ? object->GetGOInfo() : nullptr;
    uint32 const trapId = info ? info->GetLinkedGameObjectEntry() : 0;
    if (!trapId)
        return false;
    GameObjectTemplate const* trap = sObjectMgr->GetGameObjectTemplate(trapId);
    return trap && trap->type == GAMEOBJECT_TYPE_TRAP && trap->trap.spellId && !Risk::JudgeSummon(player, rt, trap->trap.spellId).usable;
}

bool OrderService::StepInteraction(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    uint32 const now = getMSTime();
    BotTask& task = rt.task;
    InteractionPlan const& plan = task.plan;
    // The corpses of an item used on a slain creature are not skinned meanwhile (auto loot, the gather detour): a skinned
    // corpse is gone at once. Thrakgar (43, Skinning 306) killed 96 Firemane dragonkin for "The Essence of Enmity" in four
    // hours and found a corpse for Brogg's Totem 19 times, 2026-11-27. (Refreshed at every step, also in its fights.)
    if (plan.targetDead && plan.target == InteractionTarget::Creature)
    {
        constexpr uint32 KeepCorpsesMs = 90 * IN_MILLISECONDS;
        QuestInteraction::KeepCorpses(player->GetGUID(), plan.targetEntries, KeepCorpsesMs);
    }

    Quest const* quest = sObjectMgr->GetQuestTemplate(task.questId);
    QuestStatus const status = player->GetQuestStatus(task.questId);
    if (!quest || status != QUEST_STATUS_INCOMPLETE)
    {
        ClearTask(rt, 0, status == QUEST_STATUS_COMPLETE ? "quest complete" : "quest no longer open");
        return false;
    }
    auto statusItr = player->getQuestStatusMap().find(task.questId);
    if (statusItr == player->getQuestStatusMap().end())
    {
        ClearTask(rt, 0, "quest no longer open");
        return false;
    }
    QuestStatusData const& progress = statusItr->second;
    uint32 have = 0;
    uint32 need = 1;
    if (plan.kind == InteractionKind::Explore)
        have = progress.Explored ? 1 : 0;
    else if (task.objective >= 0 && task.objective < QUEST_OBJECTIVES_COUNT)
    {
        have = progress.CreatureOrGOCount[task.objective];
        need = quest->RequiredNpcOrGoCount[task.objective];
    }
    else if (task.objective >= QUEST_OBJECTIVES_COUNT && task.objective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
    {
        have = progress.ItemCount[task.objective - QUEST_OBJECTIVES_COUNT];
        need = quest->RequiredItemCount[task.objective - QUEST_OBJECTIVES_COUNT];
    }
    if (have >= need)
    {
        ClearTask(rt, 0, "objective done");
        return false;
    }
    // An item that brings the creature may be used up by its use (the Horn of Echeyakee): the minute for the creature
    // still runs.
    if (plan.kind == InteractionKind::UseItem && !player->HasItemCount(plan.itemId, 1) &&
        !(plan.bringsCreature && task.actedAt && getMSTimeDiff(task.actedAt, now) < MINUTE * IN_MILLISECONDS))
    {
        ClearTask(rt, 30 * IN_MILLISECONDS, "the quest item is gone");
        return false;
    }

    auto fail = [&](char const* why)
    {
        LOG_INFO(ActivityLog, "activity: {} quest {} \"{}\" interaction failed ({}): {}, {} acts, progress {}/{}", bot.name,
            task.questId, QuestTitle(task.questId), why, plan.describe, uint32(task.acts), have, need);
        AvoidSpot(rt, task.spot, RetryMs);
        FailQuest(guild, bot, player, task.questId, "interaction_failed");
        ClearTask(rt, 5 * IN_MILLISECONDS, why);
    };

    if (getMSTimeDiff(task.startedAt, now) > InteractTaskMaxMs)
    {
        fail("took too long");
        return false;
    }

    // A cast is still under way (the act's own or one the bot started): its result comes afterwards.
    if (player->IsNonMeleeSpellCast(false))
    {
        if (task.actedAt)
        {
            task.actedAt = now;
            // A channel the act started gives its credit only at its end, and a step or another cast of the bot's AI
            // breaks it: the Ooze Buster of "Corrosion Prevention" channels 5 s on the ooze (Ooze Channel, cancelled by
            // moving), and Warlockdot fought and walked through every channel after the first (1 of 10 in 7 minutes).
            // The AI waits while it runs; the bot stands.
            if (player->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            {
                BotDriver::PauseAI(player, 1500);
                if (player->isMoving())
                    player->StopMoving();
            }
        }
        return true;
    }

    // The result of the last act: progress, or one more miss.
    if (task.actedAt)
    {
        // The creature an item brings walks up first (Zelfrax comes along a path from the water): a minute for it. It is
        // attacked once it is there: a Stonemaul Spirit stands and talks and attacks nobody, and Retadin stood beside it
        // until the owner took over and hit it ("Spirits of Stonemaul Hold", 2026-11-14).
        if (have == task.lastHave && getMSTimeDiff(task.actedAt, now) < (plan.bringsCreature ? MINUTE * IN_MILLISECONDS : InteractResultMs))
        {
            uint32 const broughtEntry = plan.brought ? plan.brought : (task.objective >= 0 && task.objective < QUEST_OBJECTIVES_COUNT &&
                quest->RequiredNpcOrGo[task.objective] > 0) ? uint32(quest->RequiredNpcOrGo[task.objective]) : 0;
            if (plan.bringsCreature && !player->IsInCombat() && broughtEntry)
            {
                Creature* brought = player->FindNearestCreature(broughtEntry, SightDistance, true);
                if (brought && player->IsValidAttackTarget(brought))
                {
                    if (task.approachTarget != brought->GetGUID())
                    {
                        task.approachTarget = brought->GetGUID();
                        task.approachSince = now;
                        LOG_INFO(ActivityLog, "activity: {} quest {}: attacks {} its item brought ({:.1f} yd)", bot.name, task.questId,
                            brought->GetName(), player->GetDistance(brought));
                    }
                    if (!BotDriver::Attack(player, brought))
                        player->Attack(brought, true);
                    if (!player->IsWithinMeleeRange(brought) && now - task.lastAssert >= 2 * IN_MILLISECONDS)
                    {
                        player->GetMotionMaster()->MoveChase(brought);
                        task.lastAssert = now;
                    }
                }
            }
            return true;
        }
        if (have > task.lastHave)
        {
            task.misses = 0;
            task.spellIndex = 0;
            if (task.actTarget && std::find(task.usedTargets.begin(), task.usedTargets.end(), task.actTarget) == task.usedTargets.end())
                task.usedTargets.push_back(task.actTarget);
            LOG_INFO(ActivityLog, "activity: {} quest {} progress {}/{} ({})", bot.name, task.questId, have, need, plan.describe);
        }
        else
            ++task.misses;
        task.actedAt = 0;
        uint32 const maxMisses = plan.kind == InteractionKind::CastSpells ?
            std::max<uint32>(InteractMaxMisses, uint32(task.spells.size()) + 1) : InteractMaxMisses;
        if (task.misses >= maxMisses)
        {
            fail("no progress");
            return false;
        }
    }
    task.lastHave = have;

    // Walk to the spot; NewRpg does the long way, the planner the last yards.
    if (!task.nearSince)
    {
        float const distance = Dist2d(player, task.spot);
        // A POI spot's height is the ground's surface (GetHeight from above), so only a spawn's height counts.
        bool const otherLevel = task.spot.spawnId && std::fabs(player->GetPositionZ() - task.spot.z) > InteractClimb;
        // A spot under the water the bot swims in (the Giant Clams of "Pearl Diving" on the sea floor, 12 to 110 yd down):
        // no walk finds it, the last yards dive (below). Again swam above the clams for 24 minutes ("spot not reached",
        // "no target in sight", a rescue), 0 of 9 pearls.
        bool const diveTo = otherLevel && distance <= InteractDiveDistance && SwimsAbove(player, task.spot.x, task.spot.y, task.spot.z);
        if ((distance > InteractSpotDistance || otherLevel) && !diveTo)
        {
            // A spot on another level near by (a cave under the bot's feet, a ledge above it): the planner walks it there
            // itself along the core's whole path, a stretch at a time. NewRpg's walk does not get down: a druid stood 19
            // and 6 yd from Scrimshank's Surveying Gear in the Gaping Chasm of Tanaris for 239 s and 388 s ("go camp").
            constexpr float OtherLevelOwnYards = 80.0f;
            if (otherLevel && distance <= OtherLevelOwnYards && !task.ownSteps && !task.otherLevelLooked && !player->IsInCombat())
            {
                task.otherLevelLooked = true;
                float chain = 0.0f;
                if (ChainReaches(player, task.spot.x, task.spot.y, task.spot.z, chain) && chain <= std::max(400.0f, distance * 8.0f))
                {
                    task.ownSteps = true;
                    task.ownRounds = 3; // (it keeps them: no stretch back to NewRpg)
                    task.ownFromX = player->GetPositionX();
                    task.ownFromY = player->GetPositionY();
                    task.ownFailStreak = 0;
                    task.ownStepAt = 0;
                    LOG_INFO(ActivityLog, "activity: {} walks to the spot of quest {} itself: it lies {:.0f} yd {} it, {:.0f} yd away; the "
                        "path finder's way there is {:.0f} yd", bot.name, task.questId, std::fabs(task.spot.z - player->GetPositionZ()),
                        task.spot.z < player->GetPositionZ() ? "below" : "above", distance, chain);
                }
            }
            if (UpdateStuck(player, rt, task.spot.x, task.spot.y))
            {
                LOG_INFO("module", "mod-guild-adventure: {} cannot get to the spot of quest {} \"{}\" ({:.0f} yd left)", bot.name,
                    task.questId, QuestTitle(task.questId), distance);
                AvoidSpot(rt, task.spot, AvoidSpawnMs);
                NoteSpotUnreached(player, task);
                ClearTask(rt, 5 * IN_MILLISECONDS, "spot not reached");
                return false;
            }
            // Along the road map like an objective's walk, and not past a group the bot cannot take or creatures far above
            // it (what lives at the spot itself is the interaction's business): Hunterbm swam through five Greymist murlocs
            // off Auberdine on the way to the Cliffspring River Waterfall ("The Cliffspring River") and died.
            WalkTo(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z);
            SpawnPoint const spot = task.spot;
            if (LeaveDangerousWalk(bot, player, rt, sBotSettings.Get(player, Setting::WalksMaxLevelsAbove), true))
            {
                AvoidSpot(rt, spot, uint32(sBotSettings.Get(player, Setting::WalksPostponeMinutes)) * MINUTE * IN_MILLISECONDS);
                return false;
            }
            return true;
        }
        task.nearSince = now;
        task.lastAssert = 0;
        BotDriver::Rest(player);
        if (plan.kind == InteractionKind::CastSpells)
        {
            task.spells.clear();
            for (uint32 spellId : plan.castSpells)
                if (player->HasSpell(spellId))
                    task.spells.push_back(spellId);
            if (plan.castSpells.empty())
                task.spells = QuestInteraction::HelpfulSpells(player);
        }
        LOG_INFO(ActivityLog, "activity: {} at the spot of quest {}: {}", bot.name, task.questId, plan.describe);
    }

    // Talk, lead, fight: its own steps (the credit comes with the kill, long after the talk).
    if (plan.kind == InteractionKind::Provoke)
        return StepProvoke(bot, player, rt);

    // A creature to weaken first: its own step (also called in a fight, where the planner does not run the task).
    if (StepWeaken(player, rt))
        return true;

    // A fight at the spot: playerbots fights it, and the interaction goes on afterwards. Its point movements to the
    // anchor or the trigger would pull the bot away from its foe, and an act in a fight is wasted (a corpse plan's own
    // pull is a fight too: the use follows when the creature is dead).
    if (player->IsInCombat())
    {
        rt.why = Acore::StringFormat("Fighting at the spot of \"{}\"", QuestTitle(task.questId));
        return true;
    }

    // Exploration: into the area trigger, then report it the way the client does (CMSG_AREATRIGGER).
    if (plan.kind == InteractionKind::Explore)
    {
        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(plan.areaTrigger);
        if (!trigger)
        {
            fail("area trigger unknown");
            return false;
        }
        if (!player->IsInAreaTriggerRadius(trigger))
        {
            if (now - task.lastAssert >= 2 * IN_MILLISECONDS)
            {
                // (To the ground in the trigger the planner found, else its middle.)
                bool const inside = task.hasSpot && trigger->radius > 0.0f &&
                    std::sqrt((task.spot.x - trigger->x) * (task.spot.x - trigger->x) + (task.spot.y - trigger->y) * (task.spot.y - trigger->y) +
                        (task.spot.z - trigger->z) * (task.spot.z - trigger->z)) < trigger->radius;
                if (inside)
                    player->GetMotionMaster()->MovePoint(0, task.spot.x, task.spot.y, task.spot.z);
                else
                    player->GetMotionMaster()->MovePoint(0, trigger->x, trigger->y, trigger->z);
                task.lastAssert = now;
            }
            if (getMSTimeDiff(task.nearSince, now) > 2 * MINUTE * IN_MILLISECONDS)
                fail("cannot get into the area");
            return true;
        }
        WorldPacket packet(CMSG_AREATRIGGER, 4);
        packet << uint32(plan.areaTrigger);
        player->GetSession()->HandleAreaTriggerOpcode(packet);
        task.actedAt = now;
        ++task.acts;
        LOG_INFO(ActivityLog, "activity: {} entered area trigger {} for quest {}", bot.name, plan.areaTrigger, task.questId);
        return true;
    }

    auto unused = [&](ObjectGuid guid)
    {
        return std::find(task.usedTargets.begin(), task.usedTargets.end(), guid) == task.usedTargets.end();
    };

    // How far it looks: a spawn spot far below it (a Giant Clam 40 to 110 yd down on the sea floor, the bot at the surface
    // above it) is looked for that much deeper; at 60 yd the deep clams were never in sight.
    float const lookFar = SightDistance + (task.spot.spawnId ? std::max(0.0f, player->GetPositionZ() - task.spot.z) : 0.0f);
    // A creature released by an object (the Servants of "The Stones That Bind Us"): out of a fight the bot attacks one
    // (as the AI's own attack does, with a chase); in the fight StepWeaken brings it to its line and uses its object.
    if (!plan.beatEntries.empty() && !player->IsInCombat())
    {
        Creature* foe = nullptr;
        std::list<Creature*> creatures;
        player->GetCreatureListWithEntryInGrid(creatures, plan.beatEntries, lookFar);
        for (Creature* creature : creatures)
            if (creature->IsAlive() && !creature->IsInCombat() && unused(creature->GetGUID()) && player->IsValidAttackTarget(creature) &&
                (!foe || player->GetDistance(creature) < player->GetDistance(foe)))
                foe = creature;
        if (!foe)
        {
            uint32 const since = task.acts ? getMSTimeDiff(task.lastAssert, now) : getMSTimeDiff(task.nearSince, now);
            if (since > TargetWaitMs)
            {
                AvoidSpot(rt, task.spot, RetryMs);
                ClearTask(rt, 5 * IN_MILLISECONDS, task.acts ? "nothing left to act on here" : "no target in sight");
                return false;
            }
            return true;
        }
        if (task.approachTarget != foe->GetGUID())
        {
            task.approachTarget = foe->GetGUID();
            task.approachSince = now;
            LOG_INFO(ActivityLog, "activity: {} quest {}: attacks {} to {}", bot.name, task.questId, foe->GetName(), plan.describe);
        }
        else if (getMSTimeDiff(task.approachSince, now) > 30 * IN_MILLISECONDS)
        {
            task.usedTargets.push_back(foe->GetGUID());
            task.approachTarget.Clear();
            player->AttackStop();
            return true;
        }
        if (!BotDriver::Attack(player, foe))
            player->Attack(foe, true);
        if (!player->IsWithinMeleeRange(foe) && now - task.lastAssert >= 2 * IN_MILLISECONDS)
        {
            // A chase that stands still on a ledge above its foe: the chase takes the foe as near enough, the swing does
            // not (Bloodyo stood 4 yd above a Servant of Grol for 30 s, out of its sight). After a few seconds the bot walks
            // to the foe's own place along the core's path instead.
            if (getMSTimeDiff(task.approachSince, now) > 4 * IN_MILLISECONDS)
                player->GetMotionMaster()->MovePoint(0, foe->GetPositionX(), foe->GetPositionY(), foe->GetPositionZ());
            else
                player->GetMotionMaster()->MoveChase(foe);
            task.lastAssert = now;
        }
        return true;
    }
    // What the act goes to (target) and what the bot stands next to (anchor).
    WorldObject* target = nullptr;
    WorldObject* anchor = nullptr;
    // The item works only near a creature (the Ooze Buster near a Zeppelin Power Core): the bot stands within its reach
    // (StandNear: on the line from that creature to the target), so only targets that are near one are taken.
    std::list<Creature*> nearOnes;
    if (plan.nearCreature)
        player->GetCreatureListWithEntryInGrid(nearOnes, plan.nearCreature, SightDistance + plan.nearRange);
    float const standFromNear = NearStandYards;
    auto nearOneOf = [&](WorldObject const* of) -> Creature*
    {
        Creature* found = nullptr;
        float best = standFromNear + plan.range - 4.0f;
        for (Creature* one : nearOnes)
            if (one->IsInWorld() && of->GetDistance(one) < best)
            {
                best = of->GetDistance(one);
                found = one;
            }
        return found;
    };
    if (plan.target == InteractionTarget::Creature)
    {
        std::list<Creature*> creatures;
        player->GetCreatureListWithEntryInGrid(creatures, plan.targetEntries, lookFar);
        float best = lookFar + 1.0f;
        Creature* living = nullptr;
        float bestLiving = lookFar + 1.0f;
        for (Creature* creature : creatures)
        {
            if (!creature->IsInWorld() || !player->CanSeeOrDetect(creature) || !unused(creature->GetGUID()))
                continue;
            if (plan.nearCreature && !nearOneOf(creature))
                continue;
            float const distance = player->GetDistance(creature);
            // (Not one the spell's conditions refuse: a Lazy Peon that is awake and at work.)
            bool const auraFits =
                std::all_of(plan.targetAuras.begin(), plan.targetAuras.end(), [&](uint32 aura) { return creature->HasAura(aura); }) &&
                std::none_of(plan.targetNoAuras.begin(), plan.targetNoAuras.end(), [&](uint32 aura) { return creature->HasAura(aura); });
            if (creature->IsAlive() != plan.targetDead && distance < best && auraFits)
            {
                best = distance;
                target = creature;
            }
            else if (plan.targetDead && creature->IsAlive() && distance < bestLiving &&
                player->IsValidAttackTarget(creature) && !creature->IsInCombat())
            {
                bestLiving = distance;
                living = creature;
            }
        }
        // The item wants a corpse and none is here: kill one first (the combat AI takes over once the fight starts). The
        // bot walks up to it (Attack() alone does not: Retadin "attacked" a Firemane Ash Tail once a second for four
        // minutes without a fight, "The Essence of Enmity"); one no fight comes of within 30 s is passed.
        // The creature the pull check turned the bot to (its selection: one that stands alone) is kept when it is one of
        // the plan's: the two must not hand the target back and forth (Retadin stood at "The Essence of Enmity" with
        // "You are too far away!" while the interaction set another Firemane once a second).
        if (!target && !player->IsInCombat())
            if (Creature* chosen = ObjectAccessor::GetCreature(*player, player->GetTarget()))
                if (chosen != living && chosen->IsAlive() && unused(chosen->GetGUID()) && !chosen->IsInCombat() &&
                    player->IsValidAttackTarget(chosen) && player->GetDistance(chosen) <= SightDistance &&
                    std::find(plan.targetEntries.begin(), plan.targetEntries.end(), chosen->GetEntry()) != plan.targetEntries.end())
                    living = chosen;
        if (!target && living && !player->IsInCombat())
        {
            if (task.approachTarget != living->GetGUID())
            {
                task.approachTarget = living->GetGUID();
                task.approachSince = now;
                LOG_INFO(ActivityLog, "activity: {} quest {}: attacks {} to use the item on its corpse", bot.name, task.questId,
                    living->GetName());
            }
            else if (getMSTimeDiff(task.approachSince, now) > 30 * IN_MILLISECONDS)
            {
                LOG_INFO(ActivityLog, "activity: {} quest {}: no fight with {} after 30 s: the next one", bot.name, task.questId,
                    living->GetName());
                task.usedTargets.push_back(living->GetGUID());
                task.approachTarget.Clear();
                task.approachSince = 0;
                player->AttackStop();
                return true;
            }
            // As the AI's own attack does (its current target and the combat engine): the AI walks up and fights. The
            // chase covers a target out of its sight for the moment.
            if (!BotDriver::Attack(player, living))
                player->Attack(living, true);
            if (!player->IsWithinMeleeRange(living) && now - task.lastAssert >= 2 * IN_MILLISECONDS)
            {
                player->GetMotionMaster()->MoveChase(living);
                task.lastAssert = now;
            }
            return true;
        }
        anchor = target;
    }
    else if (plan.target == InteractionTarget::GameObject)
    {
        std::list<GameObject*> objects;
        player->GetGameObjectListWithEntryInGrid(objects, plan.targetEntries, lookFar);
        float best = lookFar + 1.0f;
        for (GameObject* object : objects)
        {
            if (!object->isSpawned() || !object->IsInWorld() || !unused(object->GetGUID()))
                continue;
            // A quest object that is not active for the bot is left alone. A chest is not: its loot may hold the quest's item
            // as ordinary loot, and the core activates a chest for a quest only for loot flagged as quest loot (the Giant
            // Clams of "Pearl Diving" drop their Blue Pearl at 100 % without the flag; every clam was skipped as "no target").
            if (plan.kind == InteractionKind::UseObject && object->GetGoType() != GAMEOBJECT_TYPE_CHEST && !object->ActivateToQuest(player))
                continue;
            // Not one whose trap summons what the bot cannot take: the planner chose a spawn without such a trap
            // (Risk::JudgeSummon), and the step took the nearest object in sight. Of the Kodo Bones of "Bone Collector"
            // one kind springs a Kodo Apparition of level 38: a level-31 shaman opened the trapped ones beside the spawn
            // it was sent to, fled from the Apparition twice and went home by hearthstone, 2026-11-25.
            if (plan.kind == InteractionKind::UseObject && TrapRefused(player, rt, object))
                continue;
            float const distance = player->GetDistance(object);
            if (distance < best)
            {
                best = distance;
                target = object;
            }
        }
        anchor = target;
    }
    else if (!plan.walkObjects.empty())
    {
        std::list<GameObject*> objects;
        player->GetGameObjectListWithEntryInGrid(objects, plan.walkObjects, SightDistance);
        float best = SightDistance + 1.0f;
        for (GameObject* object : objects)
        {
            float const distance = player->GetDistance(object);
            if (object->isSpawned() && distance < best)
            {
                best = distance;
                anchor = object;
            }
        }
    }

    bool const needsAnchor = plan.target != InteractionTarget::None || !plan.walkObjects.empty();
    if (needsAnchor && !anchor)
    {
        // Nothing (left) in sight here: wait a little for a creature to respawn or walk by, then another spawn of the
        // target. A game object does not walk: if none is here, this spawn is not active (pooled, used up).
        uint32 const since = task.acts ? getMSTimeDiff(task.lastAssert, now) : getMSTimeDiff(task.nearSince, now);
        uint32 const patience = plan.target == InteractionTarget::Creature ? TargetWaitMs : 5 * IN_MILLISECONDS;
        if (since > patience)
        {
            AvoidSpot(rt, task.spot, RetryMs);
            ClearTask(rt, 5 * IN_MILLISECONDS, task.acts ? "nothing left to act on here" : "no target in sight");
            return false;
        }
        return true;
    }

    // The last yards: into range and line of sight.
    if (anchor)
    {
        // Line of sight only to creatures: a game object's own model often blocks the ray to its centre.
        bool const blocked = anchor->ToUnit() && !player->IsWithinLOSInMap(anchor);
        // An object to use: the core's own test (Spell::CheckRange -> GameObject::IsAtInteractDistance: inside the
        // object's model box grown by the opening spell's range, its height included). By GetDistance alone a bot stood
        // 3 to 4 yd from an Ancient Statuette on the bottom of the Zoram Strand, from Sand Pears up in their trees and
        // from Fel Cones and Sacks of Oats, and the open-lock spell failed with SPELL_FAILED_OUT_OF_RANGE (97); Mageyo
        // tried the statuette for six minutes.
        // The same for an item whose spell goes on a game object (a key on a cage), with that spell's range.
        GameObject const* useObject = plan.kind == InteractionKind::UseObject ||
            (plan.kind == InteractionKind::UseItem && plan.target == InteractionTarget::GameObject) ? anchor->ToGameObject() : nullptr;
        bool outOfReach = useObject ? !useObject->IsAtInteractDistance(player,
            plan.kind == InteractionKind::UseItem ? sSpellMgr->GetSpellInfo(plan.spellId) : nullptr) : player->GetDistance(anchor) > plan.range;
        // Near a creature (StandNear): within its reach and within the item's range of the target, or the bot walks to a
        // point on the line from that creature to the target (standFromNear from it, less than the range from the target).
        Position standAt = anchor->GetPosition();
        if (plan.nearCreature)
        {
            Creature* nearOne = nearOneOf(anchor);
            if (!nearOne)
            {
                if (unused(anchor->GetGUID()))
                    task.usedTargets.push_back(anchor->GetGUID());
                return true;
            }
            outOfReach = outOfReach || player->GetDistance(nearOne) > plan.nearRange - 4.0f;
            float const span = nearOne->GetDistance2d(anchor);
            float const along = span > 0.1f ? std::min(standFromNear, span) / span : 0.0f;
            float const x = nearOne->GetPositionX() + (anchor->GetPositionX() - nearOne->GetPositionX()) * along;
            float const y = nearOne->GetPositionY() + (anchor->GetPositionY() - nearOne->GetPositionY()) * along;
            float const z = player->GetMap()->GetHeight(player->GetPhaseMask(), x, y, std::max(anchor->GetPositionZ(),
                nearOne->GetPositionZ()) + 10.0f);
            standAt.Relocate(x, y, z > INVALID_HEIGHT ? z : anchor->GetPositionZ());
            // In reach and in range, but the creature's aura is not on it (its periodic spell checks the line of sight
            // too: the power cores lie in a wreck): up to the creature itself and a few seconds there, else the next target.
            if (!outOfReach && !blocked && plan.nearAura && !player->HasAura(plan.nearAura))
            {
                if (player->GetDistance(nearOne) > 3.0f)
                {
                    if (now - task.lastAssert >= 2 * IN_MILLISECONDS)
                    {
                        player->GetMotionMaster()->MovePoint(0, nearOne->GetPositionX(), nearOne->GetPositionY(), nearOne->GetPositionZ());
                        task.lastAssert = now;
                        if (!task.auraWaitSince)
                            task.auraWaitSince = now;
                    }
                }
                else if (!task.auraWaitSince)
                    task.auraWaitSince = now;
                if (task.auraWaitSince && getMSTimeDiff(task.auraWaitSince, now) > 15 * IN_MILLISECONDS)
                {
                    LOG_INFO(ActivityLog, "activity: {} quest {}: does not get the aura {} near {} ({:.1f} yd): the next target",
                        bot.name, task.questId, plan.nearAura, nearOne->GetName(), player->GetDistance(nearOne));
                    if (unused(anchor->GetGUID()))
                        task.usedTargets.push_back(anchor->GetGUID());
                    task.auraWaitSince = 0;
                }
                return true;
            }
            if (plan.nearAura && player->HasAura(plan.nearAura))
                task.auraWaitSince = 0;
        }
        if (outOfReach || blocked)
        {
            if (now - task.lastAssert >= 2 * IN_MILLISECONDS)
            {
                // Far above or below (the crates in a mine under the bot): NewRpg's walk finds the way round, a point
                // movement would go straight through the ground (the ground guard refuses it).
                // NewRpg walks only in the Travel profile (task.leading): at the spot the task holds, and Warlockdot stood
                // six minutes at an emptied Lumber Pile with the next one 11 yd lower ("moved 0 yd in 5 min").
                // Down to a target under the water it swims in: straight through the water (NewRpg's walk stays at the
                // surface, the path finder has no way down).
                bool const dive = SwimsAbove(player, standAt.GetPositionX(), standAt.GetPositionY(), standAt.GetPositionZ());
                task.leading = !dive && std::fabs(player->GetPositionZ() - standAt.GetPositionZ()) > InteractClimb;
                if (dive)
                    player->GetMotionMaster()->MovePoint(0, standAt.GetPositionX(), standAt.GetPositionY(), standAt.GetPositionZ(),
                        FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false);
                else if (task.leading)
                    BotDriver::GoTo(player, anchor->GetMapId(), standAt.GetPositionX(), standAt.GetPositionY(), standAt.GetPositionZ(), false);
                else
                    player->GetMotionMaster()->MovePoint(0, standAt.GetPositionX(), standAt.GetPositionY(), standAt.GetPositionZ());
                task.lastAssert = now;
            }
            if (!task.acts && getMSTimeDiff(task.nearSince, now) > 3 * MINUTE * IN_MILLISECONDS)
            {
                AvoidSpot(rt, task.spot, RetryMs);
                ClearTask(rt, 5 * IN_MILLISECONDS, "cannot get close");
                return false;
            }
            // Each target gets a minute of its own: after the first act the task used to try one it could not reach
            // until its 15 minutes were over (Shammpower stood a quarter of an hour below a Sand Pear, "took too long").
            if (task.approachTarget != anchor->GetGUID())
            {
                task.approachTarget = anchor->GetGUID();
                task.approachSince = now;
            }
            else if (task.acts && getMSTimeDiff(task.approachSince, now) > InteractApproachMs)
            {
                LOG_INFO(ActivityLog, "activity: {} quest {}: cannot get close to {} ({:.1f} yd): the next one", bot.name, task.questId,
                    anchor->GetName(), player->GetDistance(anchor));
                if (unused(anchor->GetGUID()))
                    task.usedTargets.push_back(anchor->GetGUID());
                task.approachTarget.Clear();
                task.approachSince = 0;
                player->GetMotionMaster()->Clear(false);
                return true;
            }
            return true;
        }
        task.leading = false;
        if (player->isMoving())
        {
            player->StopMoving();
            return true;
        }
        player->SetFacingToObject(anchor);
    }

    // Act (off the mount: Hordlock used Umi's Mechanical Yeti mounted, SPELL_FAILED_NOT_MOUNTED, gaps thread's level sweep).
    DismountForUse(player);
    task.actTarget = target ? target->GetGUID() : ObjectGuid::Empty;
    std::string done;
    switch (plan.kind)
    {
        case InteractionKind::UseItem:
        {
            // A spell that makes an item needs room for it in the bags: the core refuses it quietly otherwise
            // (SPELL_FAILED_DONT_REPORT; Hunterbm used the Empty Sampling Tube five times with full bags, "no progress").
            // The bag sweep puts what the loot filter lets go into the guild storage; still no room: the bags first.
            if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(plan.spellId))
                for (SpellEffectInfo const& effect : spell->Effects)
                {
                    if ((effect.Effect != SPELL_EFFECT_CREATE_ITEM && effect.Effect != SPELL_EFFECT_CREATE_ITEM_2) || !effect.ItemType)
                        continue;
                    ItemPosCountVec dest;
                    if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, effect.ItemType, 1) == EQUIP_ERR_OK)
                        continue;
                    uint32 const swept = sLootService.SweepBags(player);
                    dest.clear();
                    if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, effect.ItemType, 1) == EQUIP_ERR_OK)
                    {
                        LOG_INFO(ActivityLog, "activity: {} quest {}: no room for the item its use makes; the bag sweep looked at {} "
                            "stacks and made room", bot.name, task.questId, swept);
                        continue;
                    }
                    LOG_INFO(ActivityLog, "activity: {} quest {}: no room in its bags for the item its use makes ({}): the quest "
                        "waits a quarter of an hour", bot.name, task.questId, plan.describe);
                    AvoidSpot(rt, task.spot, RetryMs);
                    BotRuntime::Danger& danger = rt.dangerousQuests[task.questId];
                    danger.level = player->GetLevel();
                    danger.until = now + 15 * MINUTE * IN_MILLISECONDS;
                    ClearTask(rt, 30 * IN_MILLISECONDS, "bags full");
                    return false;
                }
            // A creature to weaken first: the fight comes before the item (the step above uses it when the creature is
            // low enough).
            if (plan.weakenBelow && target && target->ToCreature() && target->ToCreature()->IsAlive())
            {
                Creature* creature = target->ToCreature();
                if (!creature->HasAuraState(plan.weakenBelow >= 35 ? AURA_STATE_HEALTHLESS_35_PERCENT : AURA_STATE_HEALTHLESS_20_PERCENT))
                {
                    // One that no fight comes of within 30 s of the walk up (it cannot be reached, it is not hostile to
                    // the bot): the next.
                    if (task.approachTarget != creature->GetGUID())
                    {
                        task.approachTarget = creature->GetGUID();
                        task.approachSince = now;
                    }
                    else if (getMSTimeDiff(task.approachSince, now) > 30 * IN_MILLISECONDS)
                    {
                        LOG_INFO(ActivityLog, "activity: {} quest {}: no fight with {} after 30 s: the next one", bot.name,
                            task.questId, creature->GetName());
                        task.usedTargets.push_back(creature->GetGUID());
                        task.approachTarget.Clear();
                        task.approachSince = 0;
                        player->AttackStop();
                        return true;
                    }
                    if (!BotDriver::Attack(player, creature))
                        player->Attack(creature, true);
                    player->GetMotionMaster()->MoveChase(creature);
                    task.lastAssert = now;
                    LOG_INFO(ActivityLog, "activity: {} quest {}: attacks {} to weaken it below {}% health for {}", bot.name,
                        task.questId, creature->GetName(), uint32(plan.weakenBelow), plan.describe);
                    return true;
                }
            }
            // Standing next to an object (spell focus, "near" condition): every other try names the object as target, for
            // spells that want it as their target after all.
            WorldObject* useOn = plan.target != InteractionTarget::None ? target : (task.acts % 2 == 1 ? anchor : nullptr);
            uint32 const result = QuestInteraction::UseItem(player, plan.itemId, plan.spellId, useOn);
            // The use started a channel (the Ooze Channel): the AI waits from the first moment (see the cast check above).
            bool const channels = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr;
            if (channels)
                BotDriver::PauseAI(player, 1500);
            done = Acore::StringFormat("used item {}{} (check {}{})", plan.itemId, useOn && !target ? " with the object as target" : "",
                result, channels ? ", it channels" : "");
            // One use per creature or object (it transforms, despawns or gives no second credit).
            if (target && unused(target->GetGUID()))
                task.usedTargets.push_back(target->GetGUID());
            break;
        }
        case InteractionKind::CastSpells:
        {
            if (task.spells.empty() || !target || !target->ToUnit())
            {
                fail("no helpful spell");
                return false;
            }
            if (task.spellIndex >= task.spells.size())
            {
                // Every spell tried on this one: the next creature.
                task.usedTargets.push_back(target->GetGUID());
                task.spellIndex = 0;
                return true;
            }
            uint32 const spellId = task.spells[task.spellIndex++];
            SpellCastResult const result = player->CastSpell(target->ToUnit(), spellId, false);
            done = Acore::StringFormat("cast {} (result {})", spellId, uint32(result));
            break;
        }
        case InteractionKind::UseObject:
        {
            GameObject* object = target ? target->ToGameObject() : nullptr;
            if (!object)
                return true;
            // An item objective: a chest that has nothing for the bot gets fresh loot at its next opening.
            uint32 const needed = task.objective >= QUEST_OBJECTIVES_COUNT &&
                task.objective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT ?
                quest->RequiredItemId[task.objective - QUEST_OBJECTIVES_COUNT] : 0;
            // The quest's item needs room in the bags: with none the loot window opens and nothing is taken, the object is
            // used up for the bot and the task ends "nothing left to act on here". Magefrost opened Wyrmtails 16 times in
            // round 120 with 0 free bag slots ("looted 0"). The bag sweep first; still no room: the quest waits a quarter
            // of an hour (its vendor trips and the bag sweep make room meanwhile).
            if (needed && object->GetGoType() == GAMEOBJECT_TYPE_CHEST)
            {
                ItemPosCountVec dest;
                if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, needed, 1) != EQUIP_ERR_OK)
                {
                    uint32 const swept = sLootService.SweepBags(player);
                    dest.clear();
                    if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, needed, 1) != EQUIP_ERR_OK)
                    {
                        LOG_INFO(ActivityLog, "activity: {} quest {}: no room in its bags for {} (the bag sweep looked at {} stacks "
                            "and made none): the quest waits a quarter of an hour", bot.name, task.questId,
                            sObjectMgr->GetItemTemplate(needed) ? sObjectMgr->GetItemTemplate(needed)->Name1 : std::to_string(needed), swept);
                        BotRuntime::Danger& danger = rt.dangerousQuests[task.questId];
                        danger.level = player->GetLevel();
                        danger.until = now + 15 * MINUTE * IN_MILLISECONDS;
                        ClearTask(rt, 5 * IN_MILLISECONDS, "bags full");
                        return false;
                    }
                }
            }
            std::string const how = QuestInteraction::UseGameObject(player, object);
            uint32 const taken = object->GetGoType() == GAMEOBJECT_TYPE_CHEST ? QuestInteraction::TakeObjectLoot(player, object, needed) : 0;
            done = Acore::StringFormat("used object, looted {} ({})", taken, how);
            task.usedTargets.push_back(object->GetGUID());
            break;
        }
        default:
            fail("nothing to do");
            return false;
    }
    task.actedAt = now;
    task.lastAssert = now;
    ++task.acts;
    LOG_INFO(ActivityLog, "activity: {} quest {}: {} on {} ({:.1f} yd), progress before the act {}/{}", bot.name, task.questId, done,
        target ? target->GetName() : (anchor ? anchor->GetName() : std::string("the spot")),
        anchor ? player->GetDistance(anchor) : 0.0f, have, need);
    return true;
}

bool OrderService::AidFollower(GuildBot const& bot, Player* player, BotRuntime& rt, Quest const* quest, Creature* follower)
{
    FollowerAid const* aid = quest && follower ? QuestInteraction::GetFollowerAid(quest, follower->GetEntry()) : nullptr;
    if (!aid)
        return false;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    BotTask& task = rt.task;
    ItemTemplate const* item = sObjectMgr->GetItemTemplate(aid->itemId);
    std::string const itemName = item ? item->Name1 : Acore::StringFormat("item {}", aid->itemId);

    // Busy casting (the aid's spell takes up to 4.5 s): stand still, NewRpg's walk would break the cast.
    if (player->IsNonMeleeSpellCast(false))
    {
        if (player->isMoving())
        {
            player->GetMotionMaster()->Clear(false);
            player->StopMoving();
        }
        if (BotDriver::GetRpgState(player) != RpgState::Rest)
            BotDriver::Rest(player);
        return true;
    }

    // Without the item: the chest it comes from, if one is in sight (the horn next to Kerlonian, the bell next to Shay).
    if (!player->HasItemCount(aid->itemId, 1))
    {
        InteractionPlan const* fetch = aid->itemObjective >= 0 ? QuestInteraction::Resolve(player, quest, aid->itemObjective) : nullptr;
        if (!fetch || fetch->kind != InteractionKind::UseObject || task.aidFetches >= AidMaxFetches)
            return false;
        GameObject* chest = nullptr;
        for (uint32 entry : fetch->targetEntries)
        {
            GameObject* object = player->FindNearestGameObject(entry, SightDistance);
            if (object && object->isSpawned() && (!chest || player->GetDistance(object) < player->GetDistance(chest)))
                chest = object;
        }
        if (!chest)
            return false;
        rt.why = Acore::StringFormat("Fetching {} from {} for \"{}\"", itemName, chest->GetName(), quest->GetTitle());
        if (player->GetDistance(chest) > fetch->range)
        {
            if (now - task.lastAssert >= 2 * IN_MILLISECONDS)
            {
                player->GetMotionMaster()->MovePoint(0, chest->GetPositionX(), chest->GetPositionY(), chest->GetPositionZ());
                task.lastAssert = now;
            }
            return true;
        }
        if (player->isMoving())
        {
            player->StopMoving();
            return true;
        }
        if (task.aidAt && now - task.aidAt < 3 * IN_MILLISECONDS)
            return true;
        DismountForUse(player);
        player->SetFacingToObject(chest);
        std::string const how = QuestInteraction::UseGameObject(player, chest);
        uint32 const taken = chest->GetGoType() == GAMEOBJECT_TYPE_CHEST ? QuestInteraction::TakeObjectLoot(player, chest, aid->itemId) : 0;
        ++task.aidFetches;
        task.aidAt = now;
        LOG_INFO(ActivityLog, "activity: {} quest {}: opened {} for {} ({}), looted {}, has it {}", bot.name, quest->GetQuestId(),
            chest->GetName(), itemName, how, taken, player->HasItemCount(aid->itemId, 1) ? "yes" : "no");
        return true;
    }

    if (!QuestInteraction::NeedsAid(follower))
    {
        if (task.aidNeedSince && task.aidUses)
            LOG_INFO(ActivityLog, "activity: {} quest {}: {} goes on after {} use(s) of {}", bot.name, quest->GetQuestId(),
                follower->GetName(), task.aidUses, itemName);
        task.aidNeedSince = 0;
        task.aidUses = 0;
        return false;
    }
    if (!task.aidNeedSince)
        task.aidNeedSince = now;
    if (now - task.aidNeedSince < AidNeedMs || task.aidUses >= AidMaxUses)
        return false;

    rt.why = Acore::StringFormat("Using {} on {} for \"{}\"", itemName, follower->GetName(), quest->GetTitle());
    if (player->GetDistance(follower) > aid->range || !player->IsWithinLOSInMap(follower))
    {
        if (now - task.lastAssert >= 2 * IN_MILLISECONDS)
        {
            player->GetMotionMaster()->MovePoint(0, follower->GetPositionX(), follower->GetPositionY(), follower->GetPositionZ());
            task.lastAssert = now;
        }
        return true;
    }
    if (player->isMoving())
    {
        player->StopMoving();
        return true;
    }
    if (task.aidAt && now - task.aidAt < AidRetryMs)
        return true;
    // Nothing may move the bot during the cast (the lead's walk, NewRpg's own moves), and items are not used mounted
    // (Shay's Bell failed with SPELL_FAILED_NOT_MOUNTED for a level 50 hunter that rode after Shay).
    DismountForUse(player);
    player->GetMotionMaster()->Clear(false);
    if (BotDriver::GetRpgState(player) != RpgState::Rest)
        BotDriver::Rest(player);
    player->SetFacingToObject(follower);
    uint32 const result = QuestInteraction::UseItem(player, aid->itemId, aid->spellId, follower);
    ++task.aidUses;
    task.aidAt = now;
    LOG_INFO(ActivityLog, "activity: {} quest {}: used {} on {} ({:.1f} yd, {}; check {})", bot.name, quest->GetQuestId(), itemName,
        follower->GetName(), player->GetDistance(follower),
        follower->getStandState() == UNIT_STAND_STATE_SLEEP ? "asleep" : "not following", result);
    return true;
}

// A creature that is released by an object (InteractionPlan::beatEntries, "The Stones That Bind Us"): in the fight with
// it, once it is at its line (invincible at 1 HP, it stuns itself) the bot lets go of it, walks to the nearest of the plan's
// objects within 40 yd of it (the object's spell reaches that far) and uses it; the creature dies of the object's spell
// and the bot that fought it gets the credit. Above the line the AI fights as it likes.
static bool StepReleaseObject(Player* player, BotRuntime& rt)
{
    BotTask& task = rt.task;
    InteractionPlan const& plan = task.plan;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    constexpr float ReleaseReach = 40.0f;
    std::list<Creature*> foes;
    player->GetCreatureListWithEntryInGrid(foes, plan.beatEntries, ReleaseReach);
    Creature* low = nullptr;
    for (Creature* foe : foes)
        if (foe->IsAlive() && foe->IsInCombat() && foe->GetHealthPct() <= float(plan.beatBelow) + 0.5f &&
            (!low || player->GetDistance(foe) < player->GetDistance(low)))
            low = foe;
    if (!low)
        return false;
    BotDriver::PauseAI(player, 2 * IN_MILLISECONDS);
    if (player->GetVictim())
        player->AttackStop();
    std::list<GameObject*> objects;
    low->GetGameObjectListWithEntryInGrid(objects, plan.targetEntries, ReleaseReach);
    GameObject* object = nullptr;
    for (GameObject* candidate : objects)
        if (candidate->isSpawned() && (!object || low->GetDistance(candidate) < low->GetDistance(object)))
            object = candidate;
    if (!object)
    {
        if (task.approachTarget != low->GetGUID())
        {
            task.approachTarget = low->GetGUID();
            LOG_INFO(ActivityLog, "activity: {} quest {}: no {} within {:.0f} yd of {} at {:.0f}% health", player->GetName(),
                task.questId, plan.describe, ReleaseReach, low->GetName(), low->GetHealthPct());
        }
        return false;
    }
    if (!object->IsAtInteractDistance(player))
    {
        if (now - task.lastAssert >= IN_MILLISECONDS)
        {
            player->GetMotionMaster()->MovePoint(0, object->GetPositionX(), object->GetPositionY(), object->GetPositionZ());
            task.lastAssert = now;
        }
        return true;
    }
    // Its use is under way (the object's spell, the creature's death).
    if (task.actedAt && getMSTimeDiff(task.actedAt, now) < 4 * IN_MILLISECONDS)
        return true;
    if (player->isMoving())
        player->StopMoving();
    std::string const how = QuestInteraction::UseGameObject(player, object);
    task.actedAt = now;
    task.lastAssert = now;
    task.actTarget = object->GetGUID();
    ++task.acts;
    LOG_INFO(ActivityLog, "activity: {} quest {}: uses {} on {} at {} health ({:.0f}%; {})", player->GetName(), task.questId,
        object->GetName(), low->GetName(), low->GetHealth(), low->GetHealthPct(), how);
    return true;
}

bool OrderService::StepWeaken(Player* player, BotRuntime& rt)
{
    BotTask& task = rt.task;
    InteractionPlan const& plan = task.plan;
    // A channel the act started runs on in a fight too (the zapped ooze comes for the bot): the AI waits, the bot stands,
    // or its first blow or step breaks the channel and the credit with it.
    if (task.kind == TaskKind::Interact && task.actedAt && player->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
    {
        BotDriver::PauseAI(player, 1500);
        if (player->isMoving())
            player->StopMoving();
        return true;
    }
    if (task.kind == TaskKind::Interact && plan.kind == InteractionKind::UseObject && !plan.beatEntries.empty())
        return player->IsAlive() && player->IsInCombat() && StepReleaseObject(player, rt);
    if (task.kind != TaskKind::Interact || plan.kind != InteractionKind::UseItem || !plan.weakenBelow ||
        plan.target != InteractionTarget::Creature || !player->IsAlive() || !player->IsInCombat())
        return false;
    // (Out of combat the interaction's own steps walk up and start the fight: a victim set by Attack() without a fight
    // held the bot for four minutes, Ropetest at a raptor it never reached.)
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    // A creature to weaken first (the item's spell works only below 20 or 35 % of its health: "capture a raptor"): the
    // fight with it is the interaction. Above the line plus 30 % the AI fights as it likes; closer to it the AI is held
    // and only white blows land (its spells would kill it); below the line no more blows, the item at once, and the AI
    // held a few seconds more so that it does not finish the creature while the credit comes.
    Unit* victim = player->GetVictim();
    Creature* foe = victim ? victim->ToCreature() : nullptr;
    if (foe && (!foe->IsAlive() || std::find(plan.targetEntries.begin(), plan.targetEntries.end(), foe->GetEntry()) == plan.targetEntries.end()))
        foe = nullptr;
    if (foe)
    {
        AuraStateType const line = plan.weakenBelow >= 35 ? AURA_STATE_HEALTHLESS_35_PERCENT : AURA_STATE_HEALTHLESS_20_PERCENT;
        if (foe->HasAuraState(line))
        {
            BotDriver::PauseAI(player, 6 * IN_MILLISECONDS); // the cast (0.5 s) and the credit the creature casts (4 s)
            player->AttackStop();
            if (Pet* pet = player->GetPet())
                pet->AttackStop();
            DismountForUse(player);
            player->SetFacingToObject(foe);
            uint32 const result = QuestInteraction::UseItem(player, plan.itemId, plan.spellId, foe);
            task.actTarget = foe->GetGUID();
            task.actedAt = now;
            task.lastAssert = now;
            ++task.acts;
            if (std::find(task.usedTargets.begin(), task.usedTargets.end(), foe->GetGUID()) == task.usedTargets.end())
                task.usedTargets.push_back(foe->GetGUID());
            LOG_INFO(ActivityLog, "activity: {} quest {}: used item {} on {} at {:.0f}% health ({:.1f} yd) (check {})", player->GetName(), task.questId,
                plan.itemId, foe->GetName(), foe->GetHealthPct(), player->GetDistance(foe), result);
            return true;
        }
        rt.why = Acore::StringFormat("Weakening {} for \"{}\"", foe->GetName(), QuestTitle(task.questId));
        if (foe->GetHealthPct() <= float(plan.weakenBelow) + 30.0f)
        {
            BotDriver::PauseAI(player, IN_MILLISECONDS);
            if (player->GetVictim() != foe || !player->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                player->Attack(foe, true);
            if (!player->IsWithinMeleeRange(foe) && now - task.lastAssert >= IN_MILLISECONDS)
            {
                player->GetMotionMaster()->MoveChase(foe);
                task.lastAssert = now;
            }
            if (Pet* pet = player->GetPet())
                if (pet->GetVictim() == foe)
                    pet->AttackStop();
        }
        return true;
    }
    return false;
}

bool OrderService::StepProvoke(GuildBot const& bot, Player* player, BotRuntime& rt)
{
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    BotTask& task = rt.task;
    InteractionPlan const& plan = task.plan;
    uint32 const entry = plan.targetEntries.empty() ? 0 : plan.targetEntries.front();
    std::string const title = QuestTitle(task.questId);

    // In a fight (the creature turned, or something else came): playerbots fights it, the kill gives the credit.
    if (player->IsInCombat())
    {
        rt.why = Acore::StringFormat("Fighting for \"{}\"", title);
        return true;
    }

    Creature* target = task.actTarget ? ObjectAccessor::GetCreature(*player, task.actTarget) : nullptr;
    if (!target || !target->IsAlive())
        target = player->FindNearestCreature(entry, SightDistance, true);
    if (!target)
    {
        if (getMSTimeDiff(task.nearSince, now) > TargetWaitMs)
        {
            AvoidSpot(rt, task.spot, RetryMs);
            ClearTask(rt, 5 * IN_MILLISECONDS, "no target in sight");
            return false;
        }
        rt.why = Acore::StringFormat("Waiting for {} (\"{}\")", task.targetName, title);
        return true;
    }
    task.actTarget = target->GetGUID();

    // It turned: at it.
    if (player->IsValidAttackTarget(target))
    {
        if (player->GetDistance(target) > 25.0f && now - task.lastAssert >= 2 * IN_MILLISECONDS)
        {
            player->GetMotionMaster()->MovePoint(0, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
            task.lastAssert = now;
        }
        else if (player->GetDistance(target) <= 25.0f)
        {
            DismountForUse(player);
            player->Attack(target, true);
        }
        rt.why = Acore::StringFormat("Fighting {} for \"{}\"", target->GetName(), title);
        return true;
    }

    bool const follows = target->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE;
    // NewRpg walks only in the Travel profile; at the creature the task holds (OrderService::Reconcile).
    task.leading = false;
    if (follows && plan.leadRange > 0.0f && plan.leadMap == player->GetMapId())
    {
        // Lead it to the place where it turns; wait when it falls behind.
        float const left = std::sqrt((player->GetPositionX() - plan.leadX) * (player->GetPositionX() - plan.leadX) +
            (player->GetPositionY() - plan.leadY) * (player->GetPositionY() - plan.leadY));
        if (player->GetDistance2d(target) > 20.0f || left <= std::max(2.0f, plan.leadRange - 3.0f))
        {
            if (player->isMoving())
            {
                player->GetMotionMaster()->Clear(false);
                player->StopMoving();
            }
            if (BotDriver::GetRpgState(player) != RpgState::Rest)
                BotDriver::Rest(player);
            rt.why = Acore::StringFormat(left <= plan.leadRange ? "Waiting at its place for {} to turn (\"{}\")" :
                "Waiting for {} to catch up (\"{}\")", target->GetName(), title);
            return true;
        }
        task.leading = true;
        WalkTo(player, rt, plan.leadMap, plan.leadX, plan.leadY, plan.leadZ);
        rt.why = Acore::StringFormat("Leading {} to its place for \"{}\"", target->GetName(), title);
        return true;
    }
    if (follows)
    {
        rt.why = Acore::StringFormat("Waiting for {} to turn (\"{}\")", target->GetName(), title);
        return true;
    }

    // Not started yet: talk to it like a client (the gossip choice of its script).
    if (task.aidAt && now - task.aidAt < 8 * IN_MILLISECONDS)
        return true;
    if (player->GetDistance(target) > INTERACTION_DISTANCE - 1.0f)
    {
        if (now - task.lastAssert >= 2 * IN_MILLISECONDS)
        {
            player->GetMotionMaster()->MovePoint(0, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
            task.lastAssert = now;
        }
        return true;
    }
    if (player->isMoving())
    {
        player->StopMoving();
        return true;
    }
    if (task.acts >= InteractMaxMisses)
    {
        LOG_INFO(ActivityLog, "activity: {} quest {} \"{}\": {} did not start after {} talks", bot.name, task.questId, title,
            target->GetName(), uint32(task.acts));
        AvoidSpot(rt, task.spot, RetryMs);
        ClearTask(rt, 5 * IN_MILLISECONDS, "it did not start");
        return false;
    }
    DismountForUse(player);
    player->SetFacingToObject(target);
    QuestEventPlan talk;
    talk.kind = QuestEventKind::Talk;
    talk.entry = entry;
    talk.menuId = plan.gossipMenu;
    talk.optionId = plan.gossipOption;
    std::string detail;
    TalkResult const result = sQuestEvents.Talk(player, target, talk, detail);
    ++task.acts;
    task.aidAt = now;
    LOG_INFO(ActivityLog, "activity: {} quest {}: talked to {} ({:.1f} yd): {}{}", bot.name, task.questId, target->GetName(),
        player->GetDistance(target), result == TalkResult::Chosen ? "chose" : (result == TalkResult::NoMenu ? "no menu" : "no way"),
        detail.empty() ? std::string() : " (" + detail + ")");
    rt.why = Acore::StringFormat("Starting {} for \"{}\"", target->GetName(), title);
    return true;
}
