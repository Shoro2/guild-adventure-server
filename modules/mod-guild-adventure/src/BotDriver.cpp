/*
 * BotDriver implementation. See BotDriver.h.
 */

#include "BotDriver.h"
#include "Ammunition.h"
#include "AiFactory.h"
#include "Pet.h"
#include "PlayerbotRepository.h"
#include "Spell.h"
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "AiObjectContext.h"
#include "Bag.h"
#include "BotSettings.h"
#include "DBCStores.h"
#include "Engine.h"
#include "FightEscape.h"
#include "FightProfiles.h"
#include "Group.h"
#include "GuildAdventureSettings.h"
#include "GuildBotHolder.h"
#include "GroupMgr.h"
#include "Item.h"
#include "ItemEnchantmentMgr.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "LootObjectStack.h"
#include "MotionMaster.h"
#include "NewRpgInfo.h"
#include "NonCombatActions.h"
#include "PaladinActions.h"
#include "PaladinBlessingAction.h"
#include "HunterActions.h"
#include "MageActions.h"
#include "ShamanActions.h"
#include "MovementActions.h"
#include "ChooseTargetActions.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "PlayerbotGuildMgr.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "QuestPackets.h"
#include "QuestSupport.h"
#include "SpellAuraDefines.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "StatsWeightCalculator.h"
#include "Timer.h"
#include "Transport.h"
#include "TravelMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <cstring>
#include <atomic>
#include <shared_mutex>
#include "DatabaseEnv.h"

using namespace GuildAdventure;

namespace
{
    // Non-combat strategies the profiles switch. Everything else of playerbots' defaults (food, loot, gather, buff,
    // mount, quest packets, chat) stays as it is.
    constexpr char const* AdventureStrategies = "+new rpg,+grind,-follow,-stay,-rpg,-travel,-move random";
    // Without "grind" a walking bot does not run after every quest mob in sight (it still fights back).
    constexpr char const* TravelStrategies = "+new rpg,-grind,-follow,-stay,-rpg,-travel,-move random";
    constexpr char const* FollowStrategies = "+follow,-new rpg,-grind,-stay,-rpg,-travel,-move random";
    constexpr char const* HoldStrategies = "-new rpg,-grind,-follow,-stay,-rpg,-travel,-move random";
}

char const* GuildAdventure::ToString(StrategyProfile profile)
{
    switch (profile)
    {
        case StrategyProfile::None:      return "none";
        case StrategyProfile::Adventure: return "adventure";
        case StrategyProfile::Travel:    return "travel";
        case StrategyProfile::Follow:    return "follow";
        case StrategyProfile::Hold:      return "hold";
    }
    return "none";
}

char const* GuildAdventure::ToString(RpgState state)
{
    switch (state)
    {
        case RpgState::Idle:         return "idle";
        case RpgState::GoGrind:      return "go grind";
        case RpgState::GoCamp:       return "go camp";
        case RpgState::WanderRandom: return "wander random";
        case RpgState::WanderNpc:    return "wander npc";
        case RpgState::DoQuest:      return "do quest";
        case RpgState::TravelFlight: return "travel flight";
        case RpgState::Rest:         return "rest";
        case RpgState::Other:        return "other";
    }
    return "other";
}

PlayerbotAI* BotDriver::GetAI(Player* bot)
{
    // (A placeholder AI is no AI for the module: its character is played by hand, GuildBotHolder::StartPlaceholder.)
    if (!bot || GuildAdventure::IdleAI::IsSet(bot->GetGUID()))
        return nullptr;
    return GET_PLAYERBOT_AI(bot);
}

bool BotDriver::HasProfile(Player* bot, StrategyProfile profile)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return false;

    bool const rpg = ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT);
    bool const grind = ai->HasStrategy("grind", BOT_STATE_NON_COMBAT);
    bool const follow = ai->HasStrategy("follow", BOT_STATE_NON_COMBAT);
    switch (profile)
    {
        case StrategyProfile::Adventure: return rpg && grind && !follow;
        case StrategyProfile::Travel:    return rpg && !grind && !follow;
        case StrategyProfile::Follow:    return follow && !rpg && !grind;
        case StrategyProfile::Hold:      return !rpg && !grind && !follow;
        case StrategyProfile::None:      return true;
    }
    return false;
}

void BotDriver::ApplyProfile(Player* bot, StrategyProfile profile)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return;

    switch (profile)
    {
        case StrategyProfile::Adventure:
            ai->ChangeStrategy(AdventureStrategies, BOT_STATE_NON_COMBAT);
            break;
        case StrategyProfile::Travel:
            ai->ChangeStrategy(TravelStrategies, BOT_STATE_NON_COMBAT);
            break;
        case StrategyProfile::Follow:
            ai->ChangeStrategy(FollowStrategies, BOT_STATE_NON_COMBAT);
            break;
        case StrategyProfile::Hold:
            ai->ChangeStrategy(HoldStrategies, BOT_STATE_NON_COMBAT);
            break;
        case StrategyProfile::None:
            break;
    }
}

Player* BotDriver::GetMaster(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    return ai ? ai->GetMaster() : nullptr;
}

void BotDriver::SetMaster(Player* bot, Player* master)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai || master == bot)
        return;
    ai->SetMaster(master);
}

void BotDriver::LeaveNormalGroup(Player* bot)
{
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group || group->isBGGroup() || group->isBFGroup() || group->isLFGGroup())
        return;

    bot->RemoveFromGroup();
}

bool BotDriver::JoinGroupOf(Player* bot, Player* master)
{
    if (!bot || !master || bot == master)
        return false;

    Group* group = master->GetGroup();
    if (group && bot->GetGroup() == group)
        return true;

    if (bot->GetGroup())
        LeaveNormalGroup(bot);
    if (bot->GetGroup())
        return false;

    if (!group)
    {
        group = new Group;
        if (!group->Create(master))
        {
            delete group;
            return false;
        }
        sGroupMgr->AddGroup(group);
    }

    if (group->isBGGroup() || group->isBFGroup() || group->isLFGGroup() || group->IsFull())
        return false;

    return group->AddMember(bot);
}

RpgState BotDriver::GetRpgState(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return RpgState::Other;

    switch (ai->rpgInfo.GetStatus())
    {
        case RPG_IDLE:          return RpgState::Idle;
        case RPG_GO_GRIND:      return RpgState::GoGrind;
        case RPG_GO_CAMP:       return RpgState::GoCamp;
        case RPG_WANDER_RANDOM: return RpgState::WanderRandom;
        case RPG_WANDER_NPC:    return RpgState::WanderNpc;
        case RPG_DO_QUEST:      return RpgState::DoQuest;
        case RPG_TRAVEL_FLIGHT: return RpgState::TravelFlight;
        case RPG_REST:          return RpgState::Rest;
        default:                return RpgState::Other;
    }
}

uint32 BotDriver::GetRpgStateSeconds(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    return ai ? GetMSTimeDiffToNow(ai->rpgInfo.startT) / IN_MILLISECONDS : 0;
}

uint32 BotDriver::GetRpgQuest(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return 0;
    if (auto const* data = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data))
        return data->questId;
    return 0;
}

namespace
{
    // Until when the module holds each bot's AI (PauseAI, any length): a new way for NewRpg does not cut such a hold.
    std::mutex moduleHoldLock;
    std::unordered_map<ObjectGuid, uint32> moduleHolds;

    struct NextCheckPeek : PlayerbotAIBase
    {
        static uint32 Of(PlayerbotAIBase const* ai) { return ai->*(&NextCheckPeek::nextAICheckDelay); }
    };

    // A new way for NewRpg: what holds the AI now is the wait of its last short move, up to Playerbots.MaxWaitForMove
    // (at a quest's place NewRpg walks 8 yd at random and the AI sleeps until that walk should be over), not something
    // the new way must wait for. The new task's walk set off 6 to 10 s after the planner gave it (Nayeli, a druid of the
    // Horde test guild, stood 31 to 54 s in five minutes so, between its spawns of "Dream Dust in the Swamp", 2026-11-27).
    void WakeForNewWay(PlayerbotAI* ai, Player* bot)
    {
        constexpr uint32 MoveWaitMs = 5500; // just above Playerbots.MaxWaitForMove (5000)
        if (bot->IsInCombat() || bot->IsNonMeleeSpellCast(false) || !bot->IsAlive())
            return;
        uint32 const delay = NextCheckPeek::Of(ai);
        if (!delay || delay > MoveWaitMs)
            return;
        {
            std::lock_guard<std::mutex> guard(moduleHoldLock);
            if (auto const found = moduleHolds.find(bot->GetGUID()); found != moduleHolds.end() &&
                int32(found->second - getMSTime()) > 0)
                return;
        }
        ai->SetNextCheckDelay(0);
    }
}

void BotDriver::GoTo(Player* bot, uint32 mapId, float x, float y, float z, bool grindOnArrival)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return;

    WorldPosition const pos(mapId, x, y, z);
    // (A new way only: the planner asserts the same one again every few seconds.)
    bool same = false;
    if (auto const* camp = std::get_if<NewRpgInfo::GoCamp>(&ai->rpgInfo.data); camp && !grindOnArrival)
        same = camp->pos.GetMapId() == mapId && camp->pos.GetExactDist2d(x, y) < 3.0f;
    else if (auto const* grind = std::get_if<NewRpgInfo::GoGrind>(&ai->rpgInfo.data); grind && grindOnArrival)
        same = grind->pos.GetMapId() == mapId && grind->pos.GetExactDist2d(x, y) < 3.0f;
    if (grindOnArrival)
        ai->rpgInfo.ChangeToGoGrind(pos);
    else
        ai->rpgInfo.ChangeToGoCamp(pos);
    if (!same)
        WakeForNewWay(ai, bot);
}

void BotDriver::DoQuest(Player* bot, Quest const* quest, int32 objectiveIndex, bool hasSpot, float x, float y, float z)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai || !quest)
        return;

    bool same = false;
    if (auto const* data = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data))
        same = data->questId == quest->GetQuestId() && (!hasSpot || data->pos.GetExactDist2d(x, y) < 3.0f);
    ai->rpgInfo.ChangeToDoQuest(quest->GetQuestId(), quest);
    if (!same)
        WakeForNewWay(ai, bot);
    if (!hasSpot)
        return;

    if (auto* data = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data))
    {
        data->pos = WorldPosition(bot->GetMapId(), x, y, z);
        data->objectiveIdx = objectiveIndex;
        data->lastReachPOI = 0;
    }
}

void BotDriver::Visit(Player* bot, ObjectGuid target)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return;

    auto const* before = std::get_if<NewRpgInfo::WanderNpc>(&ai->rpgInfo.data);
    if (!before || before->npcOrGo != target)
        WakeForNewWay(ai, bot);
    ai->rpgInfo.ChangeToWanderNpc();
    if (auto* data = std::get_if<NewRpgInfo::WanderNpc>(&ai->rpgInfo.data))
    {
        data->npcOrGo = target;
        data->lastReach = 0;
    }
    // The planner sends the bot to this NPC on purpose: a give-up from an earlier approach in passing (NewRpg's own, or
    // the ground guard's, GiveUpQuestGiverAt) does not hold for it.
    ai->rpgInfo.ignoredQuestGivers.erase(target);
}

bool BotDriver::IsRealGuild(uint32 guildId)
{
    return PlayerbotGuildMgr::instance().IsRealGuild(guildId);
}

// World thread, while the maps do not update (as playerbots' own hourly validation, BotGuildCacheWorldScript).
void BotDriver::ReloadRealGuilds()
{
    PlayerbotGuildMgr::instance().ValidateGuildCache();
}

bool BotDriver::IsVisiting(Player* bot, ObjectGuid target)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return false;
    auto const* data = std::get_if<NewRpgInfo::WanderNpc>(&ai->rpgInfo.data);
    return data && data->npcOrGo == target;
}

bool BotDriver::HasReachedVisitTarget(Player* bot, ObjectGuid target)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return false;
    auto const* data = std::get_if<NewRpgInfo::WanderNpc>(&ai->rpgInfo.data);
    return data && data->npcOrGo == target && data->lastReach;
}

void BotDriver::SetMountAllowed(Player* bot, bool allowed)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return;
    if (ai->HasStrategy("mount", BOT_STATE_NON_COMBAT) != allowed)
        ai->ChangeStrategy(allowed ? "+mount" : "-mount", BOT_STATE_NON_COMBAT);
    if (!allowed && bot->IsMounted())
    {
        bot->Dismount();
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
    }
}

bool BotDriver::HasLootTarget(Player* bot, float range, std::string* name, WorldObject** found)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return false;
    LootObject target = ai->GetAiObjectContext()->GetValue<LootObject>("loot target")->Get();
    if (target.IsEmpty() || !target.IsLootPossible(bot))
        return false;
    WorldObject* object = target.GetWorldObject(bot);
    if (!object || bot->GetDistance(object) > range)
        return false;
    if (name)
        *name = object->GetName();
    if (found)
        *found = object;
    return true;
}

void BotDriver::DeferLootTarget(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return;
    AiObjectContext* context = ai->GetAiObjectContext();
    LootObject target = context->GetValue<LootObject>("loot target")->Get();
    if (target.IsEmpty())
        return;
    if (LootObjectStack* stack = context->GetValue<LootObjectStack*>("available loot")->Get())
        stack->DeferLoot(target.guid);
    context->GetValue<LootObject>("loot target")->Set(LootObject());
}

bool BotDriver::EquipForLevel(Player* bot, uint32 quality)
{
    if (!GetAI(bot))
        return false;
    PlayerbotFactory factory(bot, bot->GetLevel(), std::clamp<uint32>(quality, ITEM_QUALITY_UNCOMMON, ITEM_QUALITY_EPIC));
    factory.InitBags(false);
    factory.InitEquipment(false);
    factory.InitAmmo();
    return true;
}

