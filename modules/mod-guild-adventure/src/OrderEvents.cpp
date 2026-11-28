/*
 * Quest events of the order planner: quests whose credit comes from a script event (docs/orders.md "Quest events").
 * Part of OrderService; kept in a file of its own. QuestEvents says what each quest needs; this file does it:
 *
 *   - Talk: walk to the NPC or game object, greet it and pick the options that lead to the choice, then wait for the
 *           credit (scripts hold speeches or send enemies first); the talk is tried three times.
 *   - Stay: the event runs where the quest was taken. The bot stays with the quest giver and fights what attacks
 *           either of them, until the quest completes, fails or takes too long.
 *   - Lead: the quest giver follows the bot. The bot walks to the NPC it has to be brought to, waits when it falls
 *           behind and fights for it.
 *
 * Stay and Lead events start the moment the quest is taken, and NewRpg takes quests in passing. So the planner watches
 * the quest log (TrackEventQuests): a quest of these kinds that appears in it is "fresh", ends whatever task runs and
 * gets its event task before anything else is planned (PlanFreshEvent). One that has been in the log for longer (taken
 * before the bot died, before a restart) has no event running: the bot goes back to the giver, gives the quest back and
 * takes it afresh, once.
 */

#include "OrderService.h"
#include "Farming.h"
#include "FightEscape.h"
#include "Professions.h"
#include "BotDriver.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GameObject.h"
#include "GroundGuard.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GuildAdventureMgr.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "QuestEvents.h"
#include "QuestSupport.h"
#include "RiskModel.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldIndex.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <concepts>
#include <list>
#include <unordered_set>

