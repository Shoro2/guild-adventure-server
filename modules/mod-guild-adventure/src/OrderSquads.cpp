/*
 * Squads in the order planner (docs/orders.md "Squads", Squads.h). Part of OrderService; kept in a file of its own.
 *
 * A member that follows its squad leader plans nothing of its own: playerbots' follow strategy keeps it beside the
 * leader (its master) and fights beside it in the leader's party. This file does what follow does not:
 *   - UpdateSquad: once per tick, before Reconcile: whether the bot follows a leader now; a member far behind its
 *     leader (the leader flew, took a ship or a portal, revived at a spirit healer) catches up; a leader looks whether
 *     it has to wait for a member (resting, dead and running back, fighting, lagging behind);
 *   - RunSquadMember: in place of the member's order: quests (it takes what its leader took at the giver it stands at,
 *     hands in completed quests at their ender, gives up what its leader gave up) and the why line;
 *   - StepSquadFlee: a member whose leader runs from a lost fight runs with it;
 *   - StepSquadWait: the leader's task clocks stand still while it waits;
 *   - OnSquadMemberDied: the leader remembers where a member died, as if it had died there itself (RiskModel);
 *   - StepPartnerMeet: a quest partner (QuestPartners.h) walks to a leader it has not met yet.
 */

#include "OrderService.h"
#include "DungeonRoles.h"
#include "DungeonNav.h"
#include "Dungeons.h"
#include "FightEscape.h"
#include "BotDriver.h"
#include "BotSettings.h"
#include "CellImpl.h"
#include "Corpse.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildAdventureUtil.h"
#include "GuildBotHolder.h"
#include "GroundGuard.h"
#include "Log.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathProbe.h"
#include "Pet.h"
#include "Player.h"
#include "QuestDef.h"
#include "QuestPartners.h"
#include "RiskModel.h"
#include "ScriptMgr.h"
#include "Squads.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"
#include "WorldIndex.h"
#include <algorithm>
#include <cmath>

using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";
    constexpr uint32 TickMs = 1000;

    // A member this long far behind its leader (GuildAdventure.Squads.CatchUpYards, or on another map) catches up.
    constexpr uint32 CatchUpAfterMs = 15 * IN_MILLISECONDS;
    // In a dungeon run: a member with no way on foot to its leader catches up from this far, the way is looked at this
    // often, and a fight it fights alone that long does not keep it from catching up.
    constexpr float DungeonCatchUpYards = 60.0f;
    constexpr uint32 DungeonWayCheckMs = 10 * IN_MILLISECONDS;
    constexpr uint32 DungeonAloneFightMs = MINUTE * IN_MILLISECONDS;
    // The leader waits for a member that is alive and this far behind (closer: it is just walking behind), until it is
    // this close again (a leader that went on at 50 yards stopped every few seconds on a long walk).
    constexpr float LagStartYards = 60.0f;
    constexpr float LagEndYards = 20.0f;
    // A member this far above or below its leader with no way on foot to it walks back to it along the roads.
    constexpr float RejoinHeightYards = 8.0f;
    constexpr uint32 RejoinPauseMs = 2 * MINUTE * IN_MILLISECONDS;
    // A member far behind travels after its leader until it is this near to it; its follow does the rest.
    constexpr float FarRejoinDoneYards = 60.0f;
    // Within this of its leader the travel's last stretch is a walk to where the leader stands (StepPartnerMeet's).
    constexpr float FarRejoinWalkYards = 300.0f;
    // .. its meeting point: there within this, and a journey arrives this near to a spot (OrderService.cpp).
    constexpr float MeetArriveYards = 30.0f;
    constexpr float JourneyArriveYards = 250.0f;
    constexpr uint32 MeetWaitMs = 15 * MINUTE * IN_MILLISECONDS;
    // After a wait that ran out the leader goes on for this long before it waits again.
    constexpr uint32 NoWaitAfterLongWaitMs = MINUTE * IN_MILLISECONDS;
    // Quests: how often a member looks at its leader's quest log, and how close a quest giver or ender must stand.
    constexpr uint32 QuestCheckMs = 2 * IN_MILLISECONDS;
    constexpr float GiverReach = 15.0f;
    constexpr float GiverSearchYards = 40.0f;
    // A quest the member cannot take now is not looked at again for this long.
    constexpr uint32 ShareSkipMs = MINUTE * IN_MILLISECONDS;
    // Below this many free quest log slots a member gives up shared quests its leader is done with.
    constexpr uint32 MemberFreeSlots = 4;
    // A member helps a squad character in a fight within this distance.
    constexpr float AssistYards = 40.0f;
    // ... in a dungeon run (the group goes to help a member that fights: OrderService::StepDungeonRescue).
    constexpr float DungeonAssistYards = 100.0f;
    // A quest partner on its way has met its leader this close (QuestPartners.h).
    constexpr float PartnerMeetYards = 30.0f;
    // ... and walks to where the leader stands now when it has moved this far from where the walk was set.
    constexpr float PartnerMovedYards = 30.0f;

    float Dist2d(float x1, float y1, float x2, float y2)
    {
        float const dx = x1 - x2;
        float const dy = y1 - y2;
        return std::sqrt(dx * dx + dy * dy);
    }

    // A member of a dungeon group on its way back to its leader: the hostile creature in no fight nearest to it whose
    // notice (its aggro range and 4 yd, 6 yd up or down) the navmesh's way passes (`corners`, DungeonNav::FindWay), or
    // none. A creature the leader stands within that reach of does not count (the group stands there). The owner's leader,
    // left behind after a restart in Dire Maul West, walked to its group through Petrified Treants nobody had fought,
    // round 260: [quote left out].
    Creature* QuietCreatureOnWay(Player* walker, Player* leader, std::vector<NavPoint> const& corners, float length)
    {
        if (corners.size() < 2)
            return nullptr;
        Creature* nearest = nullptr;
        float nearestYards = 0.0f;
        float const range = std::min(300.0f, length + 20.0f);
        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(walker, walker, range);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(walker, units, check);
        Cell::VisitObjects(walker, searcher, range);
        for (Unit* unit : units)
        {
            Creature* creature = unit->ToCreature();
            if (!creature || creature->IsPet() || creature->IsTotem() || creature->IsCivilian() || creature->IsInEvadeMode() ||
                !creature->IsHostileTo(walker) || creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE |
                UNIT_FLAG_IMMUNE_TO_PC) || creature->IsInCombat() || creature->GetVictim() || creature->HasReactState(REACT_PASSIVE))
                continue;
            float const reach = creature->GetAggroRange(walker) + 4.0f;
            if (creature->GetExactDist(leader) <= reach)
                continue;
            for (size_t i = 1; i < corners.size(); ++i)
            {
                NavPoint const& a = corners[i - 1];
                NavPoint const& b = corners[i];
                float const dx = b.x - a.x, dy = b.y - a.y;
                float const span = dx * dx + dy * dy;
                float t = span > 0.01f ? ((creature->GetPositionX() - a.x) * dx + (creature->GetPositionY() - a.y) * dy) / span : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);
                float const px = a.x + dx * t, py = a.y + dy * t, pz = a.z + (b.z - a.z) * t;
                if (Dist2d(px, py, creature->GetPositionX(), creature->GetPositionY()) > reach ||
                    std::fabs(pz - creature->GetPositionZ()) > 6.0f)
                    continue;
                float const yards = walker->GetExactDist(creature);
                if (!nearest || yards < nearestYards)
                {
                    nearest = creature;
                    nearestYards = yards;
                }
                break;
            }
        }
        return nearest;
    }

    std::string ZoneName(uint32 zoneId)
    {
        if (ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(zoneId))
            return zone->name;
        std::string const name = GetAreaName(zoneId);
        return name.empty() ? Acore::StringFormat("zone {}", zoneId) : name;
    }

    std::string QuestName(uint32 questId)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        return quest ? quest->GetTitle() : std::to_string(questId);
    }

    uint32 FreeQuestSlots(Player const* player)
    {
        uint32 free = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (!player->GetQuestSlotQuestId(slot))
                ++free;
        return free;
    }

    // The quest giver (starters) or ender of a quest that stands within GiverReach of the player.
    Object* FindQuestNpc(Player* player, uint32 questId, bool starter)
    {
        std::vector<SpawnPoint> const spawns = starter ? sWorldIndex.GetQuestStarters(questId) : sWorldIndex.GetQuestEnders(questId);
        std::vector<std::pair<uint32, bool>> checked;
        for (SpawnPoint const& spawn : spawns)
        {
            if (spawn.mapId != player->GetMapId() ||
                Dist2d(player->GetPositionX(), player->GetPositionY(), spawn.x, spawn.y) > GiverSearchYards)
                continue;
            std::pair<uint32, bool> const key(spawn.entry, spawn.gameObject);
            if (std::find(checked.begin(), checked.end(), key) != checked.end())
                continue;
            checked.push_back(key);
            if (spawn.gameObject)
            {
                GameObject* object = player->FindNearestGameObject(spawn.entry, GiverReach);
                if (object && (starter ? object->hasQuest(questId) : object->hasInvolvedQuest(questId)))
                    return object;
            }
            else
            {
                Creature* creature = player->FindNearestCreature(spawn.entry, GiverReach);
                if (creature && (starter ? creature->hasQuest(questId) : creature->hasInvolvedQuest(questId)))
                    return creature;
            }
        }
        return nullptr;
    }

    // The reward a member picks: the item playerbots rates best for it, else the one that sells for most.
    uint32 RewardChoice(Player* player, Quest const* quest)
    {
        // A tank or healer by its dungeon role: when no choice is an upgrade for levelling, the one that improves its
        // dungeon set most (levelling comes first).
        if (int32 const forRole = sRoleService.RewardChoice(player, quest); forRole >= 0)
        {
            bool levelling = false;
            for (uint32 i = 0; i < QUEST_REWARD_CHOICES_COUNT && !levelling; ++i)
                levelling = quest->RewardChoiceItemId[i] && BotDriver::IsUpgrade(player, quest->RewardChoiceItemId[i], 0);
            if (!levelling)
                return uint32(forRole);
        }
        uint32 best = 0;
        float bestScore = -1.0f;
        uint32 bestPrice = 0;
        for (uint32 i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
        {
            uint32 const item = quest->RewardChoiceItemId[i];
            if (!item)
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            float const score = BotDriver::ItemScore(player, item, 0);
            uint32 const price = proto ? proto->SellPrice : 0;
            if (score > bestScore || (score == bestScore && price > bestPrice))
            {
                best = i;
                bestScore = score;
                bestPrice = price;
            }
        }
        return best;
    }

    // What the client's "complete quest" does at the ender (WorldSession::HandleQuestgiverChooseRewardOpcode), without
    // its distance check: the member stands a few yards behind its leader.
    bool RewardAt(Player* player, Quest const* quest, Object* ender)
    {
        uint32 const choice = RewardChoice(player, quest);
        if (!player->CanRewardQuest(quest, choice, false))
            return false;
        player->RewardQuest(quest, choice, ender);
        if (Creature* creature = ender->ToCreature())
        {
            if (!sScriptMgr->OnQuestReward(player, creature, quest, choice))
                creature->AI()->sQuestReward(player, quest, choice);
        }
        else if (GameObject* object = ender->ToGameObject())
        {
            if (!sScriptMgr->OnQuestReward(player, object, quest, choice))
                object->AI()->QuestReward(player, quest, choice);
        }
        return true;
    }

    // The first creature that fights `player` (or its pet), for a squad character that helps.
    Unit* FirstAttacker(Player* player)
    {
        if (Unit* victim = player->GetVictim(); victim && victim->IsAlive())
            return victim;
        for (Unit* attacker : player->getAttackers())
            if (attacker && attacker->IsAlive())
                return attacker;
        if (Pet* pet = player->GetPet())
            for (Unit* attacker : pet->getAttackers())
                if (attacker && attacker->IsAlive())
                    return attacker;
        return nullptr;
    }
}