bool BotDriver::Attack(Player* bot, Unit* target, bool requireSight)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai || !target || !target->IsInWorld() || !target->IsAlive() || !bot->IsAlive() || !bot->IsValidAttackTarget(target) ||
        (requireSight && !bot->IsWithinLOSInMap(target)))
        return false;

    // What AttackAction::Attack does once its checks passed.
    AiObjectContext* context = ai->GetAiObjectContext();
    Unit* const old = context->GetValue<Unit*>("current target")->Get();
    if (old == target && bot->GetVictim() == target)
        return true;
    bot->SetSelection(target->GetGUID());
    context->GetValue<Unit*>("old target")->Set(old);
    context->GetValue<Unit*>("current target")->Set(target);
    ai->ChangeEngine(BOT_STATE_COMBAT);
    bot->Attack(target, bot->IsWithinMeleeRange(target) || PlayerbotAI::IsMelee(bot));
    return true;
}

bool BotDriver::FightsInMelee(Player* bot)
{
    return bot && GetAI(bot) && PlayerbotAI::IsMelee(bot);
}

float BotDriver::StandRange(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    return ai ? ai->GetRange(bot->getClass() == CLASS_HUNTER ? "shoot" : "spell") : 0.0f;
}

bool BotDriver::SetCombatStrategy(Player* bot, char const* name, bool on)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai || ai->HasStrategy(name, BOT_STATE_COMBAT) == on)
        return false;
    ai->ChangeStrategy(std::string(on ? "+" : "-") + name, BOT_STATE_COMBAT);
    return true;
}

bool BotDriver::SetStrategyEverywhere(Player* bot, char const* name, bool on)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return false;
    bool changed = false;
    for (BotState state : { BOT_STATE_COMBAT, BOT_STATE_NON_COMBAT })
        if (ai->HasStrategy(name, state) != on)
        {
            ai->ChangeStrategy(std::string(on ? "+" : "-") + name, state);
            changed = true;
        }
    return changed;
}

bool BotDriver::HasCombatStrategy(Player* bot, char const* name)
{
    PlayerbotAI* ai = GetAI(bot);
    return ai && ai->HasStrategy(name, BOT_STATE_COMBAT);
}

uint32 BotDriver::BlessingLeftTo(Player* paladin, ObjectGuid& target)
{
    target.Clear();
    // (GET_PLAYERBOT_AI, not GetAI: the placeholder AI of a hand-played character is what the plan counts.)
    PlayerbotAI* ai = paladin && paladin->getClass() == CLASS_PALADIN ? GET_PLAYERBOT_AI(paladin) : nullptr;
    if (!ai)
        return 0;
    ai::blessing::PaladinBlessingPlanner planner(ai);
    ai::blessing::PendingBlessing const next = planner.Plan();
    target = next.target;
    return next.spellId;
}

std::string BotDriver::ComplementTotems(Player* first, Player* second)
{
    PlayerbotAI* one = GetAI(first);
    PlayerbotAI* two = GetAI(second);
    if (!one || !two || first->getClass() != CLASS_SHAMAN || second->getClass() != CLASS_SHAMAN)
        return "";
    // Per element: the strategy, its totem spell, and the other one of the element.
    struct Swap { char const* strategy; char const* other; char const* otherSpell; };
    static Swap const Swaps[] = {
        { "strength of earth", "stoneskin", "stoneskin totem" },
        { "stoneskin", "strength of earth", "strength of earth totem" },
        { "magma", "flametongue", "flametongue totem" },
        { "searing", "flametongue", "flametongue totem" },
        { "wrath", "flametongue", "flametongue totem" },
        { "flametongue", "searing", "searing totem" },
        { "mana spring", "healing stream", "healing stream totem" },
        { "healing stream", "mana spring", "mana spring totem" },
        { "windfury", "wrath of air", "wrath of air totem" },
        { "wrath of air", "windfury", "windfury totem" },
    };
    std::string changed;
    for (Swap const& swap : Swaps)
    {
        if (!one->HasStrategy(swap.strategy, BOT_STATE_COMBAT) || !two->HasStrategy(swap.strategy, BOT_STATE_COMBAT) ||
            two->HasStrategy(swap.other, BOT_STATE_COMBAT) || !SpellIdByName(second, swap.otherSpell))
            continue;
        two->ChangeStrategy(std::string("-") + swap.strategy, BOT_STATE_COMBAT);
        two->ChangeStrategy(std::string("+") + swap.other, BOT_STATE_COMBAT);
        changed += (changed.empty() ? "" : ", ") + std::string(swap.other) + " for " + swap.strategy;
    }
    if (!changed.empty())
        PlayerbotRepository::instance().Save(two);
    return changed;
}

uint32 BotDriver::SpellIdByName(Player* bot, std::string const& name)
{
    PlayerbotAI* ai = GetAI(bot);
    Value<uint32>* id = ai ? ai->GetAiObjectContext()->GetValue<uint32>("spell id", name) : nullptr;
    return id ? id->Get() : 0;
}

Unit* BotDriver::IntendedTarget(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return nullptr;
    Unit* target = ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    return target ? target : bot->GetVictim();
}

void BotDriver::SetRti(Player* bot, std::string const& icon)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return;
    if (Value<std::string>* value = ai->GetAiObjectContext()->GetValue<std::string>("rti"); value && value->Get() != icon)
        value->Set(icon);
}

void BotDriver::DropTarget(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return;
    ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
    bot->AttackStop();
    bot->InterruptNonMeleeSpells(false);
    bot->SetTarget();
    bot->StopMoving();
    bot->GetMotionMaster()->Clear();
    if (Pet* pet = bot->GetPet())
    {
        pet->AttackStop();
        pet->InterruptNonMeleeSpells(false);
    }
    ai->ChangeEngine(BOT_STATE_NON_COMBAT);
}

namespace
{
    // Until when "attack anything" leaves each creature a bot let go of alone (BotDriver::LetGo). Written in the world
    // thread (Escape::CheckPull), read by the AI in the map threads.
    std::mutex& LetGoLock()
    {
        static std::mutex lock;
        return lock;
    }

    std::unordered_map<ObjectGuid, std::unordered_map<ObjectGuid, uint32>>& LetGoUntil()
    {
        static std::unordered_map<ObjectGuid, std::unordered_map<ObjectGuid, uint32>> until;
        return until;
    }
}

bool BotDriver::IsLetGo(Player const* bot, Unit const* target)
{
    std::lock_guard<std::mutex> guard(LetGoLock());
    auto const mine = LetGoUntil().find(bot->GetGUID());
    if (mine == LetGoUntil().end())
        return false;
    auto const it = mine->second.find(target->GetGUID());
    return it != mine->second.end() && int32(it->second - getMSTime()) > 0;
}

void BotDriver::LetGo(Player* bot, Unit* target)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return;
    // mod-playerbots' "attack anything" leaves it alone for a while (GuildAttackAnythingAction): it picked the same
    // creature again at every look, and each time the walk stopped.
    if (target)
    {
        constexpr uint32 LetGoHoldMs = 30 * IN_MILLISECONDS;
        uint32 const now = getMSTime();
        std::lock_guard<std::mutex> guard(LetGoLock());
        std::unordered_map<ObjectGuid, uint32>& mine = LetGoUntil()[bot->GetGUID()];
        if (mine.size() > 16)
            for (auto it = mine.begin(); it != mine.end();)
                it = int32(it->second - now) <= 0 ? mine.erase(it) : std::next(it);
        mine[target->GetGUID()] = now + LetGoHoldMs;
    }
    ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
    // (And as its old target: mod-playerbots goes back to that one when it has none, and a shaman let go of the Horde's
    // flight master in Light's Hope attacked her again a second later, round 147.)
    if (target && ai->GetAiObjectContext()->GetValue<Unit*>("old target")->Get() == target)
        ai->GetAiObjectContext()->GetValue<Unit*>("old target")->Set(nullptr);
    if (target && bot->GetSelectedUnit() == target)
        bot->SetSelection(ObjectGuid::Empty);
    if (bot->GetVictim() == target)
        bot->AttackStop();
    for (CurrentSpellTypes type : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL, CURRENT_AUTOREPEAT_SPELL })
        if (Spell* spell = bot->GetCurrentSpell(type); spell && target && spell->m_targets.GetUnitTargetGUID() == target->GetGUID())
            bot->InterruptSpell(type, false);
    if (target && bot->GetTarget() == target->GetGUID())
        bot->SetTarget();
    MotionMaster* motion = bot->GetMotionMaster();
    if (motion->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
    {
        bot->StopMoving();
        motion->Clear();
    }
    if (Pet* pet = bot->GetPet(); pet && target && pet->GetVictim() == target)
        pet->AttackStop();
    ai->ChangeEngine(BOT_STATE_NON_COMBAT);
}

void BotDriver::Rest(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return;
    ai->rpgInfo.ChangeToRest();
}

void BotDriver::Fly(Player* bot, uint32 flightMasterEntry, uint32 mapId, float x, float y, float z, std::vector<uint32> const& route)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return;
    ai->rpgInfo.ChangeToTravelFlight(flightMasterEntry, WorldPosition(mapId, x, y, z), route);
}

void BotDriver::Hop(Player* bot, float x, float y, float z)
{
    if (!bot || !bot->IsInWorld())
        return;
    bot->StopMoving();
    bot->GetMotionMaster()->Clear();
    bot->NearTeleportTo(x, y, z, bot->GetOrientation());
}

void BotDriver::LeaveTransport(Player* bot)
{
    if (Transport* transport = bot ? bot->GetTransport() : nullptr)
        transport->RemovePassenger(bot, true);
}

bool BotDriver::HasAIGivenUp(Player* bot, uint32 questId)
{
    PlayerbotAI* ai = GetAI(bot);
    return ai && ai->lowPriorityQuest.count(questId);
}

void BotDriver::ForgetAIGivenUp(Player* bot, uint32 questId)
{
    if (PlayerbotAI* ai = GetAI(bot))
        ai->lowPriorityQuest.erase(questId);
}

void BotDriver::AbandonQuest(Player* bot, uint32 questId)
{
    if (!bot || !bot->GetSession())
        return;

    uint16 const slot = bot->FindQuestSlot(questId);
    if (slot >= MAX_QUEST_LOG_SIZE)
        return;

    // The same path as the client's "abandon quest" (and NewRpg's quest-log cleanup).
    WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
    packet << uint8(slot);
    WorldPackets::Quest::QuestLogRemoveQuest removeQuest(std::move(packet));
    removeQuest.Read();
    bot->GetSession()->HandleQuestLogRemoveQuest(removeQuest);
}

void BotDriver::ResetDeathCount(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return;
    if (Value<uint32>* count = ai->GetAiObjectContext()->GetValue<uint32>("death count"))
        if (count->Get())
            count->Set(0);
}

bool BotDriver::ResurrectAtSpiritHealer(Player* bot, Creature* healer)
{
    if (!bot || !healer || !bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) || !bot->GetSession())
        return false;
    if (!bot->IsWithinDistInMap(healer, INTERACTION_DISTANCE))
        return false;
    bot->StopMoving();
    bot->GetMotionMaster()->Clear();
    WorldPacket packet(CMSG_SPIRIT_HEALER_ACTIVATE, 8);
    packet << healer->GetGUID();
    bot->GetSession()->HandleSpiritHealerActivateOpcode(packet);
    return bot->IsAlive();
}

void BotDriver::BuyFromVendor(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return;
    ai->DoSpecificAction("buy", Event("buy", "vendor"), true);
}

namespace
{
    // The module's last long pause of each bot's AI: who set it (for AIState).
    struct LongPause
    {
        uint32 at = 0;
        uint32 ms = 0;
        char const* file = nullptr;
        uint32 line = 0;
    };
    std::mutex longPauseLock;
    std::unordered_map<ObjectGuid, LongPause> longPauses;
    constexpr uint32 LongPauseMs = 3 * IN_MILLISECONDS;
}

void BotDriver::PauseAI(Player* bot, uint32 ms, std::source_location where)
{
    if (PlayerbotAI* ai = GetAI(bot))
    {
        ai->SetNextCheckDelay(ms);
        {
            std::lock_guard<std::mutex> guard(moduleHoldLock);
            moduleHolds[bot->GetGUID()] = getMSTime() + ms;
        }
        if (ms >= LongPauseMs)
        {
            std::lock_guard<std::mutex> guard(longPauseLock);
            longPauses[bot->GetGUID()] = LongPause{ std::max<uint32>(getMSTime(), 1), ms, where.file_name(), where.line() };
        }
    }
}

void BotDriver::KeepAvoidAoe(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return;
    // (A reset of the strategies, at a change of talents or a login, takes it out again: asserted every few seconds.)
    if (!ai->HasStrategy("avoid aoe", BOT_STATE_COMBAT))
        ai->ChangeStrategy("+avoid aoe", BOT_STATE_COMBAT);
    if (!ai->HasStrategy("avoid aoe", BOT_STATE_NON_COMBAT))
        ai->ChangeStrategy("+avoid aoe", BOT_STATE_NON_COMBAT);
}

namespace
{
    // The hostile area aura of a dynamic object the bot stands in (a cloud, a puddle, a fire), or none.
    Aura const* HostileGroundAura(Player* bot)
    {
        for (auto const& [spellId, application] : bot->GetAppliedAuras())
        {
            Aura const* aura = application ? application->GetBase() : nullptr;
            if (!aura || aura->GetType() != DYNOBJ_AURA_TYPE || aura->IsRemoved() || aura->GetSpellInfo()->IsPositive())
                continue;
            // (Only what does harm: a cloud of a foe, not a friend's consecration or a totem's field.)
            Unit const* caster = aura->GetCaster();
            if (caster && !caster->IsHostileTo(bot))
                continue;
            return aura;
        }
        return nullptr;
    }
}