using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";

    // The same values the planner's NPC tasks use (OrderService.cpp).
    constexpr float SightDistance = 60.0f;
    constexpr float NearDistance = 30.0f;
    constexpr uint32 AssertGapMs = 5 * IN_MILLISECONDS;
    constexpr uint32 AvoidSpawnMs = 20 * MINUTE * IN_MILLISECONDS;

    // The NPC of an event is looked for this far.
    constexpr float EventSight = 100.0f;
    // A quest that appeared in the log this long ago still has its event running.
    constexpr uint32 EventFreshMs = 6 * MINUTE * IN_MILLISECONDS;

    // Talk: the whole task; at the spot with nobody there; next to the NPC without getting within reach; the wait for the
    // credit after a talk (speeches take their time); tries.
    constexpr uint32 TalkMaxMs = 15 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 TalkAbsentMs = 90 * IN_MILLISECONDS;
    constexpr uint32 TalkApproachMs = 3 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 TalkCreditMs = 2 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 TalkRetryMs = 20 * IN_MILLISECONDS;
    constexpr uint8 TalkTries = 3;
    constexpr float TalkReach = INTERACTION_DISTANCE - 1.5f;
    // A talk with a credit per partner (six Theramore Guards take a leaflet each): the wait for a partner's credit, how
    // long a partner that was asked is left alone, and how many partners in a row may have nothing to say before the
    // quest counts as failed.
    constexpr uint32 TalkEachCreditMs = 25 * IN_MILLISECONDS;
    constexpr uint32 TalkAgainMs = 3 * MINUTE * IN_MILLISECONDS;
    constexpr uint8 TalkMissLimit = 8;

    // A talk partner this close is visited wherever the bot is, also outside its target zone (the quest giver itself, an
    // NPC in the same city).
    constexpr float NearbyTalkDistance = 500.0f;

    // Stay: how close to the NPC, and how long the event may take.
    constexpr float StayDistance = 10.0f;
    constexpr uint32 StayMaxMs = EventFreshMs;

    // Clear: hostile creatures this close to the spawn are killed; the bot walks up to one that is farther away than the
    // second value before it attacks; the whole task may take this long.
    constexpr float ClearRadius = 80.0f;
    constexpr float ClearAttackDistance = 30.0f;
    constexpr uint32 ClearMaxMs = 25 * MINUTE * IN_MILLISECONDS;

    // (A leading bot walks: BotDriver::SetMountAllowed, set by the planner's tick.)
    // Lead: the bot waits when the follower is farther behind than this; out of sight for this long it is lost; the bot
    // stands this close to the end NPC; the whole walk may take this long.
    constexpr float LeadGap = 15.0f;
    constexpr uint32 LeadLostMs = 45 * IN_MILLISECONDS;
    constexpr float LeadArrive = 3.0f;
    constexpr uint32 LeadArriveMs = 3 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 LeadMaxMs = 25 * MINUTE * IN_MILLISECONDS;
    // A follower the bot has waited for this long without its coming does not come (asleep with no horn to wake it,
    // not following at all): the event has failed. A hunter stood 22 minutes beside Kerlonian Evershade.
    constexpr uint32 LeadWaitMs = 2 * MINUTE * IN_MILLISECONDS;

    float Dist2d(Player const* player, float x, float y)
    {
        float const dx = player->GetPositionX() - x;
        float const dy = player->GetPositionY() - y;
        return std::sqrt(dx * dx + dy * dy);
    }

    float Dist2d(Player const* player, SpawnPoint const& spot)
    {
        return Dist2d(player, spot.x, spot.y);
    }

    bool IsCredited(Player* player, Quest const* quest, QuestEventPlan const& plan)
    {
        QuestStatusMap& statusMap = player->getQuestStatusMap();
        auto itr = statusMap.find(quest->GetQuestId());
        if (itr == statusMap.end())
            return false;
        if (plan.objective >= 0)
            return itr->second.CreatureOrGOCount[plan.objective] >= quest->RequiredNpcOrGoCount[plan.objective];
        return itr->second.Explored;
    }

    WorldObject* FindEventObject(Player* player, QuestEventPlan const& plan, float range)
    {
        if (plan.gameObject)
            return player->FindNearestGameObject(plan.entry, range);
        return player->FindNearestCreature(plan.entry, range, true);
    }

    // The partner of a talk: the nearest one the bot has not just asked (with a credit per partner the nearest is the one
    // that has given its credit already).
    WorldObject* FindTalkPartner(Player* player, QuestEventPlan const& plan, float range, BotRuntime const& rt, uint32 now)
    {
        if (plan.gameObject || rt.talkedTo.empty())
            return FindEventObject(player, plan, range);
        std::list<Creature*> found;
        player->GetCreatureListWithEntryInGrid(found, plan.entry, range);
        Creature* best = nullptr;
        float bestDistance = 0.0f;
        for (Creature* creature : found)
        {
            if (!creature->IsAlive() || rt.TalkedTo(creature->GetGUID(), now))
                continue;
            float const distance = player->GetDistance(creature);
            if (!best || distance < bestDistance)
            {
                best = creature;
                bestDistance = distance;
            }
        }
        return best;
    }

    // Whom to fight for an event: what attacks the bot or the NPC of the event, whom that NPC fights, or the NPC
    // itself once it turned on the bot (a brawl). The nearest of them the bot can see.
    Unit* PickEnemy(Player* player, Creature* ward)
    {
        Unit* best = nullptr;
        float bestDistance = 0.0f;
        auto consider = [&](Unit* unit)
        {
            if (!unit || !unit->IsInWorld() || !unit->IsAlive() || !player->IsValidAttackTarget(unit) ||
                !player->IsWithinDistInMap(unit, SightDistance) || !player->IsWithinLOSInMap(unit))
                return;
            float const distance = player->GetDistance(unit);
            if (!best || distance < bestDistance)
            {
                best = unit;
                bestDistance = distance;
            }
        };

        for (Unit* attacker : player->getAttackers())
            consider(attacker);
        if (ward)
        {
            if (ward->GetVictim() == player || (ward->IsInCombat() && ward->IsHostileTo(player)))
                consider(ward);
            for (Unit* attacker : ward->getAttackers())
                consider(attacker);
            consider(ward->GetVictim());
        }
        return best;
    }

    // An event without an NPC to protect: the nearest creature in a fight that the bot may attack.
    Unit* PickFightNearby(Player* player)
    {
        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, NearDistance);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, units, check);
        Cell::VisitObjects(player, searcher, NearDistance);

        Unit* best = nullptr;
        float bestDistance = 0.0f;
        for (Unit* unit : units)
        {
            if (!unit->IsCreature() || !unit->IsAlive() || !unit->IsInCombat() || unit->IsCritter() ||
                !player->IsValidAttackTarget(unit) || !player->IsWithinLOSInMap(unit))
                continue;
            float const distance = player->GetDistance(unit);
            if (!best || distance < bestDistance)
            {
                best = unit;
                bestDistance = distance;
            }
        }
        return best;
    }

    // The creatures a quest's event brought that stand and wait to be attacked: summoned by a script, or made hostile by
    // one (a faction other than their own), within reach of the event's NPC or the bot, not in a fight yet. They count
    // on a player's coming into their aggro range, which this server halves (Rate.Creature.Aggro 0.5): "Questioning
    // Reethe" has Ogron summon Private Hallan, two Theramore Skirmishers and Lieutenant Caldwell and Paval Reethe make
    // them hostile, and they stood beside Ogron while the guild leader waited for the fight, 2026-11-25 (the owner: [quote left out]).
    Unit* PickWaitingEventFoe(Player* player, Creature* ward)
    {
        constexpr float AroundYards = 30.0f;
        WorldObject* center = ward ? static_cast<WorldObject*>(ward) : static_cast<WorldObject*>(player);
        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(center, player, AroundYards);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(center, units, check);
        Cell::VisitObjects(center, searcher, AroundYards);
        Unit* best = nullptr;
        float bestDistance = 0.0f;
        for (Unit* unit : units)
        {
            Creature* creature = unit->ToCreature();
            if (!creature || !creature->IsAlive() || creature->IsInCombat() || creature->IsCritter() || creature->IsPet() ||
                creature->IsTotem() || !creature->IsHostileTo(player) || !player->IsValidAttackTarget(creature) ||
                !player->IsWithinLOSInMap(creature))
                continue;
            bool const brought = creature->IsSummon() ||
                (creature->GetCreatureTemplate() && creature->GetFaction() != creature->GetCreatureTemplate()->faction);
            if (!brought)
                continue;
            float const distance = player->GetDistance(creature);
            if (!best || distance < bestDistance)
            {
                best = creature;
                bestDistance = distance;
            }
        }
        return best;
    }

    // A place to clear: the hostile creature nearest to the bot among those within `radius` of the spot.
    Unit* PickHostileAround(Player* player, SpawnPoint const& spot, float radius)
    {
        float const range = Dist2d(player, spot) + radius;
        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, range);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, units, check);
        Cell::VisitObjects(player, searcher, range);

        Unit* best = nullptr;
        float bestDistance = 0.0f;
        for (Unit* unit : units)
        {
            if (!unit->IsCreature() || !unit->IsAlive() || unit->IsCritter() || !unit->IsHostileTo(player) ||
                !player->IsValidAttackTarget(unit) || !player->CanSeeOrDetect(unit) ||
                unit->GetExactDist2d(spot.x, spot.y) > radius)
                continue;
            float const distance = player->GetDistance(unit);
            if (!best || distance < bestDistance)
            {
                best = unit;
                bestDistance = distance;
            }
        }
        return best;
    }

    // The follower of a lead event is on its way: it follows someone, or it is no longer where it stands when idle.
    bool IsFollowing(Creature* npc, SpawnPoint const& spot)
    {
        if (npc->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
            return true;
        Position const& home = npc->GetHomePosition();
        return npc->GetExactDist2d(home.GetPositionX(), home.GetPositionY()) > 15.0f && npc->GetExactDist2d(spot.x, spot.y) > 15.0f;
    }

    bool Defend(Player* player, Creature* ward)
    {
        Unit* enemy = PickEnemy(player, ward);
        return enemy && BotDriver::Attack(player, enemy);
    }

    void StandStill(Player* player)
    {
        if (BotDriver::GetRpgState(player) != RpgState::Rest)
            BotDriver::Rest(player);
        if (player->isMoving())
        {
            player->GetMotionMaster()->Clear(false);
            player->StopMoving();
        }
    }
}