// The hand-in of a finished quest at its ender, as the client's "complete quest" does it (the reward playerbots rates
// best): for a squad member beside its leader, and for a bot NewRpg's visit did not hand in for.
bool OrderService::HandInAt(Player* player, Quest const* quest, Object* ender)
{
    return RewardAt(player, quest, ender);
}

void OrderService::UpdateSquad(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    Settings const& settings = GetSettings();
    BotRuntime::Squad& squad = rt.squad;
    uint32 const now = getMSTime();
    // (Set again below while it waits for its leader to come back for it.)
    squad.waitsForFetch = false;

    Player* leader = nullptr;
    // (The guild leader's own character can be a member of a dungeon group: OrderService::StartDungeonGroup.)
    SquadState const state = sSquadService.GetState(guild, bot.guid, &leader);
    bool following = state == SquadState::Active && leader;
    // A member below the level cap does not follow a leader that waits at an inn at the cap with no quest left for it: it
    // levels on its own until its leader has quests again (round 142: a level-58 member stood beside its capped level-60
    // leader at the Cenarion Hold inn for half an hour). Not a quest partner (its pair's rules part it).
    uint32 const capLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    bool leaderCapIdle = false;
    if (following && player->GetLevel() < capLevel && leader->GetLevel() >= capLevel && !sSquadService.GetPartnerQuest(bot.guid))
        if (auto lead = _runtime.find(leader->GetGUID()); lead != _runtime.end() && lead->second.capIdleUntil)
        {
            leaderCapIdle = true;
            following = false;
        }

    // Quest partners (QuestPartners.h): the quest of the partner squad it is in; a member that has not met its leader yet
    // walks to it (StepPartnerMeet) instead of following: playerbots' follow walks straight, and the catch-up teleport
    // is for a way the leader has just taken, not for the way to a new partner.
    uint32 const memberQuest = bot.guid == guild.leader ? 0 : sSquadService.GetPartnerQuest(bot.guid);
    squad.partnerQuest = memberQuest ? memberQuest : sSquadService.GetLedPartnerQuest(bot.guid);
    bool meeting = false;
    if (following && memberQuest && !sSquadService.HasMet(bot.guid))
    {
        if (leader->GetMapId() == player->GetMapId() && player->IsWithinDistInMap(leader, PartnerMeetYards))
        {
            sSquadService.MarkMet(bot.guid);
            LOG_INFO(ActivityLog, "activity: {} met its quest partner {} for quest {} \"{}\": it follows it until the quest is done",
                bot.name, leader->GetName(), memberQuest, QuestName(memberQuest));
            // The leader leaves what it was doing (a walk elsewhere, an errand) for the quest: its next plan takes the
            // quest first (PlanQuests). Not on a ship or a flight.
            if (auto lead = _runtime.find(leader->GetGUID()); lead != _runtime.end() && !leader->IsInFlight() && !leader->GetTransport() &&
                lead->second.task.kind != TaskKind::Voyage)
            {
                if (lead->second.task.kind != TaskKind::None && lead->second.task.questId != memberQuest)
                    ClearTask(lead->second, 0, "its quest partner has come");
                lead->second.errandQuest = 0;
                lead->second.errandStuck = 0;
            }
        }
        else
            meeting = true;
    }
    // A member on another level than its leader with no way on foot to it walks to it along the road map, a lift
    // included, as a quest partner walks to its leader; no teleport (the owner: bots move like players, rescues are a
    // workaround). Sunwhisper rode the lift of Freewind Post (the Mesa Elevator) up, and its member Ashmire's follow steps ran
    // straight into the cliff, every one refused, "it gives that up" once a minute, until it was 250 yd behind and the
    // catch-up put it beside its leader, round 223. In the open world; a dungeon run's group keeps its own rule. Once
    // on its way it walks until it stands within 20 yd of its leader on its level (a lift's ride on the way included).
    bool rejoining = false;
    if (following && !meeting && !player->GetMap()->IsDungeon() && !leader->GetMap()->IsDungeon() && player->IsAlive())
    {
        bool const sameMap = leader->GetMapId() == player->GetMapId();
        bool const beside = sameMap && player->IsWithinDistInMap(leader, LagEndYards) &&
            std::fabs(player->GetPositionZ() - leader->GetPositionZ()) < RejoinHeightYards;
        // (Not while a lift of its route carries it, or it waits at the stop: Ashmire stepped onto the Mesa Elevator of
        // Freewind Post, came "beside" its leader 13 s into the ride, its walk ended and the ride with it, and it stood
        // inside the mesa at the next stop's height until the rescue, round 226. The walk ends after the ride.)
        bool const onLift = (rt.task.partnerMeet && (rt.task.liftAt || rt.task.liftRiding)) || player->GetTransport();
        if (squad.rejoining && onLift && !squad.meetFailed)
            rejoining = true;
        else if (squad.rejoining && squad.meetFailed && squad.rejoinFar && !squad.meetAt && sameMap && ChooseMeetingPoint(player, rt, leader))
        {
            // Far behind and no way of its own to its leader: a meeting point near it, and its leader comes to fetch it
            // there (as a player waits for a friend at the flight master). Ashmire (26) found no way from Orgrimmar to
            // its leader at Whitereach Post: the walk from the flight of Freewind Post passes two level-28 creatures that
            // stand together, its leader (29) had walked it, and the member was put beside it, round 226.
            squad.meetFailed = false;
            rejoining = true;
            LOG_INFO(ActivityLog, "activity: {} finds no way of its own to its squad leader {} ({:.0f} yd away): it goes to {} at ({:.0f}, "
                "{:.0f}) and waits there for its leader to fetch it", bot.name, leader->GetName(), player->GetDistance2d(leader),
                squad.meetName, squad.meetX, squad.meetY);
        }
        else if (squad.rejoining && squad.meetFailed)
        {
            // The way found no way either: the member follows as before, and the way is tried again later. A member far
            // behind may then be put beside its leader (the catch-up below, Squads.CatchUpFallback).
            squad.rejoinPauseUntil = now + RejoinPauseMs;
            squad.rejoinFallback = squad.rejoinFar;
            LOG_INFO(ActivityLog, "activity: {} finds no way {} its squad leader {}{}", bot.name, squad.rejoinFar ? "to" : "back to",
                leader->GetName(), squad.rejoinFar ? (squad.meetAt ? Acore::StringFormat(" and was not fetched at {}", squad.meetName) :
                    std::string()) : Acore::StringFormat(" either: it follows it again and tries the way in {} min",
                    RejoinPauseMs / (MINUTE * IN_MILLISECONDS)));
        }
        else if (squad.rejoining && squad.rejoinFar)
            rejoining = !(sameMap && player->IsWithinDistInMap(leader, FarRejoinDoneYards));
        else if (squad.rejoining)
            rejoining = sameMap && !beside;
        // (Nor while one of the two swims or dives: one at the surface with its leader 10 yd below it, beside it on the
        // map, "had no way on foot to it" and set out along the roads, Lake Mereldar, round 230. The water is no level.
        // The map is asked: the core's own swim state is not kept for a bot.)
        // (Only near its leader: one far behind travels after it as a player would, by flight too (the catch-up below).
        // Sunwhisper flew from Thunder Bluff to Freewind Post, its member Ashmire "had no way on foot to it (4865 yd
        // away, 87 yd below it)" and walked 7898 yd of road for 13 minutes while its leader waited at the flight
        // master, 2026-11-26.)
        else if (sameMap && now >= squad.rejoinPauseUntil && !leader->IsInFlight() && !leader->GetTransport() && !player->GetTransport() &&
            player->IsWithinDistInMap(leader, settings.squadsCatchUpYards) &&
            !player->GetMap()->IsInWater(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                player->GetCollisionHeight()) &&
            !leader->GetMap()->IsInWater(leader->GetPhaseMask(), leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(),
                leader->GetCollisionHeight()) &&
            std::fabs(player->GetPositionZ() - leader->GetPositionZ()) >= RejoinHeightYards && !beside)
        {
            // (Not while its leader stands where no walkable ground is: on a lift's platform between two stops. A
            // level-50 priest set out for its leader on the Cauldron's lift in the second the leader's ride ended at a
            // middle stop, 14 yd above the lower one; its walk went for that spot in the shaft and was refused for two
            // minutes, 2026-11-27.)
            if (!GroundGuard::HasGroundAt(player, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()))
                rejoining = false;
            else
            {
                if (!squad.wayCheckedAt || getMSTimeDiff(squad.wayCheckedAt, now) >= DungeonWayCheckMs)
                {
                    squad.wayCheckedAt = std::max<uint32>(now, 1);
                    squad.noWayToLeader = !WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                        leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
                }
                rejoining = squad.noWayToLeader;
            }
        }
    }
    if (rejoining && !squad.rejoining)
        LOG_INFO(ActivityLog, "activity: {} has no way on foot to its squad leader {} ({:.0f} yd away, {:.0f} yd {} it): it walks to it "
            "along the roads", bot.name, leader->GetName(), player->GetDistance(leader), std::fabs(player->GetPositionZ() - leader->GetPositionZ()),
            leader->GetPositionZ() > player->GetPositionZ() ? "above" : "below");
    // (Only when it is back with it: a walk that ends because the quest partners parted, or the leader is gone, is no
    // "back beside its squad leader ?", round 226; the task's line says why it ended.)
    else if (!rejoining && squad.rejoining && !squad.meetFailed && following && leader)
        LOG_INFO(ActivityLog, "activity: {} is back {} its squad leader {}{}: it follows it again", bot.name, squad.rejoinFar ? "near" : "beside",
            leader->GetName(), squad.meetWaiting ? Acore::StringFormat(", which fetched it at {}", squad.meetName) : std::string());
    if (!rejoining)
    {
        squad.rejoinFar = false;
        squad.meetAt = false;
        squad.meetWaiting = false;
        squad.meetWaitSince = 0;
    }
    squad.rejoining = rejoining;
    if (rejoining)
        meeting = true;
    if (!meeting && rt.task.partnerMeet)
        ClearTask(rt, 0, following ? (squad.rejoined ? "back beside its squad leader" : "met its quest partner") : "the quest partners parted");
    squad.rejoined = rejoining;
    squad.meeting = meeting;
    if (meeting)
    {
        if (squad.following && !rejoining)
            LOG_INFO(ActivityLog, "activity: {} walks to its quest partner {} again", bot.name, leader->GetName());
        squad.following = false;
        squad.leader = leader->GetGUID();
        squad.state = uint8(state);
        squad.leads = false;
        squad.besideLeader = false;
        squad.farSince = 0;
        squad.waitFor.clear();
        return;
    }
    squad.meetFailed = false;

    if (following != squad.following || (following && squad.leader != leader->GetGUID()))
    {
        if (following)
        {
            LOG_INFO(ActivityLog, "activity: {} (level {}) follows its squad leader {} (level {}) now", bot.name,
                uint32(player->GetLevel()), leader->GetName(), uint32(leader->GetLevel()));
            ClearTask(rt, 0, "joined its squad");
        }
        else
        {
            LOG_INFO(ActivityLog, "activity: {} plays its own order again: {}", bot.name, leaderCapIdle ?
                Acore::StringFormat("its leader {} waits at an inn at the level cap with no quest left for it, and it is level {}",
                    leader->GetName(), uint32(player->GetLevel())) : std::string(GuildAdventure::Describe(state)));
            BotDriver::Rest(player);
            rt.zoneChosenAt = 0;
        }
        squad.farSince = 0;
        squad.shared.clear();
        rt.why.clear();
    }
    squad.following = following;
    squad.leader = following ? leader->GetGUID() : ObjectGuid::Empty;
    squad.state = uint8(state);

    // A member far behind: its leader flew, took a ship or a portal, or revived at a spirit healer. It takes the same
    // way after it: a short wait (the leader may come back), then it stands beside it.
    if (following)
    {
        squad.leads = false;
        squad.waitFor.clear();
        // Beside its leader the member fights the squad's fight; alone (left behind, fighting on its way to catch up)
        // it judges its own fights (FightEscape).
        squad.besideLeader = leader->GetMapId() == player->GetMapId() && player->IsWithinDistInMap(leader, AssistYards);
        bool const leaderBusy = !leader->IsAlive() || leader->IsInFlight() || leader->IsBeingTeleported() || leader->GetTransport() ||
            leader->IsInCombat();
        bool isFar = leader->GetMapId() != player->GetMapId() || !player->IsWithinDistInMap(leader, settings.squadsCatchUpYards);
        // In a dungeon run a member that has no way on foot to its leader (it was brought back where it died, on
        // another level; it fell or swam elsewhere) is far behind from 60 yd on, and it catches up out of a fight it
        // has fought alone for a minute too: alone it is no match for what lives there. (Maraudon's inner part: the
        // healer and the hunter fought alone for seven and four minutes, 100 to 150 yd from the group and two levels
        // below it, and died; the group waited for them.)
        bool dungeonRun = false;
        // (And from 20 yd on with 8 yd or more of height between them: Dire Maul West, 2026-11-21, the rogue stood on top
        // of Immol'thar's force field 18 yd above the group, 27 yd from its leader, and the leader waited for it for good.)
        // (From 3 yd of height: a warlock stood on the walkway of Twilight Lord Kelris's room 5 yd below the raised floor
        // its group had got onto in a fight, 26 yd away with no way up, and the group waited for it, Blackfathom Deeps,
        // 2026-11-25.)
        if (!isFar && player->GetMap()->IsDungeon() && leader->GetMapId() == player->GetMapId() &&
            (!player->IsWithinDistInMap(leader, DungeonCatchUpYards) ||
                (std::fabs(player->GetPositionZ() - leader->GetPositionZ()) >= 3.0f && !player->IsWithinDistInMap(leader, LagEndYards))))
            if (GuildBot const* leaderBot = guild.FindMember(leader->GetGUID()); leaderBot && leaderBot->order.type == OrderType::Dungeon)
            {
                uint32 const checkNow = getMSTime();
                if (!squad.wayCheckedAt || getMSTimeDiff(squad.wayCheckedAt, checkNow) >= DungeonWayCheckMs)
                {
                    squad.wayCheckedAt = std::max<uint32>(checkNow, 1);
                    // The whole navmesh first (DungeonNav): a winding way of hundreds of yards that WalkReaches cannot
                    // follow is still a way, and the member walks it (ViaDungeonPath) instead of being put beside its leader.
                    std::vector<NavPoint> corners;
                    float length = 0.0f;
                    bool const navWay = sDungeonNav.FindWay(player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
                        player->GetPositionZ(), leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(), &corners, &length,
                        player->GetMap());
                    squad.noWayToLeader = !navWay &&
                        !WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                            leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ());
                    // A way that passes creatures in no fight: the member does not walk it alone (they would come at it
                    // with nobody of its group beside it); it waits where it is and its group comes back for it, the tank
                    // first, as players do for one that lost them.
                    squad.quietOnWay.Clear();
                    squad.quietName.clear();
                    // (Only for one that lost its group, 40 yd or more from its leader: one a few yards below it on a ramp
                    // that a creature notices is helped by its group, which stands right there.)
                    if (navWay && !player->IsInCombat() && !player->IsWithinDistInMap(leader, 40.0f))
                        if (Creature* quiet = QuietCreatureOnWay(player, leader, corners, length))
                        {
                            squad.quietOnWay = quiet->GetGUID();
                            squad.quietName = Acore::StringFormat("{} (level {}, {:.0f} yd from it)", quiet->GetName(),
                                uint32(quiet->GetLevel()), player->GetExactDist(quiet));
                        }
                    // (No way up, but one down: the leader comes back for it, as a group does for one that fell behind,
                    // instead of the member being put beside it.)
                    squad.leaderCanCome = (squad.noWayToLeader || !squad.quietOnWay.IsEmpty()) && sDungeonNav.FindWay(player->GetMapId(),
                        leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(), player->GetPositionX(), player->GetPositionY(),
                        player->GetPositionZ(), nullptr, nullptr, player->GetMap());
                }
                // The leader comes back for it (for up to a minute and a half, two and a half for a way past creatures:
                // the group fights them on its way): the member waits where it is.
                bool const pastCreatures = !squad.noWayToLeader && !squad.quietOnWay.IsEmpty();
                if ((squad.noWayToLeader || pastCreatures) && squad.leaderCanCome)
                {
                    uint32 const fetchNow = std::max<uint32>(getMSTime(), 1);
                    if (!squad.fetchSince)
                    {
                        squad.fetchSince = fetchNow;
                        if (pastCreatures)
                            LOG_INFO(ActivityLog, "activity: {} waits where it is in {}: its way to {} ({:.0f} yd away) passes {}, "
                                "which is in no fight; its group comes back for it", bot.name, player->GetMap()->GetMapName(),
                                leader->GetName(), player->GetDistance(leader), squad.quietName);
                        BotDriver::Rest(player);
                    }
                    if (getMSTimeDiff(squad.fetchSince, fetchNow) < uint32(pastCreatures ? 150 : 90) * IN_MILLISECONDS)
                    {
                        if (BotRuntime* leaderRt = FindRuntime(leader->GetGUID()))
                        {
                            leaderRt->squad.fetchMember = player->GetGUID();
                            leaderRt->squad.fetchUntil = fetchNow + 5 * IN_MILLISECONDS;
                            leaderRt->squad.fetchWhy = pastCreatures ? Acore::StringFormat("would pass {} on its way, which is in no fight",
                                squad.quietName) : std::string("has no way up to it");
                        }
                        squad.waitsForFetch = true;
                        squad.farSince = 0;
                        return;
                    }
                }
                else
                    squad.fetchSince = 0;
                dungeonRun = squad.noWayToLeader;
                isFar = dungeonRun;
                // A way there that mod-playerbots' follow does not walk (hundreds of yards, or down a drop): the member
                // walks the navmesh's way itself instead of being put beside its leader (the owner: no porting in dungeons).
                if (!squad.noWayToLeader && !rt.rest.active && !player->IsInCombat() &&
                    WalkDungeonWay(player, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()))
                {
                    squad.farSince = 0;
                    return;
                }
            }

        bool const fightingAlone = dungeonRun && player->IsInCombat() && rt.fight.active &&
            getMSTimeDiff(rt.fight.startedAt, getMSTime()) >= DungeonAloneFightMs;
        // Not in C'Thun's chamber: the raid's places there are its own (StepRaidCthun), and a member that came out of
        // his stomach or stood on a ledge was put beside its leader, onto the melee ring, where the Giant Eye's beam
        // chained through the bunch (2026-11-24).
        if (dungeonRun && leader->GetMapId() == 531)
            if (GuildAdventure::BotRuntime const* leaderRt = GetRuntime(leader->GetGUID()); leaderRt && !leaderRt->dungeon.cthunSlot.empty())
                isFar = false;
        // Not for a leader on its way to a dungeon run with the run's teleport: the run takes the group in from wherever
        // its characters are. (The owner's Upper Blackrock Spire group: eight of its fifteen set out after their leader by
        // ship from Silithus and Dustwallow, and the leader waited for those that caught up, two and a half minutes
        // before the group went in, 2026-11-26.)
        bool const runTakesItIn = GetSettings().dungeonsTeleport && !leader->GetMap()->IsDungeon() && [&guild, leader]()
        {
            GuildBot const* leaderBot = guild.FindMember(leader->GetGUID());
            return leaderBot && leaderBot->order.type == OrderType::Dungeon;
        }();
        if (runTakesItIn && squad.rejoining && squad.rejoinFar)
        {
            squad.rejoining = false;
            squad.rejoinFar = false;
            if (rt.task.kind == TaskKind::Travel || rt.task.kind == TaskKind::Voyage)
                ClearTask(rt, 0, "the dungeon run takes it in");
        }
        // A member that rests finishes its rest first (it would arrive half dead beside a leader that fights).
        if (!isFar || runTakesItIn || (leaderBusy && !dungeonRun) || !player->IsAlive() || (player->IsInCombat() && !fightingAlone) || player->IsInFlight() ||
            player->IsBeingTeleported() || (rt.rest.active && !dungeonRun))
            squad.farSince = 0;
        else if (!squad.farSince)
            squad.farSince = now;
        else if (getMSTimeDiff(squad.farSince, now) >= CatchUpAfterMs)
        {
            squad.farSince = 0;
            float const away = leader->GetMapId() == player->GetMapId() ? player->GetDistance2d(leader) : -1.0f;
            // It travels after its leader as a player would: on foot, by flight, ship, zeppelin or portal (the owner: bots
            // move like players, no teleports). Only when that travel found no way at all is it put beside it, and only
            // with Squads.CatchUpFallback (a line in the activity log). A dungeon run keeps its own rules (it teleports
            // its group in and out; a member with no way on foot inside is put beside its leader).
            bool const travel = !dungeonRun && !player->GetMap()->IsDungeon() && !leader->GetMap()->IsDungeon();
            // (After a way that found nothing it waits RejoinPauseMs before it tries again, unless it is put beside it.)
            if (travel && !squad.rejoinFallback && now < squad.rejoinPauseUntil)
                return;
            if (travel && !squad.rejoinFallback)
            {
                squad.rejoining = true;
                squad.rejoinFar = true;
                squad.meetFailed = false;
                LOG_INFO(ActivityLog, "activity: {} is {} its squad leader {} (in {}): it travels after it as a player would", bot.name,
                    away < 0.0f ? std::string("on another map than") : Acore::StringFormat("{:.0f} yd behind", away), leader->GetName(),
                    ZoneName(leader->GetZoneId()));
                return;
            }
            if (travel)
            {
                squad.rejoinFallback = false;
                if (!sBotSettings.IsOn(player, Setting::SquadsCatchUpFallback))
                {
                    LOG_INFO(ActivityLog, "activity: {} found no way to its squad leader {}: it waits and tries the way again in {} "
                        "min (Squads.CatchUpFallback is off)", bot.name, leader->GetName(), RejoinPauseMs / (MINUTE * IN_MILLISECONDS));
                    return;
                }
                LOG_INFO(ActivityLog, "activity: {} found no way to its squad leader {} by road, flight, ship, zeppelin or portal: it is "
                    "put beside it (Squads.CatchUpFallback)", bot.name, leader->GetName());
            }
            Position const spot = leader->GetNearPosition(2.0f, float(M_PI));
            BotDriver::LeaveTransport(player);
            player->StopMoving();
            player->GetMotionMaster()->Clear();
            // Into the leader's instance of a dungeon. A member in another instance of the same map is taken out
            // first: a teleport on the same map stays in its instance (an Uldaman group stood in three instances, and
            // the catch-up put the mage and the healer "beside" their leader in instances of their own).
            if (leader->GetMap()->IsDungeon() && OrderService::BindToLeaderInstance(player, leader) &&
                leader->GetMapId() == player->GetMapId() && !OrderService::SameInstance(player, leader))
            {
                if (AreaTriggerTeleport const* exit = sObjectMgr->GetGoBackTrigger(leader->GetMapId()))
                {
                    player->TeleportTo(exit->target_mapId, exit->target_X, exit->target_Y, exit->target_Z, exit->target_Orientation);
                    rt.hasLastPosition = false;
                    LOG_INFO(ActivityLog, "activity: {} stands in another instance of {} than its squad leader {}: it is taken out "
                        "to go in to it", bot.name, ZoneName(leader->GetZoneId()), leader->GetName());
                }
                return;
            }
            if (player->TeleportTo(leader->GetMapId(), spot.GetPositionX(), spot.GetPositionY(), leader->GetPositionZ(), leader->GetOrientation()))
            {
                rt.hasLastPosition = false;
                LOG_INFO(ActivityLog, "activity: {} caught up with its squad leader {} in {} ({}{})", bot.name, leader->GetName(),
                    ZoneName(leader->GetZoneId()), away < 0.0f ? std::string("it was on another map") :
                    Acore::StringFormat("it was {:.0f} yd behind", away), dungeonRun ? Acore::StringFormat(", with no way on foot to it{}",
                    fightingAlone ? ", out of a fight it fought alone" : "") : std::string());
            }
        }
        return;
    }

    // A leader: wait for a member that needs it.
    std::vector<Player*> const followers = state == SquadState::Leads ? sSquadService.GetFollowers(guild, bot.guid) : std::vector<Player*>();
    squad.leads = !followers.empty();
    // The most pressing reason over all members: a dead one first, then one that rests, one that fights, one behind.
    std::string reason;
    uint8 urgency = 0;
    ObjectGuid whom;
    auto consider = [&reason, &urgency, &whom](uint8 level, std::string text, ObjectGuid who = ObjectGuid::Empty)
    {
        if (level > urgency)
        {
            urgency = level;
            reason = std::move(text);
            whom = who;
        }
    };
    bool partnerComing = false;
    // A member with no way up to it and a way down from it (OrderService.h fetchMember): the leader walks back to it, out
    // of a fight, and waits there; the run's next place is one both reach (StepDungeonRoom).
    if (!squad.fetchMember.IsEmpty())
    {
        Player* member = ObjectAccessor::FindConnectedPlayer(squad.fetchMember);
        uint32 const fetchNow = getMSTime();
        if (!member || fetchNow >= squad.fetchUntil || !SameInstance(member, player) || !member->IsAlive())
            squad.fetchMember.Clear();
        else if (player->GetDistance(member) <= 6.0f)
        {
            // Back with it: a room being cleared is fought from here (the old place is one the member has no way to).
            if (rt.dungeon.roomSince)
            {
                rt.dungeon.roomX = player->GetPositionX();
                rt.dungeon.roomY = player->GetPositionY();
                rt.dungeon.roomZ = player->GetPositionZ();
            }
            LOG_INFO(ActivityLog, "activity: {} is back with {} in {}", bot.name, member->GetName(), player->GetMap()->GetMapName());
            squad.fetchMember.Clear();
        }
        else if (!player->IsInCombat())
        {
            if (WalkDungeonWay(player, member->GetPositionX(), member->GetPositionY(), member->GetPositionZ()))
            {
                std::string const why = squad.fetchWhy.empty() ? std::string("has no way up to it") : squad.fetchWhy;
                if (squad.waitFor != Acore::StringFormat("{} {}", member->GetName(), why))
                    LOG_INFO(ActivityLog, "activity: {} goes back to {} in {}: {} {} ({:.0f} yd away, {:.0f} yd below or above)",
                        bot.name, member->GetName(), player->GetMap()->GetMapName(), member->GetName(), why, player->GetDistance(member),
                        std::fabs(player->GetPositionZ() - member->GetPositionZ()));
                squad.waitFor = Acore::StringFormat("{} {}", member->GetName(), why);
                squad.waitMember.Clear();
                squad.waitUrgency = 6;
                if (!squad.waitSince)
                    squad.waitSince = std::max<uint32>(fetchNow, 1);
                return;
            }
        }
    }
    // A member waiting at its meeting point (it found no way of its own to its leader, StepRejoinTravel): the leader
    // goes to fetch it, by flight when that is the way, and the two go on together.
    if (!player->GetMap()->IsDungeon() && player->IsAlive())
    {
        constexpr uint32 FetchRestartMs = MINUTE * IN_MILLISECONDS;
        constexpr uint32 FetchGiveUpMs = 10 * MINUTE * IN_MILLISECONDS;
        Player* waiting = nullptr;
        BotRuntime* waitingRt = nullptr;
        for (Player* member : followers)
            if (auto itr = _runtime.find(member->GetGUID()); itr != _runtime.end() && itr->second.squad.meetWaiting &&
                member->GetMapId() == player->GetMapId())
            {
                waiting = member;
                waitingRt = &itr->second;
                break;
            }
        if (!waiting)
        {
            squad.fetchingSince = 0;
            if (rt.task.fetchSquad)
                ClearTask(rt, 0, "its squad member is with it again");
        }
        else if (squad.fetchingSince && getMSTimeDiff(squad.fetchingSince, now) >= FetchGiveUpMs)
        {
            // It did not get there: the member's meeting is given up (it may be put beside its leader after all).
            LOG_INFO(ActivityLog, "activity: {} does not get to its squad member {} at {} in {} min: it gives the fetch up", bot.name,
                waiting->GetName(), waitingRt->squad.meetName, FetchGiveUpMs / (MINUTE * IN_MILLISECONDS));
            waitingRt->squad.meetFailed = true;
            waitingRt->squad.meetWaiting = false;
            squad.fetchingSince = 0;
            if (rt.task.fetchSquad)
                ClearTask(rt, 0, "its squad member not reached");
        }
        else if (!player->IsInCombat() && !player->IsInFlight() && !player->GetTransport() && rt.task.kind != TaskKind::Voyage &&
            !rt.task.fetchSquad)
        {
            float const away = player->GetDistance2d(waiting);
            if (!squad.fetchStartedAt || getMSTimeDiff(squad.fetchStartedAt, now) >= FetchRestartMs || away <= JourneyArriveYards)
            {
                if (!squad.fetchingSince)
                {
                    squad.fetchingSince = std::max<uint32>(now, 1);
                    LOG_INFO(ActivityLog, "activity: {} goes to fetch its squad member {} at {} ({:.0f} yd away): {} found no way of its own "
                        "to it", bot.name, waiting->GetName(), waitingRt->squad.meetName, away, waiting->GetName());
                }
                squad.fetchStartedAt = std::max<uint32>(now, 1);
                BotTask task;
                task.kind = TaskKind::Travel;
                task.fetchSquad = true;
                task.journey = away > JourneyArriveYards; // by flight when far, the last stretch on foot
                task.hasSpot = true;
                task.spot.mapId = uint16(waiting->GetMapId());
                task.spot.x = waiting->GetPositionX();
                task.spot.y = waiting->GetPositionY();
                task.spot.z = waiting->GetPositionZ();
                task.targetName = Acore::StringFormat("{} (squad member)", waiting->GetName());
                StartTask(player, rt, task);
                squad.waitFor.clear();
                squad.waitSince = 0;
                squad.waitUrgency = 0;
                squad.waitBesideAt = 0;
                return;
            }
        }
        if (waiting && rt.task.fetchSquad)
        {
            // No waiting for the squad while it fetches (StepSquadWait would hold it where it stands).
            squad.waitFor.clear();
            squad.waitSince = 0;
            squad.waitUrgency = 0;
            squad.waitBesideAt = 0;
            return;
        }
    }

    // In a dungeon run a member that fights within the group's reach comes before everything else, a dead member
    // included: its group goes to help it (OrderService::StepDungeonRescue) and brings the dead back after the fight.
    // (Uldaman: the tank waited two minutes "for Magefrost, who is dead" while the healer fought alone 55 yd behind.)
    bool const dungeonRun = player->GetMap()->IsDungeon() && bot.order.type == OrderType::Dungeon;
    float const assistYards = dungeonRun ? DungeonAssistYards : AssistYards;
    // (On its way to a dungeon run with the run's teleport it waits for nobody behind it: the run takes them in.)
    bool const runTakesItIn = bot.order.type == OrderType::Dungeon && GetSettings().dungeonsTeleport && !player->GetMap()->IsDungeon();
    if (settings.squadsWaitYards > 0.0f && player->IsAlive() && rt.task.kind != TaskKind::Voyage)
    {
        for (Player* member : followers)
        {
            auto itr = _runtime.find(member->GetGUID());
            BotRuntime const* memberRt = itr != _runtime.end() ? &itr->second : nullptr;
            // (The same instance: a member in another instance of a dungeon is not beside its leader, whatever its place.)
            bool const sameMap = SameInstance(member, player);
            float const distance = sameMap ? player->GetDistance2d(member) : -1.0f;
            if (!member->IsAlive())
            {
                // Its body: where it fell until it releases its spirit, then the corpse it runs back to.
                WorldObject const* body = member;
                bool const ghost = member->HasPlayerFlag(PLAYER_FLAGS_GHOST);
                if (ghost)
                    body = member->GetCorpse();
                if (body && body->GetMapId() == player->GetMapId() &&
                    Dist2d(body->GetPositionX(), body->GetPositionY(), player->GetPositionX(), player->GetPositionY()) <= settings.squadsWaitYards)
                    consider(4, Acore::StringFormat(ghost ? "{} is dead and runs back to its corpse" : "{} is dead", member->GetName()));
            }
            else if (sameMap && distance <= settings.squadsWaitYards && memberRt && memberRt->rest.active)
                consider(3, Acore::StringFormat("{} rests", member->GetName()), member->GetGUID());
            else if (sameMap && distance <= assistYards && member->IsInCombat() && (!dungeonRun || [member]()
                {
                    // (In a dungeon only a fight near it: a creature on it from 72 yd away that cannot get to it held a
                    // group for ten minutes.)
                    for (Unit const* attacker : member->getAttackers())
                        if (attacker->IsAlive() && member->IsWithinDistInMap(attacker, 40.0f))
                            return true;
                    Unit const* victim = member->GetVictim();
                    return victim && victim->IsAlive() && member->IsWithinDistInMap(victim, 40.0f);
                }()))
            {
                consider(dungeonRun ? 5 : 2, Acore::StringFormat("{} fights", member->GetName()));
                // Help it: its foe is the leader's foe. (In a dungeon run StepDungeonRescue chooses the foes.)
                if (!player->IsInCombat() && !dungeonRun)
                    if (Unit* foe = FirstAttacker(member))
                        BotDriver::Attack(player, foe);
            }
            else if (memberRt && memberRt->squad.meeting && sameMap && !memberRt->squad.rejoinFar)
            {
                // A quest partner on its way: the leader stands until it has come (as long as the meeting may take).
                consider(1, memberRt->squad.rejoining ? Acore::StringFormat("{} comes back along the roads ({:.0f} yd)", member->GetName(),
                    distance) : Acore::StringFormat("{} comes to team up for \"{}\" ({:.0f} yd)", member->GetName(),
                    QuestName(memberRt->squad.partnerQuest), distance));
                partnerComing = true;
            }
            else if (sameMap && distance > (squad.waitFor.empty() ? LagStartYards : LagEndYards) &&
                distance <= settings.squadsCatchUpYards && !member->IsInFlight() && !runTakesItIn)
                consider(1, Acore::StringFormat("{} catches up ({:.0f} yd behind)", member->GetName(), distance), member->GetGUID());
        }
    }
    // (The Twin Emperors: the run puts each half at its place and holds it there; a leader that waited for the other half,
    // 130 yd away, held the run, and its members walked back to it, 2026-11-23.)
    // (Nor at C'Thun: the hunter that pulls the Eye goes up alone and the raid keeps its places on the rings; the leader
    // waited for the hunter to catch up and the hunter walked back to it, again and again, 2026-11-24.)
    if (rt.dungeon.twinPlacesSince || rt.dungeon.twinPulledAt || !rt.dungeon.cthunSlot.empty() ||
        (rt.dungeon.twinBugsSince && !rt.dungeon.twinBugsDone))
        reason.clear();
    if (reason.empty() || now < squad.noWaitUntil)
    {
        if (!squad.waitFor.empty())
            rt.task.lastAssert = 0; // NewRpg was off while it waited: the task sets its status again
        squad.waitFor.clear();
        squad.waitSince = 0;
        squad.waitUrgency = 0;
        squad.waitBesideAt = 0;
        return;
    }
    if (!squad.waitSince)
        squad.waitSince = now;
    // (In a dungeon run no "120 s, then on" (package P2): the group waits until its member is with it; a member that does
    // not come is put beside the tank after a minute (StepDungeonSpread). Ten minutes as the last word.)
    uint32 const maxWaitMs = dungeonRun ? 10 * MINUTE * IN_MILLISECONDS : settings.squadsMaxWaitSeconds * IN_MILLISECONDS;
    if (getMSTimeDiff(squad.waitSince, now) >= maxWaitMs && !(partnerComing && urgency == 1))
    {
        LOG_INFO(ActivityLog, "activity: {} waited {} s for its squad ({}): goes on", bot.name,
            getMSTimeDiff(squad.waitSince, now) / IN_MILLISECONDS, reason);
        // (The acceptance numbers of a dungeon run: a run went on without a member that rested.)
        if (bot.order.type == OrderType::Dungeon && rt.dungeon.id && reason.find(" rests") != std::string::npos)
            ++rt.dungeon.metrics.wentOnResting;
        squad.noWaitUntil = now + NoWaitAfterLongWaitMs;
        squad.waitSince = 0;
        squad.waitFor.clear();
        squad.waitUrgency = 0;
        squad.waitBesideAt = 0;
        rt.task.lastAssert = 0;
        return;
    }
    if (squad.waitFor.empty() || urgency > squad.waitUrgency)
    {
        LOG_INFO(ActivityLog, "activity: {} waits for its squad: {}", bot.name, reason);
        // (The acceptance numbers of a dungeon run: the leader waits for a member far behind.)
        if (bot.order.type == OrderType::Dungeon && rt.dungeon.id && reason.find(" catches up (") != std::string::npos)
            ++rt.dungeon.metrics.catchUps;
    }
    squad.waitFor = reason;
    squad.waitUrgency = urgency;
    squad.waitMember = whom;
}