std::string BotDriver::GroundEffectOn(Player* bot)
{
    Aura const* aura = bot ? HostileGroundAura(bot) : nullptr;
    return aura ? std::string(aura->GetSpellInfo()->SpellName[LOCALE_enUS]) : std::string();
}

DynamicObject const* BotDriver::GroundEffectObject(Player* bot)
{
    Aura const* aura = bot ? HostileGroundAura(bot) : nullptr;
    return aura ? const_cast<Aura*>(aura)->GetDynobjOwner() : nullptr;
}

bool BotDriver::StepOutOfGroundEffect(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return false;
    ai->SetNextCheckDelay(0);
    if (ai->DoSpecificAction("avoid aoe", Event(), true))
        return true;
    // mod-playerbots' action found nothing (its value of the aura is a second old, its flee point had no way, or the
    // effect is wider than Playerbots.MaxAoeAvoidRadius): a step straight out of the effect's circle, away from its
    // middle, else to either side of that.
    Aura const* aura = HostileGroundAura(bot);
    DynamicObject const* dyn = aura ? const_cast<Aura*>(aura)->GetDynobjOwner() : nullptr;
    if (!dyn || !dyn->IsInWorld())
        return false;
    float const radius = dyn->GetRadius();
    if (!radius || bot->GetExactDist2d(dyn) > radius)
        return true;
    float const away = bot->GetExactDist2d(dyn) < 0.5f ? bot->GetOrientation() + float(M_PI) : dyn->GetAngle(bot);
    for (float const turn : { 0.0f, 0.8f, -0.8f, 1.6f, -1.6f })
    {
        float const angle = away + turn;
        float x = dyn->GetPositionX() + std::cos(angle) * (radius + 3.0f);
        float y = dyn->GetPositionY() + std::sin(angle) * (radius + 3.0f);
        float z = bot->GetMapHeight(x, y, bot->GetPositionZ() + 2.0f);
        if (z <= INVALID_HEIGHT || std::fabs(z - bot->GetPositionZ()) > 6.0f || !bot->IsWithinLOS(x, y, z + 1.0f))
            continue;
        bot->GetMotionMaster()->MovePoint(0, x, y, z);
        return true;
    }
    return false;
}

bool BotDriver::GiveUpQuestGiverAt(Player* bot, float x, float y, float z)
{
    constexpr float Reach = 15.0f;
    PlayerbotAI* ai = GetAI(bot);
    if (!ai || !ai->rpgInfo.questGiver)
        return false;
    WorldObject* giver = ObjectAccessor::GetWorldObject(*bot, ai->rpgInfo.questGiver);
    if (!giver || giver->GetExactDist(x, y, z) > Reach)
        return false;
    ai->rpgInfo.ignoredQuestGivers.insert(ai->rpgInfo.questGiver);
    ai->rpgInfo.questGiver.Clear();
    return true;
}

namespace
{
    // The pet a warlock levels with. mod-playerbots gives a demonology warlock the strategy "felguard" and an
    // affliction one "felhunter"; below the level of those demons its fallback is the succubus from level 20, which
    // holds nothing: every creature was on the warlock from its first cast. Until it has its Felguard the warlock
    // keeps a Voidwalker (the strategies of the pets replace one another).
    // The totem strategies of mod-playerbots' shaman, all but "searing".
    char const* const FullTotems[] = { "strength of earth", "stoneskin", "tremor", "earthbind", "magma", "flametongue", "wrath",
        "frost resistance", "healing stream", "mana spring", "cleansing", "fire resistance", "wrath of air", "windfury",
        "nature resistance", "grounding" };

    // In the open world a shaman sets a Searing Totem and nothing else (BotDriver::KeepTotems).
    bool ApplySoloTotems(PlayerbotAI* ai, Player* bot)
    {
        if (bot->getClass() != CLASS_SHAMAN)
            return false;
        Map const* map = bot->FindMap();
        if (!map || map->IsDungeon() || map->IsBattlegroundOrArena())
            return false;
        bool changed = false;
        for (char const* name : FullTotems)
            if (ai->HasStrategy(name, BOT_STATE_COMBAT))
            {
                ai->ChangeStrategy(std::string("-") + name, BOT_STATE_COMBAT);
                changed = true;
            }
        if (!ai->HasStrategy("searing", BOT_STATE_COMBAT))
        {
            ai->ChangeStrategy("+searing", BOT_STATE_COMBAT);
            changed = true;
        }
        return changed;
    }

    void ApplySoloPet(PlayerbotAI* ai, Player* bot)
    {
        constexpr uint32 SummonVoidwalker = 697;
        constexpr uint32 SummonFelguard = 30146;
        if (bot->getClass() != CLASS_WARLOCK || !bot->HasSpell(SummonVoidwalker) || bot->HasSpell(SummonFelguard))
            return;
        if (!ai->HasStrategy("voidwalker", BOT_STATE_NON_COMBAT))
            ai->ChangeStrategy("+voidwalker", BOT_STATE_NON_COMBAT);
    }
}

void BotDriver::ResetStrategies(Player* bot)
{
    if (PlayerbotAI* ai = GetAI(bot))
    {
        ai->ResetStrategies();
        ApplySoloPet(ai, bot);
        ApplySoloTotems(ai, bot);
        // playerbots loads what it stored for the bot at every login (SyncStrategies): store the new ones now, a
        // server that stops without logging the bots out would bring the old ones back.
        PlayerbotRepository::instance().Save(ai);
    }
}

uint8 BotDriver::KeepTotems(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai || bot->getClass() != CLASS_SHAMAN)
        return 0;
    Map const* map = bot->FindMap();
    if (!map)
        return 0;
    if (!map->IsDungeon() && !map->IsBattlegroundOrArena())
    {
        if (!ApplySoloTotems(ai, bot))
            return 0;
        PlayerbotRepository::instance().Save(ai);
        return 1;
    }
    // In a dungeon: what its talents give, when the open world's one totem is still set.
    if (!ai->HasStrategy("searing", BOT_STATE_COMBAT))
        return 0;
    for (char const* name : FullTotems)
        if (ai->HasStrategy(name, BOT_STATE_COMBAT))
            return 0;
    ai->ResetStrategies();
    ApplySoloPet(ai, bot);
    PlayerbotRepository::instance().Save(ai);
    return 2;
}

bool BotDriver::SetTotems(Player* bot, char const* water, char const* air)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai || bot->getClass() != CLASS_SHAMAN)
        return false;
    static char const* const Water[] = { "healing stream", "mana spring", "cleansing", "fire resistance" };
    static char const* const Air[] = { "wrath of air", "windfury", "nature resistance", "grounding" };
    bool changed = false;
    auto slot = [&](char const* const (&names)[4], char const* wanted)
    {
        for (char const* name : names)
            if (std::strcmp(name, wanted) != 0 && ai->HasStrategy(name, BOT_STATE_COMBAT))
            {
                ai->ChangeStrategy(std::string("-") + name, BOT_STATE_COMBAT);
                changed = true;
            }
        if (!ai->HasStrategy(wanted, BOT_STATE_COMBAT))
        {
            ai->ChangeStrategy(std::string("+") + wanted, BOT_STATE_COMBAT);
            changed = true;
        }
    };
    slot(Water, water);
    slot(Air, air);
    if (ai->HasStrategy("searing", BOT_STATE_COMBAT))
    {
        ai->ChangeStrategy("-searing", BOT_STATE_COMBAT);
        changed = true;
    }
    if (changed)
        PlayerbotRepository::instance().Save(ai);
    return changed;
}

void BotDriver::SetFocusHeal(Player* bot, std::list<ObjectGuid> const& targets)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return;
    ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("focus heal targets")->Set(targets);
    bool const has = ai->HasStrategy("focus heal targets", BOT_STATE_COMBAT);
    if (targets.empty() && has)
        ai->ChangeStrategy("-focus heal targets", BOT_STATE_COMBAT);
    else if (!targets.empty() && !has)
        ai->ChangeStrategy("+focus heal targets", BOT_STATE_COMBAT);
}

std::string BotDriver::SyncStrategies(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return "";

    auto combat = [&]()
    {
        std::vector<std::string> names = ai->GetStrategies(BOT_STATE_COMBAT);
        // A fight profile's strategy stands for rotation strategies it took out (FightProfiles.h): they are what the
        // talents' rotation is compared with, and the profile goes in again after the reset.
        if (auto ours = std::find(names.begin(), names.end(), "ga rotation"); ours != names.end())
        {
            names.erase(ours);
            for (std::string const& name : FightProfiles::StandStrategies(bot, 0))
                if (std::find(names.begin(), names.end(), name) == names.end())
                    names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        return names;
    };
    std::vector<std::string> const before = combat();
    // "avoid aoe" is the module's own (KeepAvoidAoe), not a part of the talents' rotation: the reset takes it out and
    // the planner put it back a few seconds later, and every login logged "combat strategies avoid aoe -> nothing"
    // (634 such lines on the integration server, for every class; read as "no combat strategy left" for the death
    // knights on 2026-11-14). It stays through the reset.
    bool const avoidCombat = ai->HasStrategy("avoid aoe", BOT_STATE_COMBAT);
    bool const avoidCalm = ai->HasStrategy("avoid aoe", BOT_STATE_NON_COMBAT);
    ai->ResetStrategies();
    if (avoidCombat && !ai->HasStrategy("avoid aoe", BOT_STATE_COMBAT))
        ai->ChangeStrategy("+avoid aoe", BOT_STATE_COMBAT);
    if (avoidCalm && !ai->HasStrategy("avoid aoe", BOT_STATE_NON_COMBAT))
        ai->ChangeStrategy("+avoid aoe", BOT_STATE_NON_COMBAT);
    ApplySoloPet(ai, bot);
    ApplySoloTotems(ai, bot);
    std::vector<std::string> const after = combat();
    if (before == after)
        return "";

    PlayerbotRepository::instance().Save(ai);
    auto only = [](std::vector<std::string> const& in, std::vector<std::string> const& notIn)
    {
        std::string text;
        for (std::string const& name : in)
            if (!std::binary_search(notIn.begin(), notIn.end(), name))
                text += (text.empty() ? "" : ", ") + name;
        return text.empty() ? std::string("nothing") : text;
    };
    return only(before, after) + " -> " + only(after, before);
}

void BotDriver::HoldOffStuckTeleport(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return;
    // MoveFarTo teleports once GetMSTimeDiffToNow(stuckTs) reaches its stuckTime (90 s); a minute is well inside it,
    // and the planner ticks every second.
    constexpr uint32 HoldOffMs = MINUTE * IN_MILLISECONDS;
    NewRpgInfo& info = ai->rpgInfo;
    if (info.stuckTs && GetMSTimeDiffToNow(info.stuckTs) >= HoldOffMs)
        info.stuckTs = getMSTime();
}

uint32 BotDriver::GetDeathCount(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    Value<uint32>* count = ai ? ai->GetAiObjectContext()->GetValue<uint32>("death count") : nullptr;
    return count ? count->Get() : 0;
}

bool BotDriver::WantsItem(Player* bot, Item* item)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai || !item)
        return false;
    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return false;

    // Ammunition: what the ranged weapon it holds shoots and it can use stays with it (playerbots' own answer is
    // "no use" at exactly the stock it buys, which a sweep of the bags read as "not wanted"). How much it may carry
    // is Ammo::Trim's matter, which sells what is over its stock, the worst kind first (Ammunition.h).
    if (proto->Class == ITEM_CLASS_PROJECTILE)
        return Ammo::Fits(bot, proto);

    // playerbots' qualifier: "entry" or "entry,randomPropertyId" (ItemUsageValue::GetItemIdFromQualifier).
    std::string qualifier = std::to_string(proto->ItemId);
    if (int32 const randomProperty = item->GetItemRandomPropertyId())
        qualifier += "," + std::to_string(randomProperty);
    Value<ItemUsage>* usage = ai->GetAiObjectContext()->GetValue<ItemUsage>("item usage", qualifier);
    if (!usage)
        return false;
    switch (usage->Get())
    {
        case ITEM_USAGE_EQUIP:
        case ITEM_USAGE_REPLACE:
            // A second one of a kind it may wear once is no upgrade: it goes the loot filter's way (WearsUniqueAlready).
            // Nor is a piece for the off hand under its two-hand weapon (OffHandUnderTwoHand).
            // Nor a caster's weapon worth less than the hands it would take (CasterHandsNoBetter).
            return item->IsEquipped() || (!WearsUniqueAlready(bot, proto) && !OffHandUnderTwoHand(bot, proto) &&
                !CasterHandsNoBetter(bot, proto, item->GetItemRandomPropertyId()));
        case ITEM_USAGE_QUEST:
        {
            // Not for a quest the planner leaves alone: nothing works on it (a hunter kept the Arcane Crystals it
            // gathered for the attunement to Naxxramas, round 179). Any other quest in the log that asks for it keeps it.
            bool leftAloneOnly = false;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 const questId = bot->GetQuestSlotQuestId(slot);
                Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
                if (!quest)
                    continue;
                bool asks = false;
                for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    if (quest->RequiredItemId[i] == proto->ItemId)
                        asks = true;
                if (!asks)
                    continue;
                if (!sQuestSupport.IsLeftAlone(bot, questId))
                    return true;
                leftAloneOnly = true;
            }
            return !leftAloneOnly;
        }
        case ITEM_USAGE_USE:
        case ITEM_USAGE_AMMO:
            return true;
        case ITEM_USAGE_KEEP:
            // Food and drink to three stacks, class reagents; not trade goods kept for a profession: those reach the
            // bot's crafting from the guild storage as well (CP-7).
            return proto->Class == ITEM_CLASS_CONSUMABLE || proto->Class == ITEM_CLASS_REAGENT;
        default:
            // A necklace, ring or trinket for a bare place (Gear::FillEmptySlots puts it on).
            return FillsEmptyPlace(bot, proto);
    }
}