Unit* OrderService::PickGrindTarget(Player* player, BotRuntime const& rt, SpawnPoint const& spot, uint8 minLevel, uint8 maxLevel,
    bool skins, uint32 dropsItem, float around) const
{
    float const Around = around > 0.0f ? around : 70.0f; // of the spot
    constexpr float StrongNeighbour = 20.0f;
    // An elite too strong for the bot keeps a wider room: it wanders, it calls, and the bot walks past it to the target.
    // (Buffadin ground Cobalt Broodlings in Mazthoril of Winterspring 20 yd and more from the elite Mageweavers and
    // Scalebanes, met them on its way and in the fight, and died there twice in a day, rounds 158 and 162.)
    constexpr float StrongEliteYards = 60.0f;
    constexpr float StrongEliteWayYards = 25.0f;
    constexpr float Company = 15.0f;       // hostile creatures this close to a target fight with it ...
    constexpr float CompanyPenalty = 200.0f; // ... and each counts like this many yards more to walk

    // (A farm order's targets lie far below the bot: what stands next to one is only too strong above the bot's own
    // level, not above the targets'.)
    uint8 const strongLevel = dropsItem ? std::max<uint8>(maxLevel, player->GetLevel()) : maxLevel;
    float const range = Dist2d(player, spot) + Around;
    std::list<Unit*> units;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, range);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, units, check);
    Cell::VisitObjects(player, searcher, range);

    Unit* best = nullptr;
    float bestDistance = 0.0f;
    // (A bot that stands in such a camp itself goes out of it first, FightEscape's rule; its ways from here all start
    // in the camp, so only its targets' places are looked at.)
    bool const botInCamp = Risk::InCampItCannotTake(player, rt, player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
        player->GetPositionZ());
    for (Unit* unit : units)
    {
        Creature* creature = unit->ToCreature();
        if (!creature || !creature->IsAlive() || creature->IsCritter() || creature->IsPet() || creature->IsTotem() ||
            creature->isElite() || creature->GetLevel() < minLevel || creature->GetLevel() > maxLevel ||
            !player->IsValidAttackTarget(creature) || !player->CanSeeOrDetect(creature) ||
            creature->GetExactDist2d(spot.x, spot.y) > Around)
            continue;
        if (skins && (!Professions::IsSkinKind(creature->GetEntry()) || !Professions::CanSkin(player, creature->GetLevel())))
            continue;
        // A farm order: only what drops its item.
        if (dropsItem && sFarming.YieldOf(creature->GetEntry(), dropsItem) <= 0.0f)
            continue;
        // Not a creature of a holiday that does not attack by itself (the Wild Turkeys of Pilgrim's Bounty: its spawn is a
        // game event's), a farm order's excepted.
        if (!dropsItem && !creature->IsHostileTo(player) && sWorldIndex.IsEventSpawn(creature->GetSpawnId()))
            continue;
        // Not one under water (a farm order's excepted: some items only swimmers drop). A fight there drifts with every
        // step to range, the runs from it have no paths, and the breath runs out: Leel (60, mage) ground the Blighted
        // Horrors of Lake Mereldar, its second pull there brought four from homes 53 yd off as the fight drifted 42 yd,
        // it left at 46 % and died on the run under water after 32 s ("6 of 7 directions had no path"), round 230.
        if (!dropsItem && creature->GetMap()->IsUnderWater(creature->GetPhaseMask(), creature->GetPositionX(), creature->GetPositionY(),
            creature->GetPositionZ(), creature->GetCollisionHeight()))
            continue;
        // Somebody else's (tapped, or fighting someone who is not the bot).
        if ((creature->hasLootRecipient() && !creature->isTappedBy(player)) ||
            (creature->GetVictim() && creature->GetVictim() != player && creature->GetVictim()->GetOwnerGUID() != player->GetGUID()))
            continue;
        // One above the limit next to it would join the fight. (An elite by its health and damage: a paladin ground Cobalt
        // Whelps of Winterspring among level-56 elite Scalebanes, which counted as weaker than the bot by their level,
        // left three fights with them and died on the third run, round 158.)
        bool strongNeighbour = false;
        for (Unit* other : units)
        {
            if (other == unit || !other->IsCreature() || !other->IsAlive() || other->IsCritter() || !other->IsHostileTo(player))
                continue;
            // (An elite down to five levels below the bot counts, as on the walks since round 143: the Cobalt Mageweavers,
            // 57 and 58, came out at no more than 60 by their health and damage and were no strong neighbours for a
            // level-60 paladin.)
            constexpr int32 EliteLevelsBelow = 5;
            bool const strongElite = other->ToCreature()->isElite() &&
                int32(other->GetLevel()) + EliteLevelsBelow >= int32(player->GetLevel());
            if (!strongElite && Escape::StrengthOfCreature(other->ToCreature()) <= strongLevel)
                continue;
            float const room = other->ToCreature()->isElite() ? StrongEliteYards : StrongNeighbour;
            if (other->GetExactDist2d(creature) <= room)
            {
                strongNeighbour = true;
                break;
            }
            // (And an elite beside the straight way from the bot to the target.)
            if (other->ToCreature()->isElite())
            {
                float const ax = player->GetPositionX(), ay = player->GetPositionY();
                float const bx = creature->GetPositionX() - ax, by = creature->GetPositionY() - ay;
                float const length = bx * bx + by * by;
                float t = length > 0.0f ? ((other->GetPositionX() - ax) * bx + (other->GetPositionY() - ay) * by) / length : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);
                float const dx = ax + bx * t - other->GetPositionX(), dy = ay + by * t - other->GetPositionY();
                if (dx * dx + dy * dy <= StrongEliteWayYards * StrongEliteWayYards)
                {
                    strongNeighbour = true;
                    break;
                }
            }
        }
        if (strongNeighbour)
            continue;
        // Not at a place it keeps away from (it died there, or left a fight there).
        if (rt.IsInAvoidedArea(creature->GetMapId(), creature->GetPositionX(), creature->GetPositionY(), player->GetLevel()))
            continue;
        // Nor one it leaves alone for a while: a fight the module ended because the creature did not die, or a creature
        // picked again and again at a grind spot with no fight coming of it.
        if (auto alone = rt.undyingFoes.find(creature->GetGUID()); alone != rt.undyingFoes.end() && getMSTime() < alone->second)
            continue;
        // One that stands alone before one of a pack (the others join the fight), then the nearest. A pack the risk
        // rules do not let it take is no target at all (a level-21 warlock died three times among the bears of one
        // grind spot).
        uint32 company = 0;
        uint8 packLevel = creature->GetLevel();
        for (Unit* other : units)
            if (other != unit && other->IsCreature() && other->IsAlive() && !other->IsCritter() && other->IsHostileTo(player) &&
                other->GetExactDist2d(creature) <= Company)
            {
                ++company;
                packLevel = std::max<uint8>(packLevel, other->GetLevel());
            }
        if (company && !Risk::CanFight(player, rt, packLevel, company + 1))
            continue;
        float const distance = player->GetDistance(creature) + float(company) * CompanyPenalty;
        if (best && distance >= bestDistance)
            continue;
        // Not in the middle of a camp it cannot take, nor across one: the creatures 15 yards around the target were
        // all its fight counted, and the camp's others wander and call (looked at only for one that would be the
        // best so far: the camp verdict is the costly part). At the level cap the grind takes creatures of its own
        // level too, and Pewpew, a level-60 hunter grinding beside the Twilight camp of Silithus, had three of its
        // Overlords and Masters on it ten seconds after a won fight, six in the end, and died inside the camp, round 196.
        if (Risk::InCampItCannotTake(player, rt, creature->GetMapId(), creature->GetPositionX(), creature->GetPositionY(),
                creature->GetPositionZ()) ||
            (!botInCamp && Risk::EntersCamp(player, rt, player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
                creature->GetPositionX(), creature->GetPositionY())))
            continue;
        // Nor one where the path finder has no walkable ground (looked at under it, for one that flies): the Noxious
        // Plaguebats on the rock beside Light's Hope stand on a top no navmesh covers, and five bots picked the same one
        // within a minute, had their walks refused ("32 yd through the air") and were cut off at the rock's foot, round 266.
        // PlanGrind skips such spawns since round 228; the creature picked at the spot was not looked at.
        {
            float const x = creature->GetPositionX();
            float const y = creature->GetPositionY();
            float ground = creature->GetMap()->GetHeight(creature->GetPhaseMask(), x, y, creature->GetPositionZ() + 2.0f, true, 50.0f);
            if (ground <= INVALID_HEIGHT + 1.0f)
                ground = creature->GetPositionZ();
            if (!GroundGuard::HasGroundAt(player, x, y, ground))
                continue;
        }
        if (!best || distance < bestDistance)
        {
            best = creature;
            bestDistance = distance;
        }
    }
    return best;
}