bool OrderService::StepSquadWait(Player* player, BotRuntime& rt)
{
    if (rt.squad.waitFor.empty())
        return false;
    // Not where the walk stopped when hostile creatures stand near: beside the member it waits for. A hunter that led
    // a warlock towards Hive'Zora's wasps stopped 60 yd ahead of it while the warlock rested, five wasps of a cluster came
    // at the hunter alone, and it died on its run, round 191.
    constexpr float NearYards = 30.0f;
    constexpr float BesideYards = 8.0f;
    constexpr uint32 StepGapMs = 3 * IN_MILLISECONDS;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    // (Not in a dungeon: creatures stand near wherever a group waits there, and the run has its own rules for one that
    // fell behind. The owner's tank walked 243 yd straight back to his leader through creatures in Dire Maul West, round 260.)
    if (!rt.squad.waitMember.IsEmpty() && player && player->IsAlive() && !player->IsInCombat() && !player->GetMap()->IsDungeon() &&
        (!rt.squad.waitBesideAt || getMSTimeDiff(rt.squad.waitBesideAt, now) >= StepGapMs))
        if (Player* member = ObjectAccessor::FindPlayer(rt.squad.waitMember);
            member && member->IsAlive() && SameInstance(member, player) && player->GetExactDist2d(member) > BesideYards &&
            Escape::HostilesAround(player, NearYards) > 0)
        {
            if (!rt.squad.waitBesideAt)
                LOG_INFO(ActivityLog, "activity: {} waits for its squad beside {}: hostile creatures stand within {:.0f} yd of where it "
                    "stopped", player->GetName(), member->GetName(), NearYards);
            rt.squad.waitBesideAt = now;
            BotDriver::Rest(player);
            player->GetMotionMaster()->MovePoint(0, member->GetPositionX(), member->GetPositionY(), member->GetPositionZ());
        }
    // The task's clocks stand still, as in a fight.
    if (rt.task.kind != TaskKind::None)
    {
        rt.task.startedAt += rt.stepGapMs;
        if (rt.task.nearSince)
            rt.task.nearSince += rt.stepGapMs;
    }
    PauseStuck(rt);
    rt.strandSince = 0;
    // Inside a dungeon run the line names the run, as the conductor's lines do (the owner, 2026-11-27: [quote left out]); its members show it as their leader's (RunSquadMember).
    DungeonPlan const* runPlan = rt.dungeon.id && rt.dungeon.phase == 2 ? sDungeons.GetPlan(rt.dungeon.id) : nullptr;
    rt.why = runPlan ? Acore::StringFormat("In {}: waiting for its squad ({})", runPlan->name, rt.squad.waitFor) :
        Acore::StringFormat("Squad: waiting ({})", rt.squad.waitFor);
    return true;
}