bool BotDriver::IsJewellery(ItemTemplate const* proto)
{
    if (!proto || proto->Class != ITEM_CLASS_ARMOR)
        return false;
    switch (proto->InventoryType)
    {
        case INVTYPE_NECK:
        case INVTYPE_FINGER:
        case INVTYPE_TRINKET:
        case INVTYPE_HOLDABLE:
        case INVTYPE_RELIC:
            return true;
        default:
            return false;
    }
}

bool BotDriver::FillsEmptyPlace(Player* bot, ItemTemplate const* proto)
{
    // mod-playerbots answers "bad equip" for every such piece and a bare place (RandomItemMgr::CanEquipArmor: the
    // armour kind of the class, cloaks excepted; a necklace's kind is "miscellaneous"). On the integration server
    // none of 30 guild bots wore a necklace or a trinket and three wore a ring, with 28 Malachite Pendants, six
    // rings and nine off-hand pieces in their guild's storage.
    if (!bot || !IsJewellery(proto) || proto->RequiredLevel > bot->GetLevel() || bot->CanUseItem(proto) != EQUIP_ERR_OK)
        return false;
    // Something to hold in the off hand is for those who cast with it; another class's free hand waits for a shield
    // or a second weapon.
    if (proto->InventoryType == INVTYPE_HOLDABLE)
        switch (bot->getClass())
        {
            case CLASS_MAGE:
            case CLASS_PRIEST:
            case CLASS_WARLOCK:
            case CLASS_DRUID:
                break;
            default:
                return false;
        }
    // Not a second one of a kind it may wear once.
    if (WearsUniqueAlready(bot, proto))
        return false;
    // A bare place for it (not the off hand under a two-handed weapon).
    return bot->FindEquipSlot(proto, NULL_SLOT, false) != NULL_SLOT;
}

bool BotDriver::OffHandUnderTwoHand(Player const* bot, ItemTemplate const* proto)
{
    if (!bot || !proto)
        return false;
    if (proto->InventoryType != INVTYPE_HOLDABLE && proto->InventoryType != INVTYPE_SHIELD && proto->InventoryType != INVTYPE_WEAPONOFFHAND)
        return false;
    return bot->IsTwoHandUsed();
}

namespace
{
    bool IsCasterForHands(Player* bot)
    {
        switch (bot->getClass())
        {
            case CLASS_MAGE:
            case CLASS_PRIEST:
            case CLASS_WARLOCK:
                return true;
            case CLASS_DRUID:
                return AiFactory::GetPlayerSpecTab(bot) != DRUID_TAB_FERAL;
            case CLASS_SHAMAN:
                return !bot->CanDualWield();
            default:
                return false;
        }
    }

    bool IsDoubleHand(ItemTemplate const* proto)
    {
        return proto->Class == ITEM_CLASS_WEAPON && !(ITEM_SUBCLASS_MASK_SINGLE_HAND & (1 << proto->SubClass)) &&
            !(ITEM_SUBCLASS_MASK_WEAPON_RANGED & (1 << proto->SubClass));
    }

    // What a piece's stats are worth to the caster, without mod-playerbots' factor for its weapon type
    // (StatsWeightCalculator::CalculateItemTypePenalty: a two-hand weapon half, a caster's one-hand weapon 65 %).
    float CasterHandsValue(StatsWeightCalculator& calculator, ItemTemplate const* proto, int32 randomProperty)
    {
        float const score = calculator.CalculateItem(proto->ItemId, randomProperty);
        if (proto->Class != ITEM_CLASS_WEAPON)
            return score;
        return IsDoubleHand(proto) ? score / 0.5f : score / 0.65f;
    }
}

bool BotDriver::CasterHandsNoBetter(Player* bot, ItemTemplate const* proto, int32 randomProperty)
{
    if (!bot || !proto || proto->Class != ITEM_CLASS_WEAPON || !IsCasterForHands(bot))
        return false;
    bool const twoHand = proto->InventoryType == INVTYPE_2HWEAPON;
    bool const mainHand = proto->InventoryType == INVTYPE_WEAPON || proto->InventoryType == INVTYPE_WEAPONMAINHAND;
    if (!twoHand && !mainHand)
        return false;
    Item const* mh = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    Item const* oh = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    bool const wearsTwoHand = mh && mh->GetTemplate()->InventoryType == INVTYPE_2HWEAPON;
    // A one-hander beside a one-hander: mod-playerbots compares like with like.
    if (mainHand && !wearsTwoHand)
        return false;
    StatsWeightCalculator calculator(bot);
    float const candidate = CasterHandsValue(calculator, proto, randomProperty);
    float worn = 0.0f;
    if (mh)
        worn += CasterHandsValue(calculator, mh->GetTemplate(), mh->GetItemRandomPropertyId());
    if (twoHand && oh && !wearsTwoHand)
        worn += CasterHandsValue(calculator, oh->GetTemplate(), oh->GetItemRandomPropertyId());
    return candidate <= worn;
}

bool BotDriver::WearsUniqueAlready(Player const* bot, ItemTemplate const* proto, uint8 exceptSlot)
{
    if (!bot || !proto)
        return false;
    // The core's own rule for putting it on: the unique-equipped flag and the limit category (gems included).
    if (bot->CanEquipUniqueItem(proto, exceptSlot) != EQUIP_ERR_OK)
        return true;
    if (!proto->MaxCount)
        return false;
    uint32 worn = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            slot != exceptSlot && item && item->GetEntry() == proto->ItemId)
            worn += item->GetCount();
    return worn >= uint32(proto->MaxCount);
}

bool BotDriver::IsUpgrade(Player* bot, uint32 itemEntry, int32 randomProperty)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai || !itemEntry)
        return false;
    // Not a second ring, trinket or weapon of a kind it may wear only once (mod-playerbots says "replace" for the
    // other finger: WearsUniqueAlready).
    if (WearsUniqueAlready(bot, sObjectMgr->GetItemTemplate(itemEntry)))
        return false;
    // Nor a piece for the off hand under its two-hand weapon: the staff would have to go (OffHandUnderTwoHand).
    if (OffHandUnderTwoHand(bot, sObjectMgr->GetItemTemplate(itemEntry)))
        return false;
    // Nor a caster's weapon worth less than the hands it would take (CasterHandsNoBetter).
    if (CasterHandsNoBetter(bot, sObjectMgr->GetItemTemplate(itemEntry), randomProperty))
        return false;
    std::string qualifier = std::to_string(itemEntry);
    if (randomProperty)
        qualifier += "," + std::to_string(randomProperty);
    Value<ItemUsage>* usage = ai->GetAiObjectContext()->GetValue<ItemUsage>("item usage", qualifier);
    if (!usage)
        return false;
    ItemUsage const result = usage->Get();
    if (result == ITEM_USAGE_EQUIP || result == ITEM_USAGE_REPLACE)
        return true;
    // Jewellery for a bare place, whatever its stats are worth to the class (the owner: a bot takes the best it can
    // use; a bare place takes anything usable).
    return FillsEmptyPlace(bot, sObjectMgr->GetItemTemplate(itemEntry));
}

float BotDriver::ItemScore(Player* bot, uint32 itemEntry, int32 randomProperty)
{
    if (!GetAI(bot) || !itemEntry)
        return 0.0f;
    StatsWeightCalculator calculator(bot);
    return calculator.CalculateItem(itemEntry, randomProperty);
}

void BotDriver::ItemScores(Player* bot, std::vector<std::pair<uint32, int32>> const& items, std::vector<float>& scores)
{
    scores.assign(items.size(), 0.0f);
    if (!GetAI(bot) || items.empty())
        return;
    StatsWeightCalculator calculator(bot);
    for (std::size_t i = 0; i < items.size(); ++i)
        if (items[i].first && sObjectMgr->GetItemTemplate(items[i].first))
            scores[i] = calculator.CalculateItem(items[i].first, items[i].second);
}

uint8 BotDriver::SpecTab(Player* bot)
{
    return bot ? AiFactory::GetPlayerSpecTab(bot) : 0;
}

namespace
{
    // The stat weights of the wowbot335 profiles (ga_stat_weight): key class, tab, variant, phase.
    struct StatProfile
    {
        float weight[STATS_TYPE_MAX] = {};
        bool has[STATS_TYPE_MAX] = {};
    };
    std::unordered_map<uint32, StatProfile> statProfiles; // written at startup only (LoadStatWeights)
    std::atomic<bool> statWeightsOn{ false };
    std::shared_mutex roleModeLock;
    std::unordered_map<ObjectGuid::LowType, uint8> roleModes;
    // From this level on the preraid profile counts (the owner, 2026-11-26: leveling below 60, preraid at 60).
    constexpr uint8 PreraidLevel = 60;

    uint32 StatProfileKey(uint8 cls, uint8 tab, uint8 variant, uint8 phase)
    {
        return (uint32(cls) << 24) | (uint32(tab) << 16) | (uint32(variant) << 8) | uint32(phase);
    }

    int32 StatTypeByName(std::string const& name)
    {
        static std::unordered_map<std::string, int32> const names = {
            { "AGILITY", STATS_TYPE_AGILITY }, { "STRENGTH", STATS_TYPE_STRENGTH }, { "INTELLECT", STATS_TYPE_INTELLECT },
            { "SPIRIT", STATS_TYPE_SPIRIT }, { "STAMINA", STATS_TYPE_STAMINA }, { "HIT", STATS_TYPE_HIT },
            { "CRIT", STATS_TYPE_CRIT }, { "HASTE", STATS_TYPE_HASTE }, { "DEFENSE", STATS_TYPE_DEFENSE },
            { "DODGE", STATS_TYPE_DODGE }, { "PARRY", STATS_TYPE_PARRY }, { "BLOCK_VALUE", STATS_TYPE_BLOCK_VALUE },
            { "BLOCK_RATING", STATS_TYPE_BLOCK_RATING }, { "SPELL_POWER", STATS_TYPE_SPELL_POWER },
            { "HEAL_POWER", STATS_TYPE_HEAL_POWER }, { "MANA_REGENERATION", STATS_TYPE_MANA_REGENERATION },
            { "ATTACK_POWER", STATS_TYPE_ATTACK_POWER }, { "ARMOR_PENETRATION", STATS_TYPE_ARMOR_PENETRATION },
            { "EXPERTISE", STATS_TYPE_EXPERTISE } };
        auto itr = names.find(name);
        return itr == names.end() ? -1 : itr->second;
    }

    // The stats a profile speaks for: one it lists gets its weight; one it does not list is 0, unless mod-playerbots
    // gave it a negative weight (spell power for a warrior, attack power for a priest: what the spec must not wear).
    // Weapon damage, armour, threat, resistance and the bonus for unknown stats stay mod-playerbots' (wowbot335 has no
    // weapon damage term, and its armour weight is on another scale).
    constexpr StatsType ProfileStats[] = { STATS_TYPE_AGILITY, STATS_TYPE_STRENGTH, STATS_TYPE_INTELLECT, STATS_TYPE_SPIRIT,
        STATS_TYPE_STAMINA, STATS_TYPE_HIT, STATS_TYPE_CRIT, STATS_TYPE_HASTE, STATS_TYPE_DEFENSE, STATS_TYPE_DODGE,
        STATS_TYPE_PARRY, STATS_TYPE_BLOCK_VALUE, STATS_TYPE_BLOCK_RATING, STATS_TYPE_SPELL_POWER, STATS_TYPE_HEAL_POWER,
        STATS_TYPE_MANA_REGENERATION, STATS_TYPE_ATTACK_POWER, STATS_TYPE_ARMOR_PENETRATION, STATS_TYPE_EXPERTISE };

    bool ApplyStatProfile(uint8 cls, uint8 tab, uint8 variant, uint8 level, float* weights)
    {
        if (!statWeightsOn.load(std::memory_order_relaxed))
            return false;
        uint8 const phase = level >= PreraidLevel ? 1 : 0;
        auto itr = statProfiles.find(StatProfileKey(cls, tab, variant, phase));
        if (itr == statProfiles.end() && variant)
            itr = statProfiles.find(StatProfileKey(cls, tab, 0, phase));
        if (itr == statProfiles.end())
            return false;
        for (StatsType stat : ProfileStats)
        {
            if (itr->second.has[stat])
                weights[stat] = itr->second.weight[stat];
            else if (weights[stat] > 0.0f)
                weights[stat] = 0.0f;
        }
        return true;
    }

    // mod-playerbots' stat weights hook (patch 0002): after its basic weights, before the talent extras. Any thread.
    [[maybe_unused]] void FillGuildStatWeights(Player* player, float* weights)
    {
        if (!player || !weights)
            return;
        uint8 variant = 0;
        {
            std::shared_lock<std::shared_mutex> lock(roleModeLock);
            auto itr = roleModes.find(player->GetGUID().GetCounter());
            variant = itr != roleModes.end() && itr->second == 1 ? 1 : 0;
        }
        ApplyStatProfile(player->getClass(), AiFactory::GetPlayerSpecTab(player), variant, player->GetLevel(), weights);
    }