bool OrderService::DefendWard(Player* player, Creature* ward)
{
    if (Defend(player, ward))
        return true;
    // And what the event brought that waits to be attacked (PickWaitingEventFoe): the fight it is there for.
    Unit* foe = PickWaitingEventFoe(player, ward);
    // The bot may walk to it a few seconds before it swings: the line once per creature, not once per step.
    auto runtime = _runtime.find(player->GetGUID());
    bool const fresh = foe && player->GetVictim() != foe
        && (runtime == _runtime.end() || runtime->second.wardFoeAnnounced != foe->GetGUID());
    if (foe && BotDriver::Attack(player, foe))
    {
        if (fresh && runtime != _runtime.end())
            runtime->second.wardFoeAnnounced = foe->GetGUID();
        if (fresh)
            LOG_INFO(ActivityLog, "activity: {} attacks {} (level {}), which its quest's event brought beside {} and which waits to be "
            "attacked", player->GetName(), foe->GetName(), uint32(foe->GetLevel()), ward ? ward->GetName() : std::string("it"));
        return true;
    }
    return false;
}

bool BotRuntime::IsEventFresh(uint32 questId) const
{
    auto itr = eventQuests.find(questId);
    return itr != eventQuests.end() && itr->second.takenAt && getMSTimeDiff(itr->second.takenAt, getMSTime()) < EventFreshMs;
}

bool OrderService::TrackEventQuests(Player* player, BotRuntime& rt)
{
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    bool yield = false;
    std::unordered_set<uint32> inLog;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = player->GetQuestSlotQuestId(slot);
        QuestEventPlan const* plan = questId ? sQuestEvents.GetEventPlan(questId) : nullptr;
        if (!plan || (plan->kind != QuestEventKind::Stay && plan->kind != QuestEventKind::Lead))
            continue;
        inLog.insert(questId);
        auto [itr, inserted] = rt.eventQuests.try_emplace(questId);
        BotRuntime::EventQuest& state = itr->second;
        if (inserted)
            state.takenAt = rt.eventLogKnown ? now : 0; // what is in the log when the bot enters the world is old
        if (state.announced || !rt.IsEventFresh(questId) || player->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;
        state.announced = true;
        bool const itsTask = rt.task.kind == TaskKind::Event && rt.task.questId == questId;
        // A voyage gives way only while the bot still walks to the dock (on the pier or on board it is too late).
        bool const voyageUnderWay = rt.task.kind == TaskKind::Voyage && rt.task.voyage != VoyagePhase::ToDock;
        if (!itsTask && rt.task.kind != TaskKind::None && !voyageUnderWay && rt.task.kind != TaskKind::Escort)
            yield = true;
    }
    std::erase_if(rt.eventQuests, [&](auto const& entry) { return !inLog.count(entry.first); });
    rt.eventLogKnown = true;
    return yield;
}

bool OrderService::PlanFreshEvent(GuildBot const& bot, Player* player, BotRuntime& rt)
{
    for (auto const& [questId, state] : rt.eventQuests)
    {
        if (player->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;
        auto block = bot.questBlocks.find(questId);
        if (block != bot.questBlocks.end() && block->second.blocked)
            continue;
        if (sQuestSupport.IsLeftAlone(player, questId))
            continue; // the bot setting Quests.Events is off for it
        QuestEventPlan const* plan = sQuestEvents.GetEventPlan(questId);
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!plan || !quest || sQuestSupport.GetEscortNpc(questId) || IsCredited(player, quest, *plan))
            continue;
        Creature* npc = player->FindNearestCreature(plan->entry, EventSight, true);
        if (!npc)
            continue;
        // Taken a moment ago, or (lead) the NPC is on its way with the bot: after a rest, an escape or a fight the
        // event goes on where it was.
        bool running = rt.IsEventFresh(questId);
        if (!running && plan->kind == QuestEventKind::Lead)
        {
            Position const& home = npc->GetHomePosition();
            running = npc->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE ||
                npc->GetExactDist2d(home.GetPositionX(), home.GetPositionY()) > 15.0f;
        }
        if (!running)
            continue;

        BotTask task;
        task.kind = TaskKind::Event;
        task.questId = questId;
        task.event = *plan;
        task.eventFresh = true;
        task.hasSpot = true;
        task.spot.entry = plan->entry;
        task.spot.mapId = uint16(player->GetMapId());
        task.spot.x = npc->GetPositionX();
        task.spot.y = npc->GetPositionY();
        task.spot.z = npc->GetPositionZ();
        task.targetName = npc->GetName();
        if (plan->kind == QuestEventKind::Lead)
        {
            // The nearest spawn of the NPC it has to be brought to.
            SpawnPoint const* end = nullptr;
            for (SpawnPoint const& spawn : sWorldIndex.GetCreatureSpawns(plan->endEntry))
                if (spawn.mapId == player->GetMapId() && (!end || Dist2d(player, spawn) < Dist2d(player, *end)))
                    end = &spawn;
            if (!end)
                continue;
            task.eventEnd = *end;
        }
        StartTask(player, rt, task);
        rt.why = Acore::StringFormat("The event of \"{}\" started at {}", quest->GetTitle(), task.targetName);
        return true;
    }
    return false;
}