bool OrderService::StepSquadFlee(Player* player, BotRuntime& rt)
{
    if (!rt.squad.following || !player->IsAlive())
        return false;
    auto itr = _runtime.find(rt.squad.leader);
    Player* leader = ObjectAccessor::FindConnectedPlayer(rt.squad.leader);
    // (Also when its leader keeps away from a pack on its way or goes out of a camp, which is no fight of the
    // leader's: a member that followed its leader into the Kurzen compound stayed in a fight against three when the
    // leader went out, and died there. Not when the leader steps aside for one roaming creature: the member looks out
    // for that itself.)
    if (itr == _runtime.end() || !leader || !leader->IsInWorld() || !itr->second.escape.active ||
        (itr->second.escape.aside && !itr->second.escape.pack) || leader->GetMapId() != player->GetMapId())
        return false;

    // Its leader runs from a fight it would lose: the member stops fighting and runs at its heels.
    if (player->GetVictim())
        player->AttackStop();
    player->InterruptNonMeleeSpells(false);
    if (Pet* pet = player->GetPet(); pet && pet->IsAlive())
    {
        pet->AttackStop();
        pet->GetMotionMaster()->MoveFollow(player, PET_FOLLOW_DIST, pet->GetFollowAngle());
    }
    BotDriver::PauseAI(player, 2 * IN_MILLISECONDS);
    if (player->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
    {
        player->GetMotionMaster()->Clear();
        player->GetMotionMaster()->MoveFollow(leader, 1.5f, float(M_PI));
    }
    rt.why = Acore::StringFormat("Squad: running from a fight with {}", leader->GetName());
    return true;
}

std::string OrderService::LeaderWhyForMember(BotRuntime const* leader)
{
    if (!leader || leader->why.empty())
        return {};
    std::string why = leader->why;
    if (why.rfind("Squad: waiting (", 0) == 0)
        return "waiting for its squad" + why.substr(std::string("Squad: waiting").size());
    if (why.rfind("Squad:", 0) == 0)
        return {};
    if (leader->dungeon.id)
        if (DungeonPlan const* plan = sDungeons.GetPlan(leader->dungeon.id))
            if (std::string const prefix = "In " + plan->name + ": "; why.rfind(prefix, 0) == 0)
                why = why.substr(prefix.size());
    return why;
}

void OrderService::RunSquadMember(AdventureGuild& /*guild*/, GuildBot& bot, Player* player, BotRuntime& rt)
{
    if (rt.task.kind != TaskKind::None)
        ClearTask(rt, 0, "follows its squad leader");
    Player* leader = ObjectAccessor::FindConnectedPlayer(rt.squad.leader);
    if (!leader || !leader->IsInWorld())
        return;
    // A member's own walk of NewRpg is over while it follows (its follow is mod-playerbots' weakest action, and any
    // NewRpg walk wins over it). The owner's guild leader, watched by its owner in a dungeon group, kept the walk to its
    // zone's camp it had begun before the run for 33 minutes, stood 25 to 54 yd behind its tank at every pack and held
    // the group at each gate for up to five minutes, Sunken Temple, 2026-11-22.
    switch (BotDriver::GetRpgState(player))
    {
        case RpgState::GoGrind:
        case RpgState::GoCamp:
        case RpgState::WanderRandom:
        case RpgState::WanderNpc:
        case RpgState::DoQuest:
            BotDriver::Rest(player);
            break;
        default:
            break;
    }

    auto leaderItr = _runtime.find(rt.squad.leader);
    // Its leader takes a lift: the member stays where it is (mod-playerbots' follow does not board a lift; a warlock that
    // followed its leader off the Mesa Elevator's top at Freewind Post walked out over the canyon, stood 54 yd above the
    // floor and was rescued, 2026-11-25), and catches up by its own way when the ride is over.
    if (leaderItr != _runtime.end() && leaderItr->second.task.liftAt && !rt.task.liftAt && leader->GetMapId() == player->GetMapId() &&
        !player->IsInCombat() && !player->GetTransport())
    {
        if (player->isMoving())
        {
            player->StopMoving();
            player->GetMotionMaster()->Clear();
        }
        BotDriver::PauseAI(player, 2 * std::max(TickMs, rt.stepGapMs));
        if (rt.why.find(" rides the lift") == std::string::npos)
            LOG_INFO(ActivityLog, "activity: {} waits while its squad leader {} rides the lift (it does not follow onto it)", bot.name,
                leader->GetName());
        rt.why = Acore::StringFormat("Squad: waiting while {} rides the lift", leader->GetName());
        return;
    }
    // It waits where it is while its leader comes back for it (UpdateSquad: no way up to it, or a way past creatures in
    // no fight): mod-playerbots' follow would walk it that way. On the j5 instance a shaman left on Shadowfang Keep's
    // upper walkway walked 70 yd towards its leader on its own while the leader came for it, into two Wailing Guardsmen.
    if (rt.squad.waitsForFetch && !player->IsInCombat() && !player->GetTransport())
    {
        if (player->isMoving())
        {
            player->StopMoving();
            player->GetMotionMaster()->Clear();
        }
        BotDriver::PauseAI(player, 2 * std::max(TickMs, rt.stepGapMs));
        rt.why = Acore::StringFormat("Squad: waiting where it is for {} to come back for it", leader->GetName());
        return;
    }
    std::string const with = rt.squad.partnerQuest ? Acore::StringFormat("{}, its quest partner for \"{}\"", leader->GetName(),
        QuestName(rt.squad.partnerQuest)) : leader->GetName();
    if (!leader->IsAlive())
        rt.why = Acore::StringFormat("Squad: waiting for {} to come back to life", with);
    else if (std::string const lead = LeaderWhyForMember(leaderItr != _runtime.end() ? &leaderItr->second : nullptr); lead.empty())
        rt.why = Acore::StringFormat("Squad: following {}", with);
    else
        rt.why = Acore::StringFormat("Squad: following {}; {}: {}", with, leader->GetName(), lead);

    // Its leader fights and it does not yet: it helps.
    if (leader->IsInCombat() && !player->IsInCombat() && player->IsWithinDistInMap(leader, AssistYards))
        if (Unit* foe = FirstAttacker(leader))
            BotDriver::Attack(player, foe);

    uint32 const now = getMSTime();
    if (now < rt.squad.nextQuests)
        return;
    rt.squad.nextQuests = now + QuestCheckMs;
    BotRuntime::Squad& squad = rt.squad;

    // The quests its leader took at the giver they stand at.
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = leader->GetQuestSlotQuestId(slot);
        if (!questId || player->GetQuestStatus(questId) != QUEST_STATUS_NONE || player->GetQuestRewardStatus(questId))
            continue;
        if (auto skip = squad.skipShare.find(questId); skip != squad.skipShare.end() && now < skip->second)
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;
        Object* giver = FindQuestNpc(player, questId, true);
        if (!giver)
            continue;
        if (!player->CanTakeQuest(quest, false) || !player->CanAddQuest(quest, false))
        {
            squad.skipShare[questId] = now + ShareSkipMs;
            continue;
        }
        player->AddQuestAndCheckCompletion(quest, giver);
        if (player->GetQuestStatus(questId) == QUEST_STATUS_NONE)
        {
            squad.skipShare[questId] = now + ShareSkipMs;
            continue;
        }
        squad.shared.insert(questId);
        LOG_INFO(ActivityLog, "activity: {} took quest {} \"{}\" with its squad leader {}", bot.name, questId, quest->GetTitle(),
            leader->GetName());
    }

    // Completed quests whose ender stands here: handed in.
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = player->GetQuestSlotQuestId(slot);
        if (!questId || player->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;
        Object* ender = FindQuestNpc(player, questId, false);
        if (!ender)
            continue;
        if (RewardAt(player, quest, ender))
        {
            squad.shared.erase(questId);
            LOG_INFO(ActivityLog, "activity: {} turned in quest {} \"{}\" beside its squad leader {}", bot.name, questId,
                quest->GetTitle(), leader->GetName());
        }
    }

    // Shared quests its leader gave up (abandoned, or handed in while the member's part is not done and its log runs
    // short): the member gives them up too, so its log stays the squad's.
    bool const shortLog = FreeQuestSlots(player) < MemberFreeSlots;
    for (auto itr = squad.shared.begin(); itr != squad.shared.end();)
    {
        uint32 const questId = *itr;
        QuestStatus const status = player->GetQuestStatus(questId);
        if (status == QUEST_STATUS_NONE || status == QUEST_STATUS_REWARDED)
        {
            itr = squad.shared.erase(itr);
            continue;
        }
        bool const leaderHas = leader->FindQuestSlot(questId) < MAX_QUEST_LOG_SIZE;
        bool const leaderDone = leader->GetQuestRewardStatus(questId);
        if (!leaderHas && status != QUEST_STATUS_COMPLETE && (!leaderDone || shortLog))
        {
            BotDriver::AbandonQuest(player, questId);
            LOG_INFO(ActivityLog, "activity: {} gave up quest {} \"{}\": its squad leader {} {} it", bot.name, questId, QuestName(questId),
                leader->GetName(), leaderDone ? "handed in" : "gave up");
            itr = squad.shared.erase(itr);
            continue;
        }
        ++itr;
    }
}