    // The tree and variant of a dungeon role's profile (role 1 tank, 2 healer): RoleScore, EnchantScore, BuffScore.
    bool RoleProfileOf(uint8 cls, uint8 role, uint8& tab, uint8& variant)
    {
        variant = 0;
        if (role == 1)
            switch (cls)
            {
                case CLASS_WARRIOR: tab = 2; return true;
                case CLASS_PALADIN: tab = 1; return true;
                case CLASS_DEATH_KNIGHT: tab = 0; variant = 1; return true;
                case CLASS_DRUID: tab = 1; variant = 1; return true;
                default: return false;
            }
        if (role == 2)
            switch (cls)
            {
                case CLASS_PALADIN: tab = 0; return true;
                case CLASS_PRIEST: tab = 1; return true;
                case CLASS_SHAMAN: tab = 2; return true;
                case CLASS_DRUID: tab = 2; return true;
                default: return false;
            }
        return false;
    }
}

void BotDriver::LoadStatWeights()
{
    statProfiles.clear();
    uint32 rows = 0;
    if (QueryResult result = WorldDatabase.Query("SELECT class, tab, variant, phase, stat, weight FROM ga_stat_weight"))
    {
        do
        {
            Field* fields = result->Fetch();
            int32 const stat = StatTypeByName(fields[4].Get<std::string>());
            if (stat < 0)
            {
                LOG_ERROR("module", "mod-guild-adventure: ga_stat_weight: unknown stat {}", fields[4].Get<std::string>());
                continue;
            }
            StatProfile& profile = statProfiles[StatProfileKey(fields[0].Get<uint8>(), fields[1].Get<uint8>(), fields[2].Get<uint8>(),
                fields[3].Get<uint8>())];
            profile.weight[stat] = fields[5].Get<float>();
            profile.has[stat] = true;
            ++rows;
        } while (result->NextRow());
    }
    bool const on = GuildAdventure::GetSettings().statWeightsEnable && !statProfiles.empty();
#ifdef PLAYERBOTS_STAT_WEIGHTS_HOOK
    StatsWeightCalculator::SetBasicWeightsHook(on ? &FillGuildStatWeights : nullptr);
    statWeightsOn = on;
    LOG_INFO("module", "mod-guild-adventure: stat weights: {} profiles ({} weights) from the wowbot335 research{}", statProfiles.size(),
        rows, on ? "" : " (off: mod-playerbots' own weights)");
#else
    statWeightsOn = false;
    LOG_WARN("module", "mod-guild-adventure: stat weights: mod-playerbots has no stat weights hook (patch 0002: "
        "tools/apply-playerbots-patches.ps1): its own weights stay ({} profiles loaded, not used)", statProfiles.size());
#endif
}

void BotDriver::SetRoleMode(ObjectGuid bot, uint8 role)
{
    std::unique_lock<std::shared_mutex> lock(roleModeLock);
    if (role)
        roleModes[bot.GetCounter()] = role;
    else
        roleModes.erase(bot.GetCounter());
}

namespace
{
    // playerbots' stat weights of the tank and healer trees (StatsWeightCalculator::GenerateBasicWeights). role 1 tank,
    // 2 healer; weights holds STATS_TYPE_MAX zeros.
    void FillPlayerbotsRoleWeights(uint8 cls, uint8 role, float* weights)
    {
        weights[STATS_TYPE_STAMINA] = 0.1f;
        weights[STATS_TYPE_ARMOR] = 0.001f;
        weights[STATS_TYPE_BONUS] = 1.0f;
        weights[STATS_TYPE_MELEE_DPS] = 0.01f;
        weights[STATS_TYPE_RANGED_DPS] = 0.01f;
        if (role == 2)
        {
            bool const plateOrMail = cls == CLASS_PALADIN || cls == CLASS_SHAMAN;
            weights[STATS_TYPE_INTELLECT] += plateOrMail ? 0.9f : 0.8f;
            weights[STATS_TYPE_SPIRIT] += plateOrMail ? 0.15f : 0.6f;
            weights[STATS_TYPE_HEAL_POWER] += 1.0f;
            weights[STATS_TYPE_MANA_REGENERATION] += 0.9f;
            weights[STATS_TYPE_CRIT] += 0.6f;
            weights[STATS_TYPE_HASTE] += 0.8f;
            // (A healer in a dungeon is hit now and then: stamina counts a little more than for a healer that levels.)
            weights[STATS_TYPE_STAMINA] += 0.2f;
            if (!plateOrMail)
                weights[STATS_TYPE_ATTACK_POWER] -= 1.0f;
        }
        else if (cls == CLASS_DRUID)
        {
            weights[STATS_TYPE_AGILITY] += 2.2f;
            weights[STATS_TYPE_STRENGTH] += 2.4f;
            weights[STATS_TYPE_STAMINA] += 4.0f;
            weights[STATS_TYPE_ATTACK_POWER] += 1.0f;
            weights[STATS_TYPE_DEFENSE] += 0.3f;
            weights[STATS_TYPE_DODGE] += 0.7f;
            weights[STATS_TYPE_ARMOR] += 0.15f;
            weights[STATS_TYPE_HIT] += 3.0f;
            weights[STATS_TYPE_CRIT] += 1.3f;
            weights[STATS_TYPE_HASTE] += 2.3f;
            weights[STATS_TYPE_EXPERTISE] += 3.7f;
        }
        else
        {
            weights[STATS_TYPE_AGILITY] += 0.2f;
            weights[STATS_TYPE_STRENGTH] += 1.3f;
            weights[STATS_TYPE_STAMINA] += 3.0f;
            weights[STATS_TYPE_ATTACK_POWER] += 0.2f;
            weights[STATS_TYPE_DEFENSE] += 2.5f;
            weights[STATS_TYPE_PARRY] += 2.0f;
            weights[STATS_TYPE_DODGE] += 2.0f;
            weights[STATS_TYPE_ARMOR] += 0.15f;
            weights[STATS_TYPE_HIT] += 2.0f;
            weights[STATS_TYPE_EXPERTISE] += 3.0f;
            weights[STATS_TYPE_MELEE_DPS] += 2.0f;
            if (cls == CLASS_DEATH_KNIGHT)
            {
                weights[STATS_TYPE_BLOCK_RATING] -= 2.0f;
                weights[STATS_TYPE_BLOCK_VALUE] -= 2.0f;
                weights[STATS_TYPE_SPELL_POWER] -= 1.0f;
            }
            else
            {
                weights[STATS_TYPE_BLOCK_RATING] += 1.0f;
                weights[STATS_TYPE_BLOCK_VALUE] += 0.5f;
                weights[STATS_TYPE_SPELL_POWER] -= 2.0f;
            }
        }
    }
}

namespace
{
    // The weights of a role (1 tank, 2 healer): mod-playerbots' tank or healer tree, then the role's wowbot335 profile in
    // their place (ApplyStatProfile).
    void FillRoleWeights(uint8 cls, uint8 role, uint8 level, float* weights)
    {
        FillPlayerbotsRoleWeights(cls, role, weights);
        uint8 tab = 0;
        uint8 variant = 0;
        if (RoleProfileOf(cls, role, tab, variant))
            ApplyStatProfile(cls, tab, variant, level, weights);
    }
}

float BotDriver::RoleScore(Player const* bot, uint8 role, uint32 itemEntry, int32 randomProperty)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
    if (!bot || !proto || (role != 1 && role != 2))
        return 0.0f;
    uint8 const cls = bot->getClass();

    // The item's stats as playerbots reads them for a tank or a healer (what its on-use and proc effects count for).
    StatsCollector collector(role == 2 ? CollectorType::SPELL_HEAL : CollectorType::MELEE_TANK, cls, bot->GetLevel());
    collector.CollectItemStats(proto);
    if (randomProperty > 0)
    {
        if (ItemRandomPropertiesEntry const* random = sItemRandomPropertiesStore.LookupEntry(randomProperty))
            for (uint32 i = PROP_ENCHANTMENT_SLOT_0; i < MAX_ENCHANTMENT_SLOT; ++i)
                if (SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(random->Enchantment[i - PROP_ENCHANTMENT_SLOT_0]))
                    collector.CollectEnchantStats(enchant);
    }
    else if (randomProperty < 0)
    {
        if (ItemRandomSuffixEntry const* suffix = sItemRandomSuffixStore.LookupEntry(-randomProperty))
            for (uint32 k = 0; k < MAX_ITEM_ENCHANTMENT_EFFECTS; ++k)
                if (SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(suffix->Enchantment[k]))
                    collector.CollectEnchantStats(enchant, uint32((suffix->AllocationPct[k] * GenerateEnchSuffixFactor(itemEntry)) / 10000));
    }

    float weights[STATS_TYPE_MAX] = {};
    FillRoleWeights(cls, role, bot->GetLevel(), weights);

    float score = 0.0f;
    for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
        score += weights[i] * collector.stats[i];

    // The hands of the role: a warrior or paladin tank holds a one-hand weapon and a shield, a holy paladin too; a
    // death knight tank a two-hand weapon.
    if (proto->Class == ITEM_CLASS_WEAPON)
    {
        bool const twoHand = proto->InventoryType == INVTYPE_2HWEAPON;
        if (twoHand && ((role == 1 && (cls == CLASS_WARRIOR || cls == CLASS_PALADIN)) || (role == 2 && cls == CLASS_PALADIN)))
            score *= 0.05f;
        else if (!twoHand && role == 1 && cls == CLASS_DEATH_KNIGHT &&
            (proto->InventoryType == INVTYPE_WEAPON || proto->InventoryType == INVTYPE_WEAPONMAINHAND))
            score *= 0.1f;
    }
    return std::max(score, 0.0f);
}

float BotDriver::EnchantScore(Player* bot, int8 role, uint32 enchantId, Item const* item)
{
    SpellItemEnchantmentEntry const* enchant = enchantId ? sSpellItemEnchantmentStore.LookupEntry(enchantId) : nullptr;
    if (!bot || !enchant)
        return 0.0f;
    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
    bool const weapon = proto && proto->Class == ITEM_CLASS_WEAPON;
    bool const ranged = weapon && (proto->InventoryType == INVTYPE_RANGED || proto->InventoryType == INVTYPE_RANGEDRIGHT ||
        proto->InventoryType == INVTYPE_THROWN);
    uint32 const delay = weapon ? proto->Delay : 0;
    if (role == 1 || role == 2)
    {
        uint8 const cls = bot->getClass();
        StatsCollector collector(role == 2 ? CollectorType::SPELL_HEAL : CollectorType::MELEE_TANK, cls, bot->GetLevel());
        collector.CollectEnchantStats(enchant, 0, delay, ranged);
        float weights[STATS_TYPE_MAX] = {};
        FillRoleWeights(cls, uint8(role), bot->GetLevel(), weights);
        float score = 0.0f;
        for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
            score += weights[i] * collector.stats[i];
        return std::max(score, 0.0f);
    }
    if (!GetAI(bot))
        return 0.0f;
    StatsWeightCalculator calculator(bot);
    return std::max(calculator.CalculateEnchant(enchantId, delay, ranged), 0.0f);
}