bool OrderService::PlanNearbyTalk(GuildBot const& bot, Player* player, BotRuntime& rt)
{
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = player->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest || player->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE ||
            sQuestSupport.GetVerdict(questId) != QuestVerdict::Supported || rt.IsDangerous(questId, player->GetLevel()))
            continue;
        auto block = bot.questBlocks.find(questId);
        if (block != bot.questBlocks.end() && block->second.blocked)
            continue;
        for (QuestEventPlan const& plan : sQuestEvents.GetPlans(questId))
        {
            if (plan.kind != QuestEventKind::Talk || IsCredited(player, quest, plan))
                continue;
            SpawnPoint const* nearest = nullptr;
            for (SpawnPoint const& spawn : plan.gameObject ? sWorldIndex.GetGameObjectSpawns(plan.entry) :
                sWorldIndex.GetCreatureSpawns(plan.entry))
                if (spawn.mapId == player->GetMapId() && !rt.IsAvoided(spawn) && Dist2d(player, spawn) <= NearbyTalkDistance &&
                    (!nearest || Dist2d(player, spawn) < Dist2d(player, *nearest)))
                    nearest = &spawn;
            if (!nearest)
                continue;

            BotTask task;
            task.kind = TaskKind::Event;
            task.questId = questId;
            task.event = plan;
            task.hasSpot = true;
            task.spot = *nearest;
            if (plan.gameObject)
            {
                GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(plan.entry);
                task.targetName = info ? info->name : "an object";
            }
            else
            {
                CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(plan.entry);
                task.targetName = info ? info->Name : "someone";
            }
            StartTask(player, rt, task);
            rt.why = Acore::StringFormat("Going to {} to talk for \"{}\"", task.targetName, quest->GetTitle());
            return true;
        }
    }
    return false;
}