void OrderService::OnSquadMemberDied(Player* member, BotRuntime& rt)
{
    if (!rt.squad.following)
        return;
    auto itr = _runtime.find(rt.squad.leader);
    if (itr == _runtime.end())
        return;
    // A quest partner that died while its leader worked their quest: a death of the guild on it (QuestPartners.h).
    if (rt.squad.partnerQuest && itr->second.task.questId == rt.squad.partnerQuest)
        sQuestPartners.OnQuestDeath(member, rt.squad.partnerQuest);
    // The place counts for the squad: the leader plans as if it had died there itself (the member's level and position).
    if (std::string const note = Risk::OnDeath(member, itr->second, rt.lastFoeLevel); !note.empty())
        LOG_INFO("module", "mod-guild-adventure: {} died in the squad of {}: {}", member->GetName(), itr->second.name, note);
}

void OrderService::KeepSquadGroup(Player* player, BotRuntime const& rt, Player* master)
{
    // A member joins its leader's party; the party loots free for all, so that every member loots the squad's kills
    // (quest items are each member's own whatever the method). On a dungeon run of its leader the party loots by
    // group loot: what is uncommon or better is rolled for (a bot rolls for what is an upgrade for it and passes on
    // the rest, mod-playerbots' "loot roll"), and what nobody rolls for is taken by whoever stands there and goes the
    // loot filter's way (the owner, 2026-11-12: [quote left out]).
    // (The leader of a raid run: its party is a raid group, which holds the whole group; docs/orders.md "Raid runs".)
    if (Group* party = master->GetGroup(); party && !party->isRaidGroup() && party->IsFull() && party->IsLeader(master->GetGUID()) &&
        LeadsRaidRun(master))
        party->ConvertToRaid();
    BotDriver::JoinGroupOf(player, master);
    Group* group = player->GetGroup();
    if (!group || !rt.squad.following || group->GetLeaderGUID() != master->GetGUID() || !sGuildBotHolder.IsAIPlayed(master->GetGUID()))
        return;
    auto lead = _runtime.find(master->GetGUID());
    bool const onRun = lead != _runtime.end() && lead->second.dungeon.id != 0;
    LootMethod const wanted = onRun ? GROUP_LOOT : FREE_FOR_ALL;
    if (group->GetLootMethod() == wanted && (!onRun || group->GetLootThreshold() == ITEM_QUALITY_UNCOMMON))
        return;
    group->SetLootMethod(wanted);
    if (onRun)
    {
        group->SetLootThreshold(ITEM_QUALITY_UNCOMMON);
        LOG_INFO(ActivityLog, "activity: the group of {} rolls for its loot on this dungeon run (group loot from uncommon up: "
            "a bot rolls for what is an upgrade for it, the rest goes the loot filter's way)", master->GetName());
    }
    group->SendUpdate();
}