float BotDriver::BuffScore(Player* bot, int8 role, uint32 itemEntry, SpellInfo const* buff)
{
    if (!bot || !buff)
        return 0.0f;
    uint8 const cls = bot->getClass();
    float weights[STATS_TYPE_MAX] = {};
    bool melee = false;
    bool ranged = false;
    if (role == 1 || role == 2)
    {
        FillRoleWeights(cls, uint8(role), bot->GetLevel(), weights);
        melee = role == 1;
    }
    else
    {
        if (!GetAI(bot) || !sObjectMgr->GetItemTemplate(itemEntry))
            return 0.0f;
        // (The calculator makes its weights when it rates an item.)
        StatsWeightCalculator calculator(bot);
        calculator.CalculateItem(itemEntry, 0);
        std::copy_n(calculator.GetStatWeights(), size_t(STATS_TYPE_MAX), weights);
        ranged = cls == CLASS_HUNTER;
        melee = !ranged && FightsInMelee(bot);
    }

    // What the buff gives, as playerbots' collector files the stats of gear.
    float stats[STATS_TYPE_MAX] = {};
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effect = buff->Effects[i];
        if (effect.Effect != SPELL_EFFECT_APPLY_AURA)
            continue;
        float const value = float(effect.CalcValue(bot));
        int32 const misc = effect.MiscValue;
        switch (effect.ApplyAuraName)
        {
            case SPELL_AURA_MOD_STAT:
                if (misc == STAT_STRENGTH || misc == -1)
                    stats[STATS_TYPE_STRENGTH] += value;
                if (misc == STAT_AGILITY || misc == -1)
                    stats[STATS_TYPE_AGILITY] += value;
                if (misc == STAT_STAMINA || misc == -1)
                    stats[STATS_TYPE_STAMINA] += value;
                if (misc == STAT_INTELLECT || misc == -1)
                    stats[STATS_TYPE_INTELLECT] += value;
                if (misc == STAT_SPIRIT || misc == -1)
                    stats[STATS_TYPE_SPIRIT] += value;
                break;
            case SPELL_AURA_MOD_ATTACK_POWER:
                if (melee)
                    stats[STATS_TYPE_ATTACK_POWER] += value;
                break;
            case SPELL_AURA_MOD_RANGED_ATTACK_POWER:
                if (ranged)
                    stats[STATS_TYPE_ATTACK_POWER] += value;
                break;
            case SPELL_AURA_MOD_DAMAGE_DONE:
                if (misc & SPELL_SCHOOL_MASK_NORMAL)
                {
                    if (melee || ranged)
                        stats[STATS_TYPE_ATTACK_POWER] += value;
                }
                // (One school of magic only, firepower or shadow power: half, the bot's school is not looked at.)
                else if (misc & SPELL_SCHOOL_MASK_MAGIC)
                    stats[STATS_TYPE_SPELL_POWER] += (misc & SPELL_SCHOOL_MASK_MAGIC) == SPELL_SCHOOL_MASK_MAGIC ? value : value * 0.5f;
                break;
            case SPELL_AURA_MOD_HEALING_DONE:
                stats[STATS_TYPE_HEAL_POWER] += value;
                break;
            case SPELL_AURA_MOD_RESISTANCE:
                if (misc & SPELL_SCHOOL_MASK_NORMAL)
                    stats[STATS_TYPE_ARMOR] += value;
                else
                    stats[STATS_TYPE_RESISTANCE] += value;
                break;
            case SPELL_AURA_MOD_INCREASE_HEALTH:
            case SPELL_AURA_MOD_INCREASE_HEALTH_2:
                stats[STATS_TYPE_STAMINA] += value / 10.0f;
                break;
            case SPELL_AURA_MOD_POWER_REGEN:
                if (misc == POWER_MANA)
                    stats[STATS_TYPE_MANA_REGENERATION] += value;
                break;
            case SPELL_AURA_MOD_REGEN:
                stats[STATS_TYPE_HEALTH_REGENERATION] += value;
                break;
            case SPELL_AURA_MOD_SPELL_CRIT_CHANCE:
                // (A percent of crit chance: about 14 rating at level 60, 46 at level 80.)
                if (!melee && !ranged)
                    stats[STATS_TYPE_CRIT] += value * 14.0f;
                break;
            case SPELL_AURA_MOD_RATING:
                if (misc & ((1 << CR_CRIT_MELEE) | (1 << CR_CRIT_RANGED) | (1 << CR_CRIT_SPELL)))
                    stats[STATS_TYPE_CRIT] += value;
                if (misc & ((1 << CR_HIT_MELEE) | (1 << CR_HIT_RANGED) | (1 << CR_HIT_SPELL)))
                    stats[STATS_TYPE_HIT] += value;
                if (misc & ((1 << CR_HASTE_MELEE) | (1 << CR_HASTE_RANGED) | (1 << CR_HASTE_SPELL)))
                    stats[STATS_TYPE_HASTE] += value;
                if (misc & (1 << CR_DEFENSE_SKILL))
                    stats[STATS_TYPE_DEFENSE] += value;
                if (misc & (1 << CR_DODGE))
                    stats[STATS_TYPE_DODGE] += value;
                if (misc & (1 << CR_PARRY))
                    stats[STATS_TYPE_PARRY] += value;
                if (misc & (1 << CR_EXPERTISE))
                    stats[STATS_TYPE_EXPERTISE] += value;
                if (misc & (1 << CR_ARMOR_PENETRATION))
                    stats[STATS_TYPE_ARMOR_PENETRATION] += value;
                break;
            default:
                break;
        }
    }

    float score = 0.0f;
    for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
        score += weights[i] * stats[i];
    return std::max(score, 0.0f);
}

namespace
{
    // Not the creature the module just let go of beside the bot's way (Escape::CheckPull: nothing its work wants, a
    // bystander, a foe it cannot get to). mod-playerbots' grind target is the nearest creature within its aggro range
    // and ten yards while NewRpg walks, and "attack anything" took it again at every look of the AI; the module let it
    // go four times a second, and each pick stopped the walk: in an hour of the integration server, 2026-11-26, 67 walks
    // stood ten seconds before the planner's own steps took them, 25 of them seconds after such a let-go (the line is
    // written once per creature in ten minutes, so more of the others were). Kept for 30 s from the last let-go.
    class GuildAttackAnythingAction : public AttackAnythingAction
    {
    public:
        explicit GuildAttackAnythingAction(PlayerbotAI* botAI) : AttackAnythingAction(botAI) { }

        bool isUseful() override
        {
            if (Unit* target = GetTarget(); target && BotDriver::IsLetGo(bot, target))
                return false;
            return AttackAnythingAction::isUseful();
        }
    };

    // mod-playerbots' grind strategy has "drink" and "food" as its default actions, and they judge by numbers of
    // their own: drink below 85 % mana (a constant), food below Playerbots.LowHealth. So a bot on an objective drank
    // after nearly every fight whatever the guild leader had set (the owner: [quote left out]; his paladin took ten drinks in five minutes without one
    // rest of the module). The module's versions add the leader's thresholds; everything else is mod-playerbots'.
    class GuildDrinkAction : public DrinkAction
    {
    public:
        explicit GuildDrinkAction(PlayerbotAI* botAI) : DrinkAction(botAI) { }

        bool isUseful() override
        {
            uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
            if (!maxMana || float(bot->GetPower(POWER_MANA)) * 100.0f / float(maxMana) >= float(sBotSettings.For(bot).restBelowMana))
                return false;
            // Not in water it cannot sit down in: the drink fails there, and the action still waits out its ten to
            // twenty-seven seconds, time after time (the module's rest takes the bot to dry ground first).
            if (Escape::TooWetToRest(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()))
                return false;
            return DrinkAction::isUseful();
        }
    };

    class GuildEatAction : public EatAction
    {
    public:
        explicit GuildEatAction(PlayerbotAI* botAI) : EatAction(botAI) { }

        bool isUseful() override
        {
            if (bot->GetHealthPct() >= float(sBotSettings.For(bot).restBelowHealth))
                return false;
            if (Escape::TooWetToRest(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()))
                return false;
            return EatAction::isUseful();
        }
    };

    // Which of mod-playerbots' actions held a bot's AI longer than a move's wait (5.5 s): the AI state line names it, so
    // a walk that does not get on says what it waited for (Grushka, a warlock of the Horde test guild, stood 22 s after
    // won fights with "ai held for 11918 ms more", and nothing said by which action, 2026-11-27).
    struct HoldSetter
    {
        std::string action;
        uint32 ms = 0;
        uint32 at = 0;
    };
    std::mutex holdSetterLock;
    std::unordered_map<ObjectGuid, HoldSetter> holdSetters;
    std::unordered_map<ObjectGuid, PlayerbotAI*> holdWatched;
    constexpr uint32 HoldNoteMs = 5500;

    class HoldWatch : public ActionExecutionListener
    {
    public:
        HoldWatch(PlayerbotAI* ai, ObjectGuid guid) : _ai(ai), _guid(guid) { }
        bool Before(Action* /*action*/, Event /*event*/) override { return true; }
        bool AllowExecution(Action* /*action*/, Event /*event*/) override { return true; }
        bool OverrideResult(Action* /*action*/, bool executed, Event /*event*/) override { return executed; }
        void After(Action* action, bool /*executed*/, Event /*event*/) override
        {
            if (!action)
                return;
            uint32 const hold = std::max(NextCheckPeek::Of(_ai), action->GetDuration());
            if (hold <= HoldNoteMs)
                return;
            std::lock_guard<std::mutex> guard(holdSetterLock);
            holdSetters[_guid] = HoldSetter{ action->getName(), hold, std::max<uint32>(getMSTime(), 1) };
        }

    private:
        PlayerbotAI* _ai;
        ObjectGuid _guid;
    };

    // PlayerbotAI::engines is protected: read through a class derived from it. The engines live as long as the AI and
    // own their listeners (they delete them).
    struct EnginePeek : PlayerbotAI
    {
        static Engine* Of(PlayerbotAI* ai, int state) { return (ai->*(&EnginePeek::engines))[state]; }
    };

    // A hold no action left behind (a value, a packet, a teleport): seen from the planner's step when the AI's wait grew
    // since its last look by more than the time between, with what the bot was doing then.
    struct HoldSeen
    {
        uint32 delay = 0;
        uint32 at = 0;
    };
    std::unordered_map<ObjectGuid, HoldSeen> holdSeen;