bool OrderService::StepEvent(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    BotTask& task = rt.task;
    QuestEventPlan const plan = task.event; // a copy: ClearTask resets the task
    uint32 const questId = task.questId;
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    QuestStatus const status = player->GetQuestStatus(questId);
    if (!quest || (status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_FAILED))
    {
        ClearTask(rt, 0, status == QUEST_STATUS_COMPLETE ? "event done" : "quest no longer in the log");
        return false;
    }
    std::string const title = quest->GetTitle();
    if (status == QUEST_STATUS_FAILED)
    {
        LOG_INFO(ActivityLog, "activity: {} failed the event of quest {} \"{}\" at {}", bot.name, questId, title, task.targetName);
        FailQuest(guild, bot, player, questId, "event_failed");
        ClearTask(rt, 5 * IN_MILLISECONDS, "event failed");
        return false;
    }
    if (IsCredited(player, quest, plan))
    {
        ClearTask(rt, 0, "event done");
        return false;
    }

    // The walk to the spot of the task; false when it got stuck (the spot is skipped for a while).
    auto walkToSpot = [&](char const* what) -> bool
    {
        if (UpdateStuck(player, rt, task.spot.x, task.spot.y))
        {
            rt.Avoid(task.spot, AvoidSpawnMs);
            LOG_INFO("module", "mod-guild-adventure: {} could not get to {} for quest {} \"{}\"; skipped for {} min", bot.name,
                task.targetName, questId, title, AvoidSpawnMs / (MINUTE * IN_MILLISECONDS));
            ClearTask(rt, 5 * IN_MILLISECONDS, "not reached");
            return false;
        }
        WalkTo(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z);
        rt.why = Acore::StringFormat("Going to {} {} \"{}\"", task.targetName, what, title);
        return true;
    };

    if (plan.kind == QuestEventKind::Talk)
    {
        if (now - task.startedAt > TalkMaxMs)
        {
            rt.Avoid(task.spot, AvoidSpawnMs);
            ClearTask(rt, 5 * IN_MILLISECONDS, "talk partner not reached");
            return false;
        }

        // A credit per partner: once this one's has come, the next partner is asked. The one that gave it is left alone
        // for a while (its script runs, and it gives no second credit while it does).
        bool const each = plan.objective >= 0 && !plan.gameObject && quest->RequiredNpcOrGoCount[plan.objective] > 1;
        if (each)
        {
            int32 count = 0;
            QuestStatusMap& statusMap = player->getQuestStatusMap();
            if (auto itr = statusMap.find(questId); itr != statusMap.end())
                count = int32(itr->second.CreatureOrGOCount[plan.objective]);
            if (task.eventCount < 0)
                task.eventCount = count;
            else if (count > task.eventCount)
            {
                LOG_INFO(ActivityLog, "activity: {} has the credit of {} for quest {} \"{}\" ({} of {}): it asks the next one", bot.name,
                    task.targetName, questId, title, count, uint32(quest->RequiredNpcOrGoCount[plan.objective]));
                if (task.eventPartner)
                    rt.talkedTo[task.eventPartner] = now + TalkAgainMs;
                rt.talkMisses.erase(questId);
                task.eventCount = count;
                task.eventTalks = 0;
                task.eventWaitUntil = 0;
                task.nearSince = 0;
                if (task.eventCredits < 255)
                    ++task.eventCredits;
            }
        }

        float const distance = Dist2d(player, task.spot);
        WorldObject* target = distance <= 2.0f * SightDistance ? FindTalkPartner(player, plan, SightDistance, rt, now) : nullptr;
        if (!target)
        {
            // Nobody left to ask around this spot: the planner takes another one.
            if (each && (task.eventCredits || task.eventPartner))
            {
                rt.Avoid(task.spot, TalkAgainMs);
                ClearTask(rt, 0, task.eventCredits ? "talk counted" : "talk partner has nothing to say");
                return false;
            }
            if (distance > NearDistance)
                return walkToSpot("to talk for");
            // At its spot, and it is not there (dead, respawning, on its rounds).
            if (!task.nearSince)
                task.nearSince = now;
            if (now - task.nearSince > TalkAbsentMs)
            {
                rt.Avoid(task.spot, AvoidSpawnMs);
                ClearTask(rt, 0, "talk partner not there");
                return false;
            }
            rt.why = Acore::StringFormat("Waiting for {} to talk for \"{}\"", task.targetName, title);
            return true;
        }

        // After a talk: the script may hold a speech or send enemies before the credit comes.
        if (task.eventTalks)
        {
            if (now < task.eventWaitUntil)
            {
                if (!Defend(player, target->ToCreature()))
                    StandStill(player);
                rt.why = Acore::StringFormat("Talked to {} for \"{}\": waiting for it to count", task.targetName, title);
                return true;
            }
            if (each)
            {
                // Its time is over without a credit: another partner (this one is counted like one with nothing to say).
                if (task.eventPartner)
                    rt.talkedTo[task.eventPartner] = now + TalkAgainMs;
                task.eventTalks = 0;
                task.eventWaitUntil = 0;
                task.nearSince = 0;
                if (++rt.talkMisses[questId] >= TalkMissLimit)
                {
                    LOG_INFO(ActivityLog, "activity: {} asked {} of {} for quest {} \"{}\" in a row without a credit", bot.name,
                        uint32(TalkMissLimit), task.targetName, questId, title);
                    rt.talkMisses.erase(questId);
                    FailQuest(guild, bot, player, questId, "no_progress");
                    ClearTask(rt, 5 * IN_MILLISECONDS, "the talk gave no credit");
                    return false;
                }
                return true;
            }
            if (task.eventTalks >= TalkTries)
            {
                LOG_INFO(ActivityLog, "activity: {} talked to {} {} times for quest {} \"{}\" without credit", bot.name,
                    task.targetName, uint32(task.eventTalks), questId, title);
                FailQuest(guild, bot, player, questId, "no_progress");
                ClearTask(rt, 5 * IN_MILLISECONDS, "the talk gave no credit");
                return false;
            }
        }

        // NewRpg stops where it can interact, which may be a step farther than TalkReach: having stood at the NPC counts.
        if (!player->IsWithinDistInMap(target, TalkReach) &&
            !(BotDriver::HasReachedVisitTarget(player, target->GetGUID()) && player->IsWithinDistInMap(target, INTERACTION_DISTANCE)))
        {
            if (!task.nearSince)
                task.nearSince = now;
            if (now - task.nearSince > TalkApproachMs)
            {
                rt.Avoid(task.spot, AvoidSpawnMs);
                ClearTask(rt, 5 * IN_MILLISECONDS, "talk partner not reached");
                return false;
            }
            // NewRpg picks another NPC after a while at one: the visit is set again then.
            if (!BotDriver::IsVisiting(player, target->GetGUID()) ||
                (BotDriver::GetRpgState(player) != RpgState::WanderNpc && now - task.lastAssert >= AssertGapMs))
            {
                BotDriver::Visit(player, target->GetGUID());
                task.lastAssert = now;
            }
            rt.why = Acore::StringFormat("Walking up to {} to talk for \"{}\"", task.targetName, title);
            return true;
        }

        StandStill(player);
        std::string detail;
        TalkResult const result = sQuestEvents.Talk(player, target, plan, detail);
        LOG_INFO(ActivityLog, "activity: {} talks to {} for quest {} \"{}\": {}", bot.name, target->GetName(), questId, title, detail);
        rt.why = Acore::StringFormat("Talked to {} for \"{}\"", task.targetName, title);
        task.eventPartner = target->GetGUID();
        if (each && result != TalkResult::Chosen)
        {
            // This one has nothing to say now (it was asked a moment ago, by this bot or another): another one.
            rt.talkedTo[target->GetGUID()] = now + TalkAgainMs;
            task.nearSince = 0;
            if (++rt.talkMisses[questId] >= TalkMissLimit)
            {
                LOG_INFO(ActivityLog, "activity: {} asked {} of {} for quest {} \"{}\" in a row and none had anything to say", bot.name,
                    uint32(TalkMissLimit), task.targetName, questId, title);
                rt.talkMisses.erase(questId);
                FailQuest(guild, bot, player, questId, "no_progress");
                ClearTask(rt, 5 * IN_MILLISECONDS, "the talk gave no credit");
                return false;
            }
            return true;
        }
        ++task.eventTalks;
        // A talk that did not get to its choice is tried again soon; one that did waits for the script.
        task.eventWaitUntil = now + (result == TalkResult::Chosen ? (each ? TalkEachCreditMs : TalkCreditMs) : TalkRetryMs);
        return true;
    }

    if (plan.kind == QuestEventKind::Enter)
    {
        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(plan.entry);
        if (!trigger)
        {
            ClearTask(rt, 0, "no such area trigger");
            return false;
        }
        if (!task.eventEntered)
        {
            if (now - task.startedAt > TalkMaxMs)
            {
                rt.Avoid(task.spot, AvoidSpawnMs);
                ClearTask(rt, 5 * IN_MILLISECONDS, "place not reached");
                return false;
            }
            if (!player->IsInAreaTriggerRadius(trigger))
                return walkToSpot("for");
            // Inside: what a client sends when its character walks into an area trigger.
            WorldPacket packet(CMSG_AREATRIGGER, 4);
            packet << uint32(plan.entry);
            player->GetSession()->HandleAreaTriggerOpcode(packet);
            task.eventEntered = true;
            task.nearSince = now;
            LOG_INFO(ActivityLog, "activity: {} walks into the place of quest {} \"{}\" (area trigger {})", bot.name, questId, title,
                plan.entry);
        }
        if (now - task.nearSince > StayMaxMs)
        {
            LOG_INFO(ActivityLog, "activity: {} waited {} s at the place of quest {} \"{}\": no credit", bot.name,
                StayMaxMs / IN_MILLISECONDS, questId, title);
            FailQuest(guild, bot, player, questId, "no_progress");
            ClearTask(rt, 5 * IN_MILLISECONDS, "event took too long");
            return false;
        }
        // The event: whoever fights next to the bot and can be attacked is its enemy.
        if (Unit* enemy = PickEnemy(player, nullptr); enemy ? BotDriver::Attack(player, enemy) : false)
        {
            rt.why = Acore::StringFormat("Fighting at the place of \"{}\"", title);
            return true;
        }
        if (Unit* enemy = PickFightNearby(player); enemy && BotDriver::Attack(player, enemy))
        {
            rt.why = Acore::StringFormat("Fighting at the place of \"{}\"", title);
            return true;
        }
        // A wave that stands and waits (the server's aggro radius is halved): the bot opens the fight.
        Unit* waiting = PickWaitingEventFoe(player, nullptr);
        bool const fresh = waiting && player->GetVictim() != waiting;
        if (Unit* enemy = waiting; enemy && BotDriver::Attack(player, enemy))
        {
            if (fresh)
                LOG_INFO(ActivityLog, "activity: {} attacks {} (level {}), which the event of quest {} brought and which waits to be "
                "attacked", bot.name, enemy->GetName(), uint32(enemy->GetLevel()), questId);
            rt.why = Acore::StringFormat("Fighting at the place of \"{}\"", title);
            return true;
        }
        if (Dist2d(player, task.spot) > StayDistance + 5.0f)
        {
            if (now - task.lastAssert >= AssertGapMs)
            {
                BotDriver::GoTo(player, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z, false);
                task.lastAssert = now;
            }
        }
        else if (BotDriver::GetRpgState(player) != RpgState::Rest)
            BotDriver::Rest(player);
        rt.why = Acore::StringFormat("At the place of \"{}\": waiting for its event", title);
        return true;
    }

    if (plan.kind == QuestEventKind::Clear)
    {
        if (now - task.startedAt > ClearMaxMs)
        {
            LOG_INFO(ActivityLog, "activity: {} cleared the place of {} for {} min for quest {} \"{}\": no credit", bot.name,
                task.targetName, ClearMaxMs / (MINUTE * IN_MILLISECONDS), questId, title);
            FailQuest(guild, bot, player, questId, "no_progress");
            ClearTask(rt, 5 * IN_MILLISECONDS, "event took too long");
            return false;
        }
        float const distance = Dist2d(player, task.spot);
        if (distance > ClearRadius && !task.eventEntered)
            return walkToSpot("to clear its place for");
        if (!task.eventEntered)
        {
            task.eventEntered = true;
            LOG_INFO(ActivityLog, "activity: {} is at the place of {} for quest {} \"{}\": it clears {} yd around it and waits", bot.name,
                task.targetName, questId, title, uint32(ClearRadius));
        }

        // What attacks the bot comes first (the waves), then the creature of the objective once it shows itself, then
        // what is left around its spawn.
        Unit* enemy = PickEnemy(player, nullptr);
        if (!enemy)
            if (Creature* wanted = player->FindNearestCreature(plan.entry, EventSight, true))
                if (player->CanSeeOrDetect(wanted) && player->IsValidAttackTarget(wanted))
                    enemy = wanted;
        if (!enemy)
            enemy = PickHostileAround(player, task.spot, ClearRadius);
        if (enemy)
        {
            task.nearSince = now;
            if (player->GetDistance(enemy) <= ClearAttackDistance && BotDriver::Attack(player, enemy))
                rt.why = Acore::StringFormat("Fighting {} at the place of {} (\"{}\")", enemy->GetName(), task.targetName, title);
            else
            {
                // Too far, or out of sight: closer first.
                if (now - task.lastAssert >= AssertGapMs)
                {
                    BotDriver::GoTo(player, task.spot.mapId, enemy->GetPositionX(), enemy->GetPositionY(), enemy->GetPositionZ(), false);
                    task.lastAssert = now;
                }
                rt.why = Acore::StringFormat("Clearing the place of {}: going for {} (\"{}\")", task.targetName, enemy->GetName(), title);
            }
            return true;
        }

        // Nothing left: at the spawn, until its event has run.
        if (!task.nearSince)
            task.nearSince = now;
        if (now - task.nearSince > StayMaxMs)
        {
            LOG_INFO(ActivityLog, "activity: {} waited {} s at the cleared place of {} for quest {} \"{}\": nothing came", bot.name,
                StayMaxMs / IN_MILLISECONDS, task.targetName, questId, title);
            FailQuest(guild, bot, player, questId, "no_progress");
            ClearTask(rt, 5 * IN_MILLISECONDS, "event took too long");
            return false;
        }
        if (distance > StayDistance)
        {
            if (now - task.lastAssert >= AssertGapMs)
            {
                BotDriver::GoTo(player, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z, false);
                task.lastAssert = now;
            }
        }
        else if (BotDriver::GetRpgState(player) != RpgState::Rest)
            BotDriver::Rest(player);
        rt.why = Acore::StringFormat("At the cleared place of {}: waiting for it to show itself (\"{}\")", task.targetName, title);
        return true;
    }

    // Stay and Lead: an event that started when the quest was taken.
    Creature* npc = player->FindNearestCreature(plan.entry, EventSight, true);
    // A follower that follows (or has left its spot) shows that its event runs, whatever happened in between: a rest
    // after the accept, an escape, a fight, a jump.
    if (!task.eventFresh && plan.kind == QuestEventKind::Lead && npc && IsFollowing(npc, task.spot))
        task.eventFresh = true;
    if (!task.eventFresh)
    {
        // The quest has been in the log for a while: its event is over or never ran for this bot. Back to the giver,
        // give the quest back and take it afresh, once.
        if (Dist2d(player, task.spot) > NearDistance)
            return walkToSpot("to start afresh");
        uint8& retakes = rt.eventRetakes[questId];
        if (retakes >= 1)
        {
            FailQuest(guild, bot, player, questId, "event_not_started");
            ClearTask(rt, 5 * IN_MILLISECONDS, "event did not start");
            return false;
        }
        ++retakes;
        LOG_INFO(ActivityLog, "activity: {} gives quest {} \"{}\" back at {} to take it afresh (its event is not running)", bot.name,
            questId, title, task.targetName);
        BotDriver::AbandonQuest(player, questId);
        BotDriver::ForgetAIGivenUp(player, questId);
        ClearTask(rt, 0, "quest given back");
        return false;
    }

    if (plan.kind == QuestEventKind::Stay)
    {
        if (now - task.startedAt > StayMaxMs)
        {
            LOG_INFO(ActivityLog, "activity: {} waited {} s at {} for the event of quest {} \"{}\": no credit", bot.name,
                StayMaxMs / IN_MILLISECONDS, task.targetName, questId, title);
            // "No progress": enough bots with the same result, and the quest is left alone by all (QuestSupport).
            FailQuest(guild, bot, player, questId, "no_progress");
            ClearTask(rt, 5 * IN_MILLISECONDS, "event took too long");
            return false;
        }
        if (DefendWard(player, npc))
        {
            rt.why = Acore::StringFormat("Fighting at {} for \"{}\"", task.targetName, title);
            return true;
        }
        float const x = npc ? npc->GetPositionX() : task.spot.x;
        float const y = npc ? npc->GetPositionY() : task.spot.y;
        float const z = npc ? npc->GetPositionZ() : task.spot.z;
        if (Dist2d(player, x, y) > StayDistance)
        {
            if (now - task.lastAssert >= AssertGapMs)
            {
                BotDriver::GoTo(player, task.spot.mapId, x, y, z, false);
                task.lastAssert = now;
            }
        }
        else if (BotDriver::GetRpgState(player) != RpgState::Rest)
            BotDriver::Rest(player);
        rt.why = Acore::StringFormat("Staying with {} for \"{}\"", task.targetName, title);
        return true;
    }

    // Lead.
    if (now - task.startedAt > LeadMaxMs)
    {
        FailQuest(guild, bot, player, questId, "event_timeout");
        ClearTask(rt, 5 * IN_MILLISECONDS, "event took too long");
        return false;
    }
    if (!npc)
    {
        if (!task.eventSeenAt)
            task.eventSeenAt = now;
        if (now - task.eventSeenAt > LeadLostMs)
        {
            LOG_INFO(ActivityLog, "activity: {} lost {} of quest {} \"{}\"", bot.name, task.targetName, questId, title);
            FailQuest(guild, bot, player, questId, "follower_lost");
            ClearTask(rt, 5 * IN_MILLISECONDS, "follower lost");
            return false;
        }
        StandStill(player);
        PauseStuck(rt);
        rt.why = Acore::StringFormat("Waiting for {} (\"{}\")", task.targetName, title);
        return true;
    }
    task.eventSeenAt = now;
    if (DefendWard(player, npc))
    {
        rt.why = Acore::StringFormat("Fighting for {} (\"{}\")", task.targetName, title);
        return true;
    }
    // A follower that fell asleep, fainted or wandered off is helped with the quest's item (Horn of Awakening,
    // Spraggle's Canteen, Shay's Bell). OrderService::AidFollower comes with the quest interactions; this compiles with
    // and without them.
    auto aidFollower = [&](auto* self) -> bool
    {
        if constexpr (requires { { self->AidFollower(bot, player, rt, quest, npc) } -> std::convertible_to<bool>; })
            return self->AidFollower(bot, player, rt, quest, npc);
        else
            return false;
    };
    if (aidFollower(this))
        return true;
    if (player->GetDistance2d(npc) > LeadGap)
    {
        if (!task.leadWaitSince)
            task.leadWaitSince = now;
        else if (now - task.leadWaitSince > LeadWaitMs)
        {
            LOG_INFO(ActivityLog, "activity: {} waited {} s for {} of quest {} \"{}\": it does not come", bot.name,
                LeadWaitMs / IN_MILLISECONDS, task.targetName, questId, title);
            FailQuest(guild, bot, player, questId, "follower_stays");
            ClearTask(rt, 5 * IN_MILLISECONDS, "the follower does not come");
            return false;
        }
        StandStill(player);
        PauseStuck(rt);
        rt.why = Acore::StringFormat("Waiting for {} to catch up (\"{}\")", task.targetName, title);
        return true;
    }
    task.leadWaitSince = 0;

    Creature* end = player->FindNearestCreature(plan.endEntry, SightDistance, true);
    std::string const endName = end ? end->GetName() : "its destination";
    if (end && player->GetDistance2d(end) <= NearDistance)
    {
        // The last steps: right up to it, so the follower comes within its reach.
        if (!task.nearSince)
            task.nearSince = now;
        if (now - task.nearSince > LeadArriveMs)
        {
            LOG_INFO(ActivityLog, "activity: {} brought {} to {} for quest {} \"{}\" without credit", bot.name, task.targetName, endName,
                questId, title);
            FailQuest(guild, bot, player, questId, "no_progress");
            ClearTask(rt, 5 * IN_MILLISECONDS, "the arrival gave no credit");
            return false;
        }
        if (player->IsWithinDistInMap(end, LeadArrive) || BotDriver::HasReachedVisitTarget(player, end->GetGUID()))
            StandStill(player);
        else if (!BotDriver::IsVisiting(player, end->GetGUID()) ||
            (BotDriver::GetRpgState(player) != RpgState::WanderNpc && now - task.lastAssert >= AssertGapMs))
        {
            BotDriver::Visit(player, end->GetGUID());
            task.lastAssert = now;
        }
        rt.why = Acore::StringFormat("Bringing {} to {} (\"{}\")", task.targetName, endName, title);
        return true;
    }

    if (UpdateStuck(player, rt, task.eventEnd.x, task.eventEnd.y))
    {
        LOG_INFO(ActivityLog, "activity: {} got stuck leading {} for quest {} \"{}\"", bot.name, task.targetName, questId, title);
        FailQuest(guild, bot, player, questId, "event_stuck");
        ClearTask(rt, 5 * IN_MILLISECONDS, "stuck on the way");
        return false;
    }
    WalkTo(player, rt, task.eventEnd.mapId, task.eventEnd.x, task.eventEnd.y, task.eventEnd.z);
    rt.why = Acore::StringFormat("Leading {} to its destination for \"{}\"", task.targetName, title);
    return true;
}