void OrderService::StepRejoinTravel(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    Player* leader = ObjectAccessor::FindConnectedPlayer(rt.squad.leader);
    if (!leader || !leader->IsInWorld())
        return;
    if (!player->IsAlive() || player->IsInCombat() || player->IsInFlight() || player->IsBeingTeleported())
        return;
    // A voyage under way goes on (ship, zeppelin, portal); after it the next leg or the travel on the other side.
    if (rt.task.kind == TaskKind::Voyage)
    {
        StepTask(guild, bot, player, rt);
        return;
    }
    Region const here = GetRegion(player);
    Region const there = GetRegion(leader);
    if (here != there || leader->GetMapId() != player->GetMapId())
    {
        TravelLink const* link = sWorldIndex.NextTravelLink(here, there, player->GetTeamId(), player->GetMapId(), player->GetPositionX(),
            player->GetPositionY(), leader->GetMapId(), leader->GetPositionX(), leader->GetPositionY());
        if (!link)
        {
            rt.squad.meetFailed = true;
            LOG_INFO(ActivityLog, "activity: {} finds no ship, zeppelin or portal for its side from {} to {}, where its squad leader {} is",
                bot.name, ToString(here), ToString(there), leader->GetName());
            return;
        }
        LOG_INFO(ActivityLog, "activity: {} travels after its squad leader {} to {}: {} from {} to {}", bot.name, leader->GetName(),
            ToString(there), link->name, link->fromName, link->toName);
        StartVoyage(player, rt, *link);
        rt.task.partnerMeet = true;
        rt.why = Acore::StringFormat("Squad: travelling after {} ({})", leader->GetName(), link->name);
        return;
    }
    // To its meeting point (no way of its own to its leader, UpdateSquad): there it waits for its leader to fetch it.
    BotRuntime::Squad& squad = rt.squad;
    if (squad.meetAt && squad.meetMap == player->GetMapId())
    {
        float const toMeet = player->GetExactDist2d(squad.meetX, squad.meetY);
        if (toMeet <= MeetArriveYards)
        {
            uint32 const now = std::max<uint32>(getMSTime(), 1);
            if (!squad.meetWaiting)
            {
                squad.meetWaiting = true;
                squad.meetWaitSince = now;
                LOG_INFO(ActivityLog, "activity: {} waits at {} for its squad leader {} to fetch it ({:.0f} yd away)", bot.name, squad.meetName,
                    leader->GetName(), player->GetDistance2d(leader));
            }
            else if (getMSTimeDiff(squad.meetWaitSince, now) >= MeetWaitMs)
            {
                squad.meetFailed = true;
                squad.meetWaiting = false;
                return;
            }
            if (rt.task.kind != TaskKind::None)
                ClearTask(rt, 0, "at its meeting point");
            BotDriver::Rest(player);
            rt.why = Acore::StringFormat("Squad: waiting at {} for {} to fetch it", squad.meetName, leader->GetName());
            return;
        }
        bool const fresh = rt.task.kind != TaskKind::Travel || !rt.task.partnerMeet ||
            Dist2d(squad.meetX, squad.meetY, rt.task.spot.x, rt.task.spot.y) > 1.0f;
        if (fresh)
        {
            BotTask task;
            task.kind = TaskKind::Travel;
            task.partnerMeet = true;
            task.journey = toMeet > JourneyArriveYards;
            task.hasSpot = true;
            task.spot.mapId = uint16(squad.meetMap);
            task.spot.x = squad.meetX;
            task.spot.y = squad.meetY;
            task.spot.z = squad.meetZ;
            task.targetName = squad.meetName;
            StartTask(player, rt, task);
        }
        else if (rt.task.journey && toMeet <= JourneyArriveYards)
        {
            rt.task.journey = false;
            rt.task.routePlanned = false;
        }
        rt.why = Acore::StringFormat("Squad: travelling to {} to meet {} ({:.0f} yd)", squad.meetName, leader->GetName(), toMeet);
        if (rt.task.journey)
            StepTask(guild, bot, player, rt);
        else
        {
            WalkTo(player, rt, rt.task.spot.mapId, rt.task.spot.x, rt.task.spot.y, rt.task.spot.z);
            if (UpdateStuck(player, rt, rt.task.spot.x, rt.task.spot.y))
                OnTravelStuck(bot, rt, toMeet);
        }
        return;
    }
    // The last stretch: a walk to where the leader stands, as a quest partner walks to its leader.
    if (player->GetDistance2d(leader) <= FarRejoinWalkYards)
    {
        rt.task.journey = false;
        StepPartnerMeet(guild, bot, player, rt);
        return;
    }
    // On its land mass: a travel to where the leader stands, flights included; the spot moves with the leader.
    bool const fresh = rt.task.kind != TaskKind::Travel || !rt.task.partnerMeet;
    if (fresh || Dist2d(leader->GetPositionX(), leader->GetPositionY(), rt.task.spot.x, rt.task.spot.y) > FarRejoinDoneYards)
    {
        BotTask task;
        task.kind = TaskKind::Travel;
        task.partnerMeet = true;
        task.journey = true; // to a spot, not to its zone (it arrives 250 yd from it, the walk does the rest)
        task.hasSpot = true;
        task.spot.mapId = uint16(leader->GetMapId());
        task.spot.x = leader->GetPositionX();
        task.spot.y = leader->GetPositionY();
        task.spot.z = leader->GetPositionZ();
        task.targetName = Acore::StringFormat("{} (squad leader)", leader->GetName());
        if (fresh)
            StartTask(player, rt, task);
        else
        {
            rt.task.spot = task.spot;
            rt.task.journey = true;
            rt.task.routePlanned = false;
        }
    }
    rt.why = Acore::StringFormat("Squad: travelling after {} ({:.0f} yd)", leader->GetName(), player->GetDistance2d(leader));
    StepTask(guild, bot, player, rt);
}