    std::string HoldContext(Player* bot)
    {
        std::string text = "set outside an action";
        if (Spell const* spell = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL); spell && spell->GetSpellInfo())
            text += Acore::StringFormat(", channelling {} ({})", spell->GetSpellInfo()->SpellName[0], spell->GetSpellInfo()->Id);
        else if (Spell const* cast = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL); cast && cast->GetSpellInfo())
            text += Acore::StringFormat(", casting {} ({})", cast->GetSpellInfo()->SpellName[0], cast->GetSpellInfo()->Id);
        if (bot->IsBeingTeleportedFar() || bot->IsBeingTeleportedNear())
            text += ", being teleported";
        if (bot->IsSitState())
            text += ", sitting";
        if (bot->IsInFlight())
            text += ", on a flight";
        if (bot->GetTransport())
            text += ", on a transport";
        if (bot->IsMounted())
            text += ", mounted";
        if (bot->isMoving())
            text += ", moving";
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot); ai && !ai->AllowActivity(ALL_ACTIVITY))
            text += ", inactive for playerbots";
        return text;
    }

    void WatchHolds(PlayerbotAI* ai, ObjectGuid guid)
    {
        {
            uint32 const now = std::max<uint32>(getMSTime(), 1);
            uint32 const delay = NextCheckPeek::Of(ai);
            std::lock_guard<std::mutex> guard(holdSetterLock);
            HoldSeen& seen = holdSeen[guid];
            uint32 const passed = seen.at ? getMSTimeDiff(seen.at, now) : 0;
            uint32 const expected = seen.delay > passed ? seen.delay - passed : 0;
            if (delay > HoldNoteMs && delay > expected + 1500)
            {
                bool moduleHold = false;
                {
                    std::lock_guard<std::mutex> holdGuard(moduleHoldLock);
                    auto const found = moduleHolds.find(guid);
                    moduleHold = found != moduleHolds.end() && int32(found->second - now) > 0;
                }
                auto const known = holdSetters.find(guid);
                if (!moduleHold && (known == holdSetters.end() || getMSTimeDiff(known->second.at, now) > 2000))
                    holdSetters[guid] = HoldSetter{ HoldContext(ai->GetBot()), delay, now };
            }
            seen = HoldSeen{ delay, now };
            auto [it, fresh] = holdWatched.try_emplace(guid, ai);
            if (!fresh && it->second == ai)
                return;
            it->second = ai;
        }
        for (int state = 0; state < int(BOT_STATE_MAX); ++state)
            if (Engine* engine = EnginePeek::Of(ai, state))
                engine->AddActionExecutionListener(new HoldWatch(ai, guid));
    }

    // No Water Walking, on itself or on its party: mod-playerbots' shaman casts it whenever it swims, and a character
    // that walks on the water cannot dive. The module's swims go through the water and its tasks dive to their targets
    // on the floor, so it took the aura off again, and the shaman cast it anew for a Fish Oil of the guild storage each
    // time (44 that day for three shamans of the test guild on "Clam Bait" in Desolace, whose crawlers live 55 to 70 yd
    // down; Mojowind rose 20 yd in 90 s on a swim to the shore and was stuck, 2026-11-26).
    class GuildWaterWalkingAction : public CastWaterWalkingAction
    {
    public:
        explicit GuildWaterWalkingAction(PlayerbotAI* botAI) : CastWaterWalkingAction(botAI) { }
        bool isUseful() override { return false; }
    };

    class GuildWaterWalkingOnPartyAction : public CastWaterWalkingOnPartyAction
    {
    public:
        explicit GuildWaterWalkingOnPartyAction(PlayerbotAI* botAI) : CastWaterWalkingOnPartyAction(botAI) { }
        bool isUseful() override { return false; }
    };

    // A paladin's Consecration only against several foes (the owner, 2026-11-14: [quote left out]): mod-playerbots' strategies cast it in every fight, against a single creature too, whatever the
    // spec. Foes in a fight within its eight yards, at least Fight.ConsecrationMinFoes of them (2); a tank holding
    // several keeps it.
    class GuildConsecrationAction : public CastConsecrationAction
    {
    public:
        explicit GuildConsecrationAction(PlayerbotAI* botAI) : CastConsecrationAction(botAI) { }

        bool isUseful() override
        {
            uint32 const wanted = sBotSettings.For(bot).consecrationMinFoes;
            if (wanted > 1)
            {
                constexpr float ConsecrationYards = 8.0f;
                std::list<Unit*> units;
                Acore::AnyUnfriendlyUnitInObjectRangeCheck check(bot, bot, ConsecrationYards);
                Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(bot, units, check);
                Cell::VisitObjects(bot, searcher, ConsecrationYards);
                uint32 foes = 0;
                for (Unit* unit : units)
                    if (unit->IsAlive() && unit->IsInCombat() && bot->IsValidAttackTarget(unit) && !unit->IsTotem() && ++foes >= wanted)
                        break;
                if (foes < wanted)
                {
                    // (Under GuildAdventure.Debug, once per half minute: a test sees the rule at work.)
                    if (GetSettings().debug && getMSTimeDiff(_noteAt, getMSTime()) >= 30 * IN_MILLISECONDS && bot->IsInCombat())
                    {
                        _noteAt = getMSTime();
                        LOG_INFO("module.ga.activity", "activity: {} does not cast Consecration: {} foe(s) in a fight within 8 yd "
                            "(from {})", bot->GetName(), foes, wanted);
                    }
                    return false;
                }
            }
            // (Nor while a creature in no fight stands in its reach: neutral ones too, which fight what hits them.)
            if (Creature* bystander = Escape::BystanderInReach(bot, 9.0f))
            {
                if (getMSTimeDiff(_noteAt, getMSTime()) >= 30 * IN_MILLISECONDS && bot->IsInCombat())
                {
                    _noteAt = getMSTime();
                    LOG_INFO("module.ga.activity", "activity: {} does not cast Consecration: {} (level {}) stands {:.0f} yd away "
                        "and is in no fight; it would draw it in", bot->GetName(), bystander->GetName(), uint32(bystander->GetLevel()),
                        bot->GetExactDist(bystander));
                }
                return false;
            }
            return CastConsecrationAction::isUseful();
        }

    private:
        uint32 _noteAt = 0;
    };

    // A paladin's Seal of Command chains its swings to creatures beside the target: not put on while one in no fight
    // stands there (Escape::StepSealOfCommand takes it off); the next seal of mod-playerbots' list is another one.
    class GuildSealOfCommandAction : public CastSealOfCommandAction
    {
    public:
        explicit GuildSealOfCommandAction(PlayerbotAI* botAI) : CastSealOfCommandAction(botAI) { }
        bool isUseful() override
        {
            return !(bot->IsInCombat() && Escape::BystanderInReach(bot, 5.0f)) && CastSealOfCommandAction::isUseful();
        }
    };

    // mod-playerbots' "flee" (its ranged combat strategy: a character that fights at range and has its foe in melee
    // reach) backs off five yards straight away from the target, again and again while the foe follows, and looks at
    // nothing behind it: a mage's fight drifted 22 to 27 yards that way into three more Kurzen of their camp, 22 to 29
    // seconds into a fight it had pulled alone, and it died (Mageyo, 2026-11-14, round 108). The same steps at the
    // same angles, but each looked at for creatures that are not in the fight yet (Escape::StepWakes); with none open
    // the character fights on where it stands.
    class GuildFleeAction : public FleeAction
    {
    public:
        explicit GuildFleeAction(PlayerbotAI* botAI) : FleeAction(botAI) { }

        bool Execute(Event /*event*/) override
        {
            Unit* target = AI_VALUE(Unit*, "current target");
            if (!target)
                return false;
            float const distance = sPlayerbotAIConfig.FleeDistance;
            float const away = target->GetAngle(bot);
            Creature* blocker = nullptr;
            for (float delta = 0.0f; delta <= float(M_PI) / 2.0f + 0.01f; delta += float(M_PI) / 8.0f)
                for (float const sign : { 1.0f, -1.0f })
                {
                    if (delta == 0.0f && sign < 0.0f)
                        continue;
                    float const angle = away + sign * delta;
                    float x = bot->GetPositionX() + std::cos(angle) * distance;
                    float y = bot->GetPositionY() + std::sin(angle) * distance;
                    float z = bot->GetPositionZ();
                    bool const exact = bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(),
                        bot->GetPositionY(), bot->GetPositionZ(), x, y, z);
                    if (!exact)
                    {
                        x = bot->GetPositionX() + std::cos(angle) * distance;
                        y = bot->GetPositionY() + std::sin(angle) * distance;
                        z = bot->GetPositionZ();
                    }
                    Creature* wakes = nullptr;
                    if (Escape::StepWakes(bot, x, y, &wakes))
                    {
                        if (!blocker)
                            blocker = wakes;
                        continue;
                    }
                    if (MoveTo(target->GetMapId(), x, y, z, false, false, true, exact, MovementPriority::MOVEMENT_COMBAT,
                            false, true))
                        return true;
                }
            if (blocker && getMSTimeDiff(_noteAt, getMSTime()) >= 30 * IN_MILLISECONDS)
            {
                _noteAt = getMSTime();
                LOG_INFO("module.ga.activity", "activity: {} does not back away from {}: every way back leads toward {} "
                    "(level {}, {:.0f} yd away), which is not in the fight yet; it fights on where it stands",
                    bot->GetName(), target->GetName(), blocker->GetName(), blocker->GetLevel(), bot->GetExactDist(blocker));
            }
            return false;
        }

    private:
        uint32 _noteAt = 0;
    };

    // A mage's "blink back" turns its back on the target and blinks twenty yards: not into creatures that are not in
    // the fight yet.
    class GuildBlinkBackAction : public CastBlinkBackAction
    {
    public:
        explicit GuildBlinkBackAction(PlayerbotAI* botAI) : CastBlinkBackAction(botAI) { }

        bool isUseful() override
        {
            if (Unit* target = AI_VALUE(Unit*, "current target"))
            {
                float const angle = target->GetAngle(bot);
                for (float const yards : { 10.0f, 20.0f })
                    if (Escape::StepWakes(bot, bot->GetPositionX() + std::cos(angle) * yards,
                            bot->GetPositionY() + std::sin(angle) * yards))
                        return false;
            }
            return CastBlinkBackAction::isUseful();
        }
    };

    // A mage's area spells not where they would hit a creature far above it that is not in the fight yet
    // (Escape::StrongBystanderWithin). Twilight Marauder Morna, a level-60 elite of 3.45 times the health and 4.6 times
    // the damage on a long round through Silithus, is flagged a civilian: she attacks nothing by herself and helps
    // nobody, but she fights what hits her. A level-60 mage's fights drew her in twice in three hours ("3 foes at
    // once (Twilight Marauder Morna level 60)", once "it was attacked" on a walk), and it died of the second, round 192.
    enum class AreaAt : uint8 { Self, Front, Target };
    bool AreaHitsBystander(Player* bot, Unit* target, AreaAt at, float yards, char const* spell, uint32& noteAt)
    {
        float x = bot->GetPositionX();
        float y = bot->GetPositionY();
        if (at == AreaAt::Target)
        {
            if (!target)
                return false;
            x = target->GetPositionX();
            y = target->GetPositionY();
        }
        Creature* bystander = Escape::StrongBystanderWithin(bot, x, y, yards);
        // (A cone reaches what stands in front of the mage; a quarter circle to either side of its facing to spare.)
        if (bystander && at == AreaAt::Front && !bot->HasInArc(float(M_PI), bystander))
            bystander = nullptr;
        if (!bystander)
            return false;
        if (getMSTimeDiff(noteAt, getMSTime()) >= 30 * IN_MILLISECONDS)
        {
            noteAt = getMSTime();
            LOG_INFO("module.ga.activity", "activity: {} does not cast {}: {} (level {}{}) stands {:.0f} yd from where it would "
                "land and is not in the fight; the spell would draw it in", bot->GetName(), spell, bystander->GetName(),
                uint32(bystander->GetLevel()), bystander->isElite() ? " elite" : "", bystander->GetExactDist2d(x, y));
        }
        return true;
    }

    // A shaman's Fire Nova not while a creature that is in no fight stands within its 10 yd of the fire totem it bursts
    // from (12 yd: the creature's size): mod-playerbots' enhancement shaman casts it whenever its foe is in melee, and its
    // flames hit neutral creatures too. Rhazko (23, enhancement) fought one creature beside four kodos of the Barrens
    // (neutral, standing 7-18 yd from its target, no call for help between them); two seconds later all four were on it
    // and it died, round 243.
    class GuildFireNovaAction : public CastFireNovaAction
    {
    public:
        explicit GuildFireNovaAction(PlayerbotAI* botAI) : CastFireNovaAction(botAI) { }
        bool isUseful() override
        {
            Creature* totem = bot->GetMap()->GetCreature(bot->m_SummonSlot[SUMMON_SLOT_TOTEM_FIRE]);
            // Not against a single foe: mod-playerbots' enhancement shaman casts it whenever a foe is in its melee reach
            // (its aoe strategy's trigger "enemy within melee"), and each cast costs 22 % of its base mana. Thrakgar
            // (34) spent 60-70 % of its mana on one creature of its level, and on a Lashtail Raptor (36) all of it in 44 s;
            // it died with the raptor at 5 % and nothing left to heal itself with, round 256 (as Consecration, DQ19).
            {
                constexpr uint32 FireNovaMinFoes = 2;
                constexpr float FireNovaYards = 10.0f;
                WorldObject const* centre = totem ? static_cast<WorldObject const*>(totem) : static_cast<WorldObject const*>(bot);
                std::list<Unit*> units;
                Acore::AnyUnfriendlyUnitInObjectRangeCheck check(centre, bot, FireNovaYards + 2.0f);
                Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(centre, units, check);
                Cell::VisitObjects(centre, searcher, FireNovaYards + 2.0f);
                uint32 foes = 0;
                for (Unit* unit : units)
                    if (unit->IsAlive() && unit->IsInCombat() && bot->IsValidAttackTarget(unit) && !unit->IsTotem() &&
                        ++foes >= FireNovaMinFoes)
                        break;
                if (foes < FireNovaMinFoes)
                {
                    if (GetSettings().debug && getMSTimeDiff(_foesNoteAt, getMSTime()) >= 30 * IN_MILLISECONDS && bot->IsInCombat())
                    {
                        _foesNoteAt = getMSTime();
                        LOG_INFO("module.ga.activity", "activity: {} does not cast Fire Nova: {} foe(s) in a fight within 10 yd of "
                            "its {} (from {})", bot->GetName(), foes, totem ? "fire totem" : "place", FireNovaMinFoes);
                    }
                    return false;
                }
            }
            if (totem)
                if (Creature* bystander = Escape::BystanderWithin(bot, totem->GetPositionX(), totem->GetPositionY(), 12.0f))
                {
                    if (getMSTimeDiff(_noteAt, getMSTime()) >= 30 * IN_MILLISECONDS)
                    {
                        _noteAt = getMSTime();
                        LOG_INFO("module.ga.activity", "activity: {} does not cast Fire Nova: {} (level {}{}) stands {:.0f} yd from its "
                            "{} and is in no fight; the flames would draw it in", bot->GetName(), bystander->GetName(),
                            uint32(bystander->GetLevel()), bystander->IsHostileTo(bot) ? "" : ", neutral",
                            bystander->GetExactDist2d(totem), totem->GetName());
                    }
                    return false;
                }
            return CastFireNovaAction::isUseful();
        }

    private:
        uint32 _noteAt = 0;
        uint32 _foesNoteAt = 0;
    };

    // A shaman's Call of the Elements not in the open world: there it sets one Searing Totem (BotDriver::KeepTotems), but
    // mod-playerbots' enhancement strategy casts Call of the Elements at the start of every fight in melee, which sets
    // every totem of its bar (Strength of Earth, Healing Stream, Searing: 20 % of its base mana at once). On the j5
    // instance a level-32 shaman cast it 128 times and Fire Nova 338 times in about 1200 fights of two days, 1108 of them
    // against one creature (its mana fell from 30 % to 1 % in three seconds). In a dungeon or a battleground its
    // talents' totems stay.
    class GuildCallOfTheElementsAction : public CastCallOfTheElementsAction
    {
    public:
        explicit GuildCallOfTheElementsAction(PlayerbotAI* botAI) : CastCallOfTheElementsAction(botAI) { }
        bool isUseful() override
        {
            Map const* map = bot->FindMap();
            if (map && !map->IsDungeon() && !map->IsBattlegroundOrArena())
            {
                if (GetSettings().debug && getMSTimeDiff(_noteAt, getMSTime()) >= 30 * IN_MILLISECONDS && bot->IsInCombat())
                {
                    _noteAt = getMSTime();
                    LOG_INFO("module.ga.activity", "activity: {} does not cast Call of the Elements: in the open world it sets "
                        "one Searing Totem", bot->GetName());
                }
                return false;
            }
            return CastCallOfTheElementsAction::isUseful();
        }

    private:
        uint32 _noteAt = 0;
    };

    class GuildFrostNovaAction : public CastFrostNovaAction
    {
    public:
        explicit GuildFrostNovaAction(PlayerbotAI* botAI) : CastFrostNovaAction(botAI) { }
        bool isUseful() override
        {
            return !AreaHitsBystander(bot, nullptr, AreaAt::Self, 12.0f, "Frost Nova", _noteAt) && CastFrostNovaAction::isUseful();
        }

    private:
        uint32 _noteAt = 0;
    };

    class GuildConeOfColdAction : public CastConeOfColdAction
    {
    public:
        explicit GuildConeOfColdAction(PlayerbotAI* botAI) : CastConeOfColdAction(botAI) { }
        bool isUseful() override
        {
            return !AreaHitsBystander(bot, nullptr, AreaAt::Front, 12.0f, "Cone of Cold", _noteAt) && CastConeOfColdAction::isUseful();
        }

    private:
        uint32 _noteAt = 0;
    };

    class GuildBlizzardAction : public CastBlizzardAction
    {
    public:
        explicit GuildBlizzardAction(PlayerbotAI* botAI) : CastBlizzardAction(botAI) { }
        bool isUseful() override
        {
            return !AreaHitsBystander(bot, AI_VALUE(Unit*, "current target"), AreaAt::Target, 10.0f, "Blizzard", _noteAt) &&
                CastBlizzardAction::isUseful();
        }

    private:
        uint32 _noteAt = 0;
    };

    class GuildFlamestrikeAction : public CastFlamestrikeAction
    {
    public:
        explicit GuildFlamestrikeAction(PlayerbotAI* botAI) : CastFlamestrikeAction(botAI) { }
        bool isUseful() override
        {
            return !AreaHitsBystander(bot, AI_VALUE(Unit*, "current target"), AreaAt::Target, 10.0f, "Flamestrike", _noteAt) &&
                CastFlamestrikeAction::isUseful();
        }

    private:
        uint32 _noteAt = 0;
    };

    // A hunter's Multi-Shot (it jumps to creatures beside its target), Volley (an area at the target) and Explosive Trap
    // (an area at its feet) not while a creature in no fight stands in their reach: neutral ones too, which fight what
    // hits them, and a formation comes with its member. The rule of Fire Nova (round 243) and Seal of Command (round 242)
    // for a hunter's area shots; the test guild's hunters fight among Arathi's neutral ogres and the Barrens' kodos.
    bool HunterAreaHitsBystander(Player* bot, float x, float y, float yards, char const* spell, uint32& noteAt)
    {
        Creature* bystander = Escape::BystanderWithin(bot, x, y, yards);
        if (!bystander)
            return false;
        if (getMSTimeDiff(noteAt, getMSTime()) >= 30 * IN_MILLISECONDS)
        {
            noteAt = getMSTime();
            LOG_INFO("module.ga.activity", "activity: {} does not cast {}: {} (level {}{}) stands {:.0f} yd from where it would hit "
                "and is in no fight; it would draw it in", bot->GetName(), spell, bystander->GetName(), uint32(bystander->GetLevel()),
                bystander->IsHostileTo(bot) ? "" : ", neutral", bystander->GetExactDist2d(x, y));
        }
        return true;
    }

    class GuildMultiShotAction : public CastMultiShotAction
    {
    public:
        explicit GuildMultiShotAction(PlayerbotAI* botAI) : CastMultiShotAction(botAI) { }
        bool isUseful() override
        {
            Unit* target = AI_VALUE(Unit*, "current target");
            return !(target && HunterAreaHitsBystander(bot, target->GetPositionX(), target->GetPositionY(), 12.0f, "Multi-Shot",
                _noteAt)) && CastMultiShotAction::isUseful();
        }

    private:
        uint32 _noteAt = 0;
    };

    class GuildVolleyAction : public CastVolleyAction
    {
    public:
        explicit GuildVolleyAction(PlayerbotAI* botAI) : CastVolleyAction(botAI) { }
        bool isUseful() override
        {
            Unit* target = AI_VALUE(Unit*, "current target");
            return !(target && HunterAreaHitsBystander(bot, target->GetPositionX(), target->GetPositionY(), 12.0f, "Volley", _noteAt)) &&
                CastVolleyAction::isUseful();
        }

    private:
        uint32 _noteAt = 0;
    };

    class GuildExplosiveTrapAction : public CastExplosiveTrapAction
    {
    public:
        explicit GuildExplosiveTrapAction(PlayerbotAI* botAI) : CastExplosiveTrapAction(botAI) { }
        bool isUseful() override
        {
            return !HunterAreaHitsBystander(bot, bot->GetPositionX(), bot->GetPositionY(), 12.0f, "Explosive Trap", _noteAt) &&
                CastExplosiveTrapAction::isUseful();
        }

    private:
        uint32 _noteAt = 0;
    };

    // The actions a bot's context has made (AiObjectContext keeps the list protected; a pointer to the member named
    // through a derived class is the language's own way to it).
    struct ContextAccess : AiObjectContext
    {
        static NamedObjectContextList<Action>& Actions(AiObjectContext* context)
        {
            return context->*(&ContextAccess::actionContexts);
        }
    };

    // The actions the module took out of a context: an engine may still hold one in its queue for this tick, so they
    // are kept (two small objects per bot and context).
    std::vector<std::unique_ptr<Action>>& RetiredActions()
    {
        static std::vector<std::unique_ptr<Action>> retired;
        return retired;
    }
}