bool OrderService::ChooseMeetingPoint(Player* player, BotRuntime& rt, Player* leader)
{
    // The flight master of the member's side nearest to its leader, on their map: a place a member reaches by flight and
    // a leader by its own way. Not one the member is near already (it found no way from there), not one farther from
    // the leader than a fetch is worth.
    constexpr float NotHereYards = 200.0f;
    constexpr float FetchMaxYards = 3000.0f;
    ServicePoint const* best = nullptr;
    float bestDistance = FetchMaxYards;
    for (ServicePoint const& service : sWorldIndex.GetServices(ServiceType::FlightMaster))
    {
        if (service.spawn.mapId != leader->GetMapId() || !WorldIndex::IsFriendlyFaction(service.factionTemplate, player))
            continue;
        if (player->GetExactDist2d(service.spawn.x, service.spawn.y) <= NotHereYards)
            continue;
        float const distance = leader->GetExactDist2d(service.spawn.x, service.spawn.y);
        if (distance >= bestDistance || distance >= player->GetDistance2d(leader))
            continue;
        if (rt.IsInAvoidedArea(service.spawn.mapId, service.spawn.x, service.spawn.y, player->GetLevel()))
            continue;
        best = &service;
        bestDistance = distance;
    }
    if (!best)
        return false;
    BotRuntime::Squad& squad = rt.squad;
    squad.meetAt = true;
    squad.meetWaiting = false;
    squad.meetWaitSince = 0;
    squad.meetMap = best->spawn.mapId;
    squad.meetX = best->spawn.x;
    squad.meetY = best->spawn.y;
    squad.meetZ = best->spawn.z;
    squad.meetName = Acore::StringFormat("the flight master {} in {}", best->name, ZoneName(sMapMgr->GetZoneId(PHASEMASK_NORMAL,
        best->spawn.mapId, best->spawn.x, best->spawn.y, best->spawn.z)));
    return true;
}