void BotDriver::GuardConsumables(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    AiObjectContext* context = ai ? ai->GetAiObjectContext() : nullptr;
    if (!context)
        return;
    WatchHolds(ai, bot->GetGUID());
    Action* drink = context->GetAction("drink");
    Action* food = context->GetAction("food");
    std::unordered_map<std::string, Action*>& created = ContextAccess::Actions(context).created;
    if (drink && !dynamic_cast<GuildDrinkAction*>(drink))
    {
        RetiredActions().emplace_back(drink);
        created["drink"] = new GuildDrinkAction(ai);
    }
    if (food && !dynamic_cast<GuildEatAction*>(food))
    {
        RetiredActions().emplace_back(food);
        created["food"] = new GuildEatAction(ai);
    }
    if (Action* attack = context->GetAction("attack anything"); attack && !dynamic_cast<GuildAttackAnythingAction*>(attack))
    {
        RetiredActions().emplace_back(attack);
        created["attack anything"] = new GuildAttackAnythingAction(ai);
    }
    if (Action* flee = context->GetAction("flee"); flee && !dynamic_cast<GuildFleeAction*>(flee))
    {
        RetiredActions().emplace_back(flee);
        created["flee"] = new GuildFleeAction(ai);
    }
    if (bot->getClass() == CLASS_MAGE)
    {
        if (Action* blink = context->GetAction("blink back"); blink && !dynamic_cast<GuildBlinkBackAction*>(blink))
        {
            RetiredActions().emplace_back(blink);
            created["blink back"] = new GuildBlinkBackAction(ai);
        }
        if (Action* nova = context->GetAction("frost nova"); nova && !dynamic_cast<GuildFrostNovaAction*>(nova))
        {
            RetiredActions().emplace_back(nova);
            created["frost nova"] = new GuildFrostNovaAction(ai);
        }
        if (Action* cone = context->GetAction("cone of cold"); cone && !dynamic_cast<GuildConeOfColdAction*>(cone))
        {
            RetiredActions().emplace_back(cone);
            created["cone of cold"] = new GuildConeOfColdAction(ai);
        }
        if (Action* blizzard = context->GetAction("blizzard"); blizzard && !dynamic_cast<GuildBlizzardAction*>(blizzard))
        {
            RetiredActions().emplace_back(blizzard);
            created["blizzard"] = new GuildBlizzardAction(ai);
        }
        if (Action* flamestrike = context->GetAction("flamestrike"); flamestrike && !dynamic_cast<GuildFlamestrikeAction*>(flamestrike))
        {
            RetiredActions().emplace_back(flamestrike);
            created["flamestrike"] = new GuildFlamestrikeAction(ai);
        }
    }
    if (bot->getClass() == CLASS_HUNTER)
    {
        if (Action* multi = context->GetAction("multi-shot"); multi && !dynamic_cast<GuildMultiShotAction*>(multi))
        {
            RetiredActions().emplace_back(multi);
            created["multi-shot"] = new GuildMultiShotAction(ai);
        }
        if (Action* volley = context->GetAction("volley"); volley && !dynamic_cast<GuildVolleyAction*>(volley))
        {
            RetiredActions().emplace_back(volley);
            created["volley"] = new GuildVolleyAction(ai);
        }
        if (Action* trap = context->GetAction("explosive trap"); trap && !dynamic_cast<GuildExplosiveTrapAction*>(trap))
        {
            RetiredActions().emplace_back(trap);
            created["explosive trap"] = new GuildExplosiveTrapAction(ai);
        }
    }
    if (bot->getClass() == CLASS_SHAMAN)
    {
        if (Action* nova = context->GetAction("fire nova"); nova && !dynamic_cast<GuildFireNovaAction*>(nova))
        {
            RetiredActions().emplace_back(nova);
            created["fire nova"] = new GuildFireNovaAction(ai);
        }
        if (Action* call = context->GetAction("call of the elements"); call && !dynamic_cast<GuildCallOfTheElementsAction*>(call))
        {
            RetiredActions().emplace_back(call);
            created["call of the elements"] = new GuildCallOfTheElementsAction(ai);
        }
        if (Action* walk = context->GetAction("water walking"); walk && !dynamic_cast<GuildWaterWalkingAction*>(walk))
        {
            RetiredActions().emplace_back(walk);
            created["water walking"] = new GuildWaterWalkingAction(ai);
        }
        if (Action* walk = context->GetAction("water walking on party"); walk && !dynamic_cast<GuildWaterWalkingOnPartyAction*>(walk))
        {
            RetiredActions().emplace_back(walk);
            created["water walking on party"] = new GuildWaterWalkingOnPartyAction(ai);
        }
    }
    // (Only a paladin's context knows the action.)
    if (bot->getClass() == CLASS_PALADIN)
    {
        if (Action* consecration = context->GetAction("consecration"); consecration && !dynamic_cast<GuildConsecrationAction*>(consecration))
        {
            RetiredActions().emplace_back(consecration);
            created["consecration"] = new GuildConsecrationAction(ai);
        }
        if (Action* command = context->GetAction("seal of command"); command && !dynamic_cast<GuildSealOfCommandAction*>(command))
        {
            RetiredActions().emplace_back(command);
            created["seal of command"] = new GuildSealOfCommandAction(ai);
        }
    }
}

void BotDriver::EquipUpgrades(Player* bot)
{
    if (PlayerbotAI* ai = GetAI(bot))
        ai->DoSpecificAction("equip upgrade", Event(), true);
}

bool BotDriver::VoteOnRolls(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    Group* group = bot->GetGroup();
    if (!ai || !group)
        return false;
    bool open = false;
    for (Roll const* roll : group->GetRolls())
    {
        auto vote = roll->playerVote.find(bot->GetGUID());
        open = open || (vote != roll->playerVote.end() && vote->second == NOT_EMITED_YET);
    }
    return open && ai->DoSpecificAction("loot roll", Event(), true);
}

namespace
{
    // nextAICheckDelay is protected; its pointer to member through a class derived from PlayerbotAIBase reads it.
    struct AIDelayPeek : PlayerbotAIBase
    {
        static uint32 Of(PlayerbotAIBase const* ai) { return ai->*(&AIDelayPeek::nextAICheckDelay); }
    };
}

// What may keep playerbots' AI of a character from acting: UpdateAI returns before its engines run while the session
// logs out, while an action's time or a pause of the module runs (nextAICheckDelay), while a spell is being cast, and
// while Playerbots.BotActiveAlone gives the bot no share (not a player's guild for playerbots, nobody near).
std::string BotDriver::AIState(Player* bot)
{
    if (!bot)
        return "-";
    if (GuildAdventure::IdleAI::IsSet(bot->GetGUID()))
        return "a placeholder (played by hand)";
    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    if (!ai)
        return "none";
    std::string text;
    auto add = [&text](std::string const& part)
    {
        text += (text.empty() ? "" : ", ") + part;
    };
    if (WorldSession* session = bot->GetSession(); !session)
        add("no session");
    else if (session->IsLoggingOut())
        add("its session logs out");
    // After every update of its AI playerbots waits Playerbots.ReactDelay plus up to 200 ms (YieldThread), and a cast
    // holds it for the global cooldown (1.5 s), so a short delay is nearly always there; only a longer one holds the AI
    // (an action's time, a pause of the module).
    constexpr uint32 HeldFromMs = 2000;
    if (uint32 const delay = AIDelayPeek::Of(ai); delay > HeldFromMs)
    {
        // Whether the module set it: its last long pause, while that can still be running.
        std::string who = "mod-playerbots' own (an action's time)";
        {
            std::lock_guard<std::mutex> guard(longPauseLock);
            if (auto const found = longPauses.find(bot->GetGUID()); found != longPauses.end() &&
                getMSTimeDiff(found->second.at, getMSTime()) < found->second.ms + 500)
            {
                std::string file = found->second.file ? found->second.file : "?";
                if (std::size_t const slash = file.find_last_of("/\\"); slash != std::string::npos)
                    file = file.substr(slash + 1);
                who = Acore::StringFormat("the module's pause of {} ms from {}:{}", found->second.ms, file, found->second.line);
            }
        }
        if (who.starts_with("mod-playerbots"))
        {
            std::lock_guard<std::mutex> guard(holdSetterLock);
            if (auto const found = holdSetters.find(bot->GetGUID()); found != holdSetters.end() &&
                getMSTimeDiff(found->second.at, getMSTime()) < found->second.ms + 1000)
                who = Acore::StringFormat("mod-playerbots' {} of {} ms, {} ms ago", found->second.action.starts_with("set outside") ?
                    found->second.action : "action \"" + found->second.action + "\"",
                    found->second.ms, getMSTimeDiff(found->second.at, getMSTime()));
        }
        add(Acore::StringFormat("held for {} ms more ({})", delay, who));
    }
    Spell* spell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!spell)
        spell = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    if (spell && spell->GetSpellInfo())
        add(Acore::StringFormat("casting {} ({}, state {})", spell->GetSpellInfo()->SpellName[0], spell->GetSpellInfo()->Id,
            uint32(spell->getState())));
    add(ai->IsInRealGuild() ? "in a player's guild for playerbots" : "not in a player's guild for playerbots");
    add(ai->AllowActivity(ALL_ACTIVITY) ? "active" : "inactive (Playerbots.BotActiveAlone)");
    add(ai->GetState() == BOT_STATE_COMBAT ? "combat engine" : ai->GetState() == BOT_STATE_DEAD ? "dead engine" :
        "non-combat engine");
    return text;
}

std::string BotDriver::DescribeAI(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    if (!ai)
        return "no AI";

    Engine* engine = ai->GetCurrentEngine();
    std::string text = "ai: " + AIState(bot) + "; last action: ";
    text += engine ? engine->GetLastAction() : std::string("-");
    text += "; nc strategies: ";
    std::string strategies;
    for (std::string const& name : ai->GetStrategies(BOT_STATE_NON_COMBAT))
        strategies += (strategies.empty() ? "" : ",") + name;
    text += strategies;
    text += "; combat strategies: ";
    strategies.clear();
    for (std::string const& name : ai->GetStrategies(BOT_STATE_COMBAT))
        strategies += (strategies.empty() ? "" : ",") + name;
    text += strategies;
    return text;
}

uint8 BotDriver::SoloRole(Player* bot)
{
    if (!GetAI(bot))
        return 0;
    BotRoles const roles = AiFactory::GetPlayerRoles(bot);
    if (roles & BOT_ROLE_HEALER)
        return 2;
    return (roles & BOT_ROLE_TANK) && !(roles & BOT_ROLE_DPS) ? 1 : 0;
}

std::string BotDriver::LastAction(Player* bot)
{
    PlayerbotAI* ai = GetAI(bot);
    Engine* engine = ai ? ai->GetCurrentEngine() : nullptr;
    return engine ? engine->GetLastAction() : std::string();
}