void OrderService::StepPartnerMeet(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    Player* leader = ObjectAccessor::FindConnectedPlayer(rt.squad.leader);
    if (!leader || !leader->IsInWorld())
        return;
    // A member far behind travels after its leader (ships, flights); the last stretch comes back here.
    if (rt.squad.rejoining && rt.squad.rejoinFar && (leader->GetMapId() != player->GetMapId() ||
        player->GetDistance2d(leader) > FarRejoinWalkYards || rt.task.kind == TaskKind::Voyage || rt.squad.meetAt))
    {
        StepRejoinTravel(guild, bot, player, rt);
        return;
    }
    std::string const title = QuestName(rt.squad.partnerQuest);
    bool const rejoining = rt.squad.rejoining;
    (void)guild;
    if (leader->GetMapId() != player->GetMapId())
    {
        rt.squad.meetFailed = true;
        rt.why = Acore::StringFormat(rejoining ? "Squad: {} is on another map" : "Quest partner: {} is on another map", leader->GetName());
        return;
    }
    float const distance = player->GetDistance2d(leader);
    rt.why = rejoining ? Acore::StringFormat("Squad: walking back to {} along the roads ({:.0f} yd; no way on foot from here)",
        leader->GetName(), distance) :
        Acore::StringFormat("Quest partner: walking to {} to team up for \"{}\" ({:.0f} yd)", leader->GetName(), title, distance);
    if (!player->IsAlive() || player->IsInCombat() || player->IsInFlight() || player->IsBeingTeleported())
        return;

    // The walk goes to where the leader stands (it waits for the member); when it has moved on (a fight, a quest giver),
    // the walk is set again.
    bool const fresh = rt.task.kind != TaskKind::Travel || !rt.task.partnerMeet;
    if (fresh || Dist2d(leader->GetPositionX(), leader->GetPositionY(), rt.task.spot.x, rt.task.spot.y) > PartnerMovedYards)
    {
        BotTask task;
        task.kind = TaskKind::Travel;
        task.partnerMeet = true;
        task.hasSpot = true;
        task.spot.mapId = uint16(leader->GetMapId());
        task.spot.x = leader->GetPositionX();
        task.spot.y = leader->GetPositionY();
        task.spot.z = leader->GetPositionZ();
        task.targetName = Acore::StringFormat(rejoining ? "{} (squad leader)" : "{} (quest partner)", leader->GetName());
        if (fresh)
            StartTask(player, rt, task);
        else
        {
            // (The same walk to a point that moved: the stuck clocks go on, the route is looked for again.)
            rt.task.spot = task.spot;
            rt.task.routePlanned = false;
        }
    }
    WalkTo(player, rt, rt.task.spot.mapId, rt.task.spot.x, rt.task.spot.y, rt.task.spot.z);
    if (UpdateStuck(player, rt, rt.task.spot.x, rt.task.spot.y))
        OnTravelStuck(bot, rt, distance);
}
