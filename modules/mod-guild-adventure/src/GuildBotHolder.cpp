/*
 * GuildBotHolder implementation. See GuildBotHolder.h and docs/take-control.md §4.2 / §5.1.
 */

#include "GuildBotHolder.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Event.h"
#include "GossipDef.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotRepository.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <memory>

using namespace GuildAdventure;

namespace
{
    // Watch mode: the control lock is sent again at least this often (docs/take-control.md §5.12).
    constexpr uint32 WatchLockResendMs = 5000;

    // Feared, confused or charmed: the core has taken the client's movement controls itself and gives them back when
    // the effect ends (Unit::SetFeared / SetConfused / RemoveCharmedBy), which also undoes the watch-mode lock.
    bool IsCrowdControlled(Player* player)
    {
        return player->HasUnitState(UNIT_STATE_FLEEING | UNIT_STATE_CONFUSED) || player->IsCharmed();
    }

    // Stops what the character's movement generators are doing, unless that would break something the core moves:
    // a flight path (clearing it drops the character out of the sky), a fear or confuse effect, a charge or a jump.
    void StopMovementIfSafe(Player* player)
    {
        if (player->IsInFlight() || player->IsCharmed() || player->HasUnitState(UNIT_STATE_LOST_CONTROL))
            return;
        player->StopMoving();
        player->GetMotionMaster()->Clear();
    }

    // SMSG_CLIENT_CONTROL_UPDATE for the client's own character: allowMove = false locks its movement keys, the way
    // the core does for a fear effect; the server keeps moving the character with splines the client plays.
    void SetClientMovement(Player* player, bool allowMove)
    {
        player->SetClientControl(player, allowMove, true);
    }
}

char const* GuildAdventure::ToString(ManagedState state)
{
    switch (state)
    {
        case ManagedState::Loading:    return "loading";
        case ManagedState::Loaded:     return "loaded";
        case ManagedState::Failed:     return "failed";
        case ManagedState::Bot:        return "bot";
        case ManagedState::Controlled: return "controlled";
    }
    return "?";
}

char const* GuildAdventure::ToString(LoginResult result)
{
    switch (result)
    {
        case LoginResult::Started:          return "login started";
        case LoginResult::AlreadyManaged:   return "already a guild bot";
        case LoginResult::AlreadyOnline:    return "character is online";
        case LoginResult::UnknownCharacter: return "unknown character";
        case LoginResult::QueryFailed:      return "login query could not be prepared";
    }
    return "?";
}

GuildBotHolder& GuildBotHolder::Instance()
{
    static GuildBotHolder instance;
    return instance;
}

WorldSession* GuildBotHolder::CreateHomeSession(uint32 accountId)
{
    // Same parameters mod-playerbots uses for its bot sessions (PlayerbotHolder::AddPlayerBot): no socket, so
    // the session is headless; it is never registered in WorldSessionMgr.
    return new WorldSession(accountId, "", 0x0, nullptr, SEC_PLAYER, EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0),
        sWorld->GetDefaultDbcLocale(), 0, false, false, 0);
}

void GuildBotHolder::MoveToSession(Player* player, WorldSession* from, WorldSession* to)
{
    ASSERT(player && from && to);
    ASSERT(player->GetSession() == from && from->GetPlayer() == player);
    ASSERT(to->GetPlayer() == nullptr);

    from->SetPlayer(nullptr);
    to->SetPlayer(player);
    player->SetSession(to);

    // PlayerMenu caches the session pointer (GossipDef.h)
    delete player->PlayerTalkClass;
    player->PlayerTalkClass = new PlayerMenu(to);

    // Movement acks carry the new session's order counter; rebase the player's counters to it so acks are not
    // dropped as "from an old map" and no flight-flag change counts as pending.
    player->SetMapChangeOrderCounter();
    player->SetPendingFlightChange(to->GetOrderCounter());
}

LoginResult GuildBotHolder::Login(ObjectGuid guid, uint32 ownerAccountId, ObjectGuid masterGuid)
{
    if (_entries.find(guid) != _entries.end())
        return LoginResult::AlreadyManaged;

    if (ObjectAccessor::FindConnectedPlayer(guid) || sWorldSessionMgr->FindOfflineSessionForCharacterGUID(guid.GetCounter()))
        return LoginResult::AlreadyOnline;

    uint32 const accountId = sCharacterCache->GetCharacterAccountIdByGuid(guid);
    if (!accountId)
        return LoginResult::UnknownCharacter;

    std::shared_ptr<LoginQueryHolder> holder = std::make_shared<LoginQueryHolder>(accountId, guid);
    if (!holder->Initialize())
        return LoginResult::QueryFailed;

    Entry& entry = _entries[guid];
    entry.ownerAccountId = ownerAccountId;
    entry.characterAccountId = accountId;
    entry.homeSession = CreateHomeSession(accountId);
    entry.state = ManagedState::Loading;
    entry.masterGuid = masterGuid;

    // The callback runs inside the home session's own query processor (driven by Update), so it only records the
    // outcome; adoption and any teardown happen in Update, outside the processor.
    entry.homeSession->AddQueryHolderCallback(CharacterDatabase.DelayQueryHolder(holder)).AfterComplete(
        [this, guid](SQLQueryHolderBase const& queryHolder)
        {
            auto itr = _entries.find(guid);
            if (itr == _entries.end())
                return;

            WorldSession* session = itr->second.homeSession;
            session->HandlePlayerLoginFromDB(static_cast<LoginQueryHolder const&>(queryHolder));
            itr->second.state = session->GetPlayer() ? ManagedState::Loaded : ManagedState::Failed;
        });

    LOG_INFO("module", "mod-guild-adventure: logging in guild bot {} (account {}) for owner account {}",
        guid.ToString(), accountId, ownerAccountId);
    return LoginResult::Started;
}

void GuildBotHolder::OnBotLoginInternal(Player* const bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    Player* master = nullptr;
    auto itr = _entries.find(bot->GetGUID());
    if (itr != _entries.end() && !itr->second.masterGuid.IsEmpty())
        master = ObjectAccessor::FindConnectedPlayer(itr->second.masterGuid);

    // Never make a bot its own master: playerbots treats that as a "self bot" (object updates, IsRealPlayer)
    if (master == bot)
        master = nullptr;

    // Guild bots on an autonomous order have no master (and so are not invited into the owner's party below in
    // PlayerbotHolder::OnBotLogin).
    if (master && itr != _entries.end() && !WantsMaster(bot->GetGUID(), itr->second))
        master = nullptr;

    botAI->SetMaster(master);
    botAI->ResetStrategies();
}

void GuildBotHolder::Adopt(Player* player, Entry& entry)
{
    // PlayerbotHolder::OnBotLogin creates the PlayerbotAI (it caches the session's account id, so the player must
    // already sit on its home session), inserts the bot into playerBots, calls OnBotLoginInternal (master and
    // strategies), loads the saved strategies and handles the master's group.
    entry.state = ManagedState::Bot;
    OnBotLogin(player);
    ApplyLeaderPolicy(player, entry);
}

void GuildBotHolder::ApplyLeaderPolicy(Player* player, Entry const& entry)
{
    // The leader (the owner's own character) holds its position while the owner plays a bot (the design log §2.8), unless
    // it has an order: then the order planner plays it like a guild bot (docs/orders.md "The leader").
    if (entry.characterAccountId != entry.ownerAccountId)
        return;
    if (_leaderPlanPolicy && _leaderPlanPolicy(player->GetGUID()))
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI || !botAI->GetMaster())
        return;

    if (!botAI->DoSpecificAction("stay chat shortcut", Event(), true))
        LOG_WARN("module", "mod-guild-adventure: leader {} could not be told to stay", player->GetName());
}

void GuildBotHolder::Quiesce(Player* player, WorldSession* session)
{
    player->StopMoving();
    player->GetMotionMaster()->Clear();
    player->InterruptNonMeleeSpells(false);
    if (ObjectGuid const loot = player->GetLootGUID())
        session->DoLootRelease(loot);
    player->PlayerTalkClass->SendCloseGossip();
    if (player->GetTradeData())
        player->TradeCancel(true);
    if (player->duel)
        player->DuelComplete(DUEL_INTERRUPTED);
    // A unit the character controls goes back to its own AI, and the client's view returns to the character.
    if (!player->GetCharmGUID().IsEmpty() || player->m_mover != player)
        player->StopCastingCharm();
    player->StopCastingBindSight();
    if (player->GetVehicle())
        player->ExitVehicle();
}

std::string GuildBotHolder::SettleForClient(Player* player)
{
    std::vector<std::string> changes;

    // What only a running client sets: direction keys, a fall in progress, pending movement acks. A fresh client
    // starts without them; a stale fall would start the client falling (and the server counting fall damage from a
    // height long gone).
    constexpr uint32 ClientOnlyFlags = MOVEMENTFLAG_MASK_MOVING | MOVEMENTFLAG_MASK_TURNING | MOVEMENTFLAG_PITCH_UP |
        MOVEMENTFLAG_PITCH_DOWN | MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR | MOVEMENTFLAG_PENDING_STOP |
        MOVEMENTFLAG_PENDING_STRAFE_STOP | MOVEMENTFLAG_PENDING_FORWARD | MOVEMENTFLAG_PENDING_BACKWARD |
        MOVEMENTFLAG_PENDING_STRAFE_LEFT | MOVEMENTFLAG_PENDING_STRAFE_RIGHT | MOVEMENTFLAG_PENDING_ROOT |
        MOVEMENTFLAG_ASCENDING | MOVEMENTFLAG_DESCENDING | MOVEMENTFLAG_SPLINE_ELEVATION;
    uint32 const stale = player->GetUnitMovementFlags() & ClientOnlyFlags;
    if (stale || player->m_movementInfo.fallTime >= 500) // a few milliseconds are left over from any step
        changes.push_back(Acore::StringFormat("movement flags 0x{:08X} and a fall of {} ms cleared", stale,
            player->m_movementInfo.fallTime));
    player->RemoveUnitMovementFlag(ClientOnlyFlags);
    player->m_movementInfo.fallTime = 0;

    // Ships, flight paths, flying and hovering keep their height.
    bool const keepHeight = player->GetTransport() || player->IsInFlight() || player->IsFlying() ||
        player->HasUnitMovementFlag(MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_HOVER);
    Map* map = player->GetMap();
    if (!keepHeight && map)
    {
        uint32 const phase = player->GetPhaseMask();
        float const x = player->GetPositionX();
        float const y = player->GetPositionY();
        float const z = player->GetPositionZ();

        // The floor right under its feet, searched from 2 yd above them (no ceiling of a room is that low).
        float ground = INVALID_HEIGHT;
        float const floor = map->GetHeight(phase, x, y, z + 2.0f, true, 50.0f);
        if (floor > INVALID_HEIGHT && floor >= z - 2.0f)
        {
            if (floor > z + 0.05f) // a little under the floor it stands on
                ground = floor;
        }
        else
        {
            // Nothing under its feet: in the air or the water (left alone, the client lands or swims), or under the
            // ground: then the nearest floor above it, else the highest surface of its spot.
            for (float const up : { 5.0f, 10.0f, 20.0f, 40.0f, 80.0f, 160.0f })
            {
                float const above = map->GetHeight(phase, x, y, z + up, true, up);
                if (above > INVALID_HEIGHT && above > z)
                {
                    ground = above;
                    break;
                }
            }
            if (ground <= INVALID_HEIGHT)
            {
                float const surface = map->GetHeight(phase, x, y, MAX_HEIGHT, true, MAX_HEIGHT);
                if (surface > INVALID_HEIGHT && surface > z)
                    ground = surface;
            }
        }

        if (ground > INVALID_HEIGHT)
        {
            changes.push_back(Acore::StringFormat("lifted from z {:.2f} onto the ground at {:.2f}", z, ground));
            player->UpdatePosition(x, y, ground, player->GetOrientation(), true);
        }
    }

    // A fall the client lands from is counted from here, not from a height the AI left long ago.
    player->SetFallInformation(0, player->GetPositionZ());

    std::string text;
    for (std::string const& change : changes)
        text += (text.empty() ? "" : "; ") + change;
    return text;
}

bool GuildBotHolder::HandToAI(Player* player, WorldSession* from, uint32 ownerAccountId, Player* master)
{
    if (!player || !from || player->GetSession() != from || from->GetPlayer() != player)
        return false;

    ObjectGuid const guid = player->GetGUID();
    auto itr = _entries.find(guid);
    if (itr != _entries.end() && itr->second.state != ManagedState::Controlled)
    {
        LOG_ERROR("module", "mod-guild-adventure: HandToAI({}) refused, entry is {}", guid.ToString(), ToString(itr->second.state));
        return false;
    }

    // Watch mode: its AI lives on the client session and ends with the character's client role; the home session
    // gets a fresh one in Adopt. The client is about to play another character (or none), so its movement lock is
    // left alone: a swap's world reload or the character screen resets it anyway.
    if (itr != _entries.end() && itr->second.watched)
        EndWatch(player, itr->second, false);
    // A placeholder AI goes the same way: Adopt gives the character its real one.
    if (itr != _entries.end() && itr->second.placeholder)
        EndPlaceholder(guid, player, itr->second);

    // The master manager playerbots created at the client login belongs to a real player only. The caller made
    // sure it holds no stock altbots (ControlSwapService::CanSwap); deleting it unregisters it. The random-bot
    // manager forgets the player as a real player, as on a stock logout (Playerbots.cpp OnPlayerBeforeLogout).
    if (PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(player))
        delete mgr;
    sRandomPlayerbotMgr.OnPlayerLogout(player);

    if (itr == _entries.end())
    {
        Entry entry;
        entry.ownerAccountId = ownerAccountId;
        entry.characterAccountId = player->GetCharacterAccountId();
        entry.homeSession = CreateHomeSession(entry.characterAccountId);
        itr = _entries.emplace(guid, entry).first;
    }

    Entry& entry = itr->second;
    entry.masterGuid = (master && master != player) ? master->GetGUID() : ObjectGuid::Empty;

    // Still on the client session: its windows close there, and nothing the client started carries over.
    Quiesce(player, from);
    MoveToSession(player, from, entry.homeSession);
    Adopt(player, entry);

    LOG_INFO("module", "mod-guild-adventure: {} handed to the AI (owner account {}, master {})", player->GetName(),
        entry.ownerAccountId, master ? master->GetName() : "none");
    return true;
}

bool GuildBotHolder::TakeFromAI(Player* player, WorldSession* to)
{
    if (!player || !to || to->GetPlayer())
        return false;

    ObjectGuid const guid = player->GetGUID();
    auto itr = _entries.find(guid);
    if (itr == _entries.end() || itr->second.state != ManagedState::Bot || player->GetSession() != itr->second.homeSession)
        return false;

    Entry& entry = itr->second;

    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(player))
    {
        PlayerbotRepository::instance().Save(botAI);
        player->StopMoving();
        player->GetMotionMaster()->Clear();
        RemoveFromPlayerbotsMap(guid);
        delete botAI; // unregisters itself from PlayerbotsMgr
    }
    else
        RemoveFromPlayerbotsMap(guid);

    // Let ready callbacks run while the home session still has its player, then park it: drop the client packets
    // the AI queued, the rest of its callbacks stay frozen until the character comes back (docs/take-control.md §4.2)
    entry.homeSession->ProcessQueryCallbacks();
    while (entry.homeSession->NextQueuedPacket()) { }

    MoveToSession(player, entry.homeSession, to);
    entry.state = ManagedState::Controlled;

    // The client-controlled character is a real player for playerbots: give it a master manager and register it
    // with the random-bot manager, as the stock login does (Playerbots.cpp OnPlayerLogin).
    sPlayerbotsMgr.AddPlayerbotData(player, false);
    sRandomPlayerbotMgr.OnPlayerLogin(player);

    LOG_INFO("module", "mod-guild-adventure: {} taken from the AI by account {}", player->GetName(), to->GetAccountId());

    // A member of a dungeon group: before the next map update (the bots of its party would take a party member
    // without an AI for their master) it gets its placeholder.
    if (_placeholderPolicy && _placeholderPolicy(guid))
        StartPlaceholder(player);
    return true;
}

bool GuildBotHolder::StartWatch(Player* player)
{
    if (!player)
        return false;

    WorldSession* session = player->GetSession();
    if (!session || session->IsHeadless() || session->GetPlayer() != player)
        return false;

    auto itr = _entries.find(player->GetGUID());
    if (itr == _entries.end())
    {
        // The leader the owner logged into from the character screen has no entry yet (HandToAI makes one at the first
        // swap). It gets one now, controlled by this session, with its parked home session like any other.
        Entry entry;
        entry.ownerAccountId = session->GetAccountId();
        entry.characterAccountId = player->GetCharacterAccountId();
        entry.homeSession = CreateHomeSession(entry.characterAccountId);
        entry.state = ManagedState::Controlled;
        itr = _entries.emplace(player->GetGUID(), entry).first;
    }
    if (itr->second.state != ManagedState::Controlled)
        return false;

    Entry& entry = itr->second;
    if (entry.watched)
        return true;

    if (session == entry.homeSession)
        return false;

    // A placeholder AI (a member of a dungeon group played by hand) makes room for the real one, in this same call.
    if (entry.placeholder)
        EndPlaceholder(player->GetGUID(), player, entry);

    // Only the watch-mode AI may sit on a client session here; anything else is playerbots' own "self bot".
    if (GET_PLAYERBOT_AI(player))
    {
        LOG_ERROR("module", "mod-guild-adventure: StartWatch({}) refused, the character already has a playerbots AI",
            player->GetName());
        return false;
    }

    // What the client was doing (a run, a click-to-move) ends here; the AI starts from a standing character. A cast
    // in progress and a fight go on.
    StopMovementIfSafe(player);

    // A PlayerbotAI on the client session, as playerbots' own "self bot" (PlayerbotMgr::HandlePlayerbotCommand "self")
    // but without a master: never the character itself (playerbots would treat it as a person at the keyboard), and
    // no one else either, so the order planner (OrderService) drives it exactly like an AI-driven guild bot. The real
    // player's master manager (GET_PLAYERBOT_MGR) stays: the client is still connected. playerbots updates the AI of
    // every player that has one (PlayerbotsPlayerScript::OnPlayerAfterUpdate), wherever it sits; teleport acks stay
    // the client's (only PlayerbotHolder::UpdateSessions sends them for a bot, and this one is not in playerBots).
    sPlayerbotsMgr.AddPlayerbotData(player, true);
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI)
        return false;

    botAI->SetMaster(nullptr);
    botAI->ResetStrategies();
    PlayerbotRepository::instance().Load(botAI);
    if (sPlayerbotAIConfig.ApplyInstanceStrategies)
        botAI->ApplyInstanceStrategies(player->GetMapId());

    entry.watched = true;
    RefreshWatchedNow();
    entry.watchTeleporting = player->IsBeingTeleported();
    entry.watchCrowdControlled = IsCrowdControlled(player);
    entry.watchFlying = player->IsInFlight();
    entry.watchLockedAt = getMSTime();
    if (player->IsInWorld() && !entry.watchTeleporting && !entry.watchCrowdControlled)
        SetClientMovement(player, false);

    LOG_INFO("module", "mod-guild-adventure: account {} watches {}, the AI plays it", session->GetAccountId(),
        player->GetName());
    return true;
}

bool GuildBotHolder::StopWatch(Player* player, bool restoreClientControl)
{
    if (!player)
        return false;

    auto itr = _entries.find(player->GetGUID());
    if (itr == _entries.end() || itr->second.state != ManagedState::Controlled || !itr->second.watched)
        return false;

    EndWatch(player, itr->second, restoreClientControl);
    LOG_INFO("module", "mod-guild-adventure: account {} takes the controls of {} again",
        player->GetSession() ? player->GetSession()->GetAccountId() : 0, player->GetName());
    return true;
}

void GuildBotHolder::EndWatch(Player* player, Entry& entry, bool restoreClientControl)
{
    entry.watched = false;
    RefreshWatchedNow();

    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(player))
    {
        PlayerbotRepository::instance().Save(botAI);
        delete botAI; // unregisters itself from PlayerbotsMgr
    }

    // The AI's walk ends where the character stands; a spell it is casting and the fight it is in go on, the owner
    // plays on from here.
    StopMovementIfSafe(player);

    // During a teleport the client unlocks its controls by itself; under a fear, confuse or charm effect the core does
    // when the effect ends.
    if (restoreClientControl && player->IsInWorld() && !player->IsBeingTeleported() && !IsCrowdControlled(player))
        SetClientMovement(player, true);

    // Still a member of a dungeon group: the owner's hands again, behind a placeholder AI, in this same call.
    if (restoreClientControl && _placeholderPolicy && _placeholderPolicy(player->GetGUID()))
        StartPlaceholder(player);
}

bool GuildBotHolder::StartPlaceholder(Player* player)
{
    if (!player || !IdleAI::Available())
        return false;

    WorldSession* session = player->GetSession();
    if (!session || session->IsHeadless() || session->GetPlayer() != player)
        return false;

    ObjectGuid const guid = player->GetGUID();
    auto itr = _entries.find(guid);
    if (itr == _entries.end())
    {
        // (As in StartWatch: the leader the owner logged into from the character screen has no entry yet.)
        Entry entry;
        entry.ownerAccountId = session->GetAccountId();
        entry.characterAccountId = player->GetCharacterAccountId();
        entry.homeSession = CreateHomeSession(entry.characterAccountId);
        entry.state = ManagedState::Controlled;
        itr = _entries.emplace(guid, entry).first;
    }
    Entry& entry = itr->second;
    if (entry.state != ManagedState::Controlled || entry.watched || session == entry.homeSession)
        return false;
    if (entry.placeholder)
        return true;
    if (GET_PLAYERBOT_AI(player))
    {
        LOG_ERROR("module", "mod-guild-adventure: StartPlaceholder({}) refused, the character already has a playerbots AI",
            player->GetName());
        return false;
    }

    // Idle before it exists: playerbots never updates it (ObjectUpdateScripts.cpp), so nothing of it ever acts. It
    // has no master, like the watch-mode AI.
    IdleAI::Set(guid, true);
    sPlayerbotsMgr.AddPlayerbotData(player, true);
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI)
    {
        IdleAI::Set(guid, false);
        return false;
    }
    botAI->SetMaster(nullptr);
    entry.placeholder = true;

    LOG_INFO("module", "mod-guild-adventure: {} is played by hand inside a group of bots: it carries a placeholder AI that "
        "never runs, so the bots keep their own leader", player->GetName());
    return true;
}

bool GuildBotHolder::StopPlaceholder(Player* player)
{
    if (!player)
        return false;

    auto itr = _entries.find(player->GetGUID());
    if (itr == _entries.end() || !itr->second.placeholder)
        return false;

    EndPlaceholder(player->GetGUID(), player, itr->second);
    return true;
}

void GuildBotHolder::EndPlaceholder(ObjectGuid guid, Player* player, Entry& entry)
{
    entry.placeholder = false;
    if (player)
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(player))
            delete botAI; // unregisters itself from PlayerbotsMgr
    IdleAI::Set(guid, false);
}

bool GuildBotHolder::HasPlaceholder(ObjectGuid guid) const
{
    auto itr = _entries.find(guid);
    return itr != _entries.end() && itr->second.placeholder;
}

void GuildBotHolder::UpdatePlaceholders()
{
    for (auto& [guid, entry] : _entries)
    {
        if (!entry.placeholder)
            continue;

        Player* player = ObjectAccessor::FindConnectedPlayer(guid);
        if (player && entry.state == ManagedState::Controlled && _placeholderPolicy && _placeholderPolicy(guid))
            continue;

        // Nobody needs it any more (the dungeon group is over, the character left it). Out of the bots' party first:
        // a party member without an AI is the master of every bot in it for playerbots.
        if (player)
            if (Group* group = player->GetGroup())
            {
                bool bots = false;
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    if (Player* member = ref->GetSource(); member && member != player && IsAIDriven(member->GetGUID()))
                        bots = true;
                if (bots)
                    player->RemoveFromGroup();
            }
        EndPlaceholder(guid, player, entry);
        if (player)
            LOG_INFO("module", "mod-guild-adventure: {} is out of the group of bots: its placeholder AI is gone", player->GetName());
    }
}

bool GuildBotHolder::IsWatched(ObjectGuid guid) const
{
    auto itr = _entries.find(guid);
    return itr != _entries.end() && itr->second.state == ManagedState::Controlled && itr->second.watched;
}

bool GuildBotHolder::IsWatchedAnyThread(ObjectGuid guid) const
{
    std::shared_lock<std::shared_mutex> lock(_watchedNowLock);
    return _watchedNow.count(guid) != 0;
}

void GuildBotHolder::RefreshWatchedNow()
{
    std::unordered_set<ObjectGuid> watched;
    for (auto const& [guid, entry] : _entries)
        if (entry.state == ManagedState::Controlled && entry.watched)
            watched.insert(guid);
    std::unique_lock<std::shared_mutex> lock(_watchedNowLock);
    _watchedNow.swap(watched);
}

void GuildBotHolder::UpdateWatchLocks()
{
    RefreshWatchedNow();
    uint32 const now = getMSTime();
    for (auto& [guid, entry] : _entries)
    {
        if (entry.state != ManagedState::Controlled || !entry.watched)
            continue;

        Player* player = ObjectAccessor::FindConnectedPlayer(guid);
        if (!player || player->GetSession() == entry.homeSession)
            continue;

        // The client unlocks its movement controls after every teleport ("client forgets about losing control",
        // WorldSession::HandleMoveTeleportAck; Battleground.cpp relies on it for a far teleport), and the core unlocks
        // them when a fear, confuse or charm effect ends. Lock again right after either and right after a flight path
        // (the client hands control back when the taxi lands), and every few seconds as a safety net for anything else
        // that resets them.
        bool const teleporting = player->IsBeingTeleported();
        bool const controlled = IsCrowdControlled(player);
        bool const flying = player->IsInFlight();
        bool resend = getMSTimeDiff(entry.watchLockedAt, now) >= WatchLockResendMs;
        if (entry.watchTeleporting && !teleporting)
            resend = true;
        if (entry.watchCrowdControlled && !controlled)
            resend = true;
        if (entry.watchFlying && !flying)
            resend = true;
        entry.watchTeleporting = teleporting;
        entry.watchCrowdControlled = controlled;
        entry.watchFlying = flying;

        if (resend && !teleporting && !controlled && player->IsInWorld())
        {
            SetClientMovement(player, false);
            entry.watchLockedAt = now;
        }
    }
}

bool GuildBotHolder::Logout(ObjectGuid guid)
{
    auto itr = _entries.find(guid);
    if (itr == _entries.end() || itr->second.state != ManagedState::Bot)
        return false;

    WorldSession* session = itr->second.homeSession;
    Player* bot = session->GetPlayer();
    if (bot)
    {
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            PlayerbotRepository::instance().Save(botAI);
        LOG_INFO("module", "mod-guild-adventure: logging out guild bot {}", bot->GetName());
    }

    RemoveFromPlayerbotsMap(guid);
    _entries.erase(itr);

    session->LogoutPlayer(true); // removes the player from the world and deletes it (and its AI)
    delete session;
    return true;
}

void GuildBotHolder::LogoutLeaderOf(uint32 ownerAccountId)
{
    std::vector<ObjectGuid> leaders;
    for (auto const& [guid, entry] : _entries)
        if (entry.ownerAccountId == ownerAccountId && entry.characterAccountId == ownerAccountId && entry.state == ManagedState::Bot)
            leaders.push_back(guid);

    for (ObjectGuid const& guid : leaders)
        Logout(guid);
}

void GuildBotHolder::LogoutAll()
{
    std::vector<ObjectGuid> guids;
    for (auto const& [guid, entry] : _entries)
        guids.push_back(guid);

    for (ObjectGuid const& guid : guids)
    {
        auto itr = _entries.find(guid);
        if (itr == _entries.end())
            continue;

        if (itr->second.state == ManagedState::Bot)
        {
            Logout(guid);
            continue;
        }

        // Loading / loaded / failed / controlled: the home session holds no AI-driven player. A loaded but not yet
        // adopted player is logged out with its session; a controlled one stays with its client session (a watched one
        // without its watch-mode AI).
        if (itr->second.watched)
            if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                EndWatch(player, itr->second, false);
        if (itr->second.placeholder)
            EndPlaceholder(guid, ObjectAccessor::FindConnectedPlayer(guid), itr->second);

        WorldSession* session = itr->second.homeSession;
        _entries.erase(itr);
        if (session->GetPlayer())
            session->LogoutPlayer(true);
        delete session;
    }
}

void GuildBotHolder::SetMasterForOwner(uint32 ownerAccountId, Player* master)
{
    for (auto& [guid, entry] : _entries)
    {
        if (entry.ownerAccountId != ownerAccountId)
            continue;

        entry.masterGuid = master ? master->GetGUID() : ObjectGuid::Empty;
        if (entry.state != ManagedState::Bot)
            continue;

        Player* bot = entry.homeSession->GetPlayer();
        if (!bot || bot == master || (master && !WantsMaster(guid, entry)))
            continue;

        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            botAI->SetMaster(master);
    }
}

bool GuildBotHolder::WantsMaster(ObjectGuid guid, Entry const& entry) const
{
    // The leader (a character of the owner's own account) follows the owner's lead, unless it has an order: then the
    // master policy decides as for a guild bot.
    if (entry.characterAccountId == entry.ownerAccountId && !(_leaderPlanPolicy && _leaderPlanPolicy(guid)))
        return true;
    return !_masterPolicy || _masterPolicy(guid);
}

void GuildBotHolder::ReapplyLeaderPolicy(ObjectGuid leader)
{
    auto itr = _entries.find(leader);
    if (itr == _entries.end() || itr->second.state != ManagedState::Bot)
        return;

    Entry& entry = itr->second;
    Player* bot = entry.homeSession->GetPlayer();
    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!botAI)
        return;

    Player* master = entry.masterGuid.IsEmpty() ? nullptr : ObjectAccessor::FindConnectedPlayer(entry.masterGuid);
    if (master == bot || (master && !master->IsInWorld()))
        master = nullptr;
    botAI->SetMaster(master);
    botAI->ResetStrategies();
    ApplyLeaderPolicy(bot, entry);
}

Player* GuildBotHolder::GetOwnerCharacter(ObjectGuid guid) const
{
    auto itr = _entries.find(guid);
    if (itr == _entries.end() || itr->second.masterGuid.IsEmpty())
        return nullptr;

    Player* owner = ObjectAccessor::FindConnectedPlayer(itr->second.masterGuid);
    if (!owner || owner->GetGUID() == guid || !owner->IsInWorld() || owner->IsDuringRemoveFromWorld())
        return nullptr;

    // Only while that character really is on the owner's client session.
    WorldSession* session = owner->GetSession();
    if (!session || session->IsHeadless() || session->GetAccountId() != itr->second.ownerAccountId)
        return nullptr;
    return owner;
}

void GuildBotHolder::ClearMaster(Player* player)
{
    if (!player)
        return;

    for (auto& [guid, entry] : _entries)
    {
        bool const watched = entry.state == ManagedState::Controlled && entry.watched;
        if (entry.state != ManagedState::Bot && !watched)
            continue;

        Player* bot = watched ? ObjectAccessor::FindConnectedPlayer(guid) : entry.homeSession->GetPlayer();
        if (!bot || bot == player)
            continue;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (botAI && botAI->GetMaster() == player)
        {
            botAI->SetMaster(nullptr);
            botAI->ResetStrategies();
        }
    }
}

void GuildBotHolder::ForgetControlled(ObjectGuid guid)
{
    auto itr = _entries.find(guid);
    if (itr == _entries.end() || itr->second.state != ManagedState::Controlled)
        return;

    if (itr->second.watched)
        if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
            EndWatch(player, itr->second, false);
    if (itr->second.placeholder)
        EndPlaceholder(guid, ObjectAccessor::FindConnectedPlayer(guid), itr->second);

    // The parked home session holds no player; its frozen callbacks belonged to the character that leaves now.
    WorldSession* session = itr->second.homeSession;
    _entries.erase(itr);
    delete session;

    LOG_INFO("module", "mod-guild-adventure: controlled character {} left the world, home session released", guid.ToString());
}

bool GuildBotHolder::OnCanMarkAccountOffline(ObjectGuid guid, uint32 accountId)
{
    // The account is the session's: a home session's is the character's own account, the owner's client session's
    // is the owner's account (the leader's), also while it plays a guild bot of a bot account.
    for (auto const& [otherGuid, entry] : _entries)
    {
        if (otherGuid == guid || entry.characterAccountId != accountId || entry.state == ManagedState::Failed)
            continue;

        // Same column as the save at logout writes (Player::SaveToDB, online = 0 while logging out); also covers a
        // logout without a save.
        CharacterDatabase.Execute("UPDATE characters SET online = 0 WHERE guid = {}", guid.GetCounter());
        LOG_DEBUG("module", "mod-guild-adventure: {} left the world, account {} stays online for {}", guid.ToString(),
            accountId, otherGuid.ToString());
        return false;
    }
    return true;
}

bool GuildBotHolder::IsManaged(ObjectGuid guid) const
{
    return _entries.find(guid) != _entries.end();
}

bool GuildBotHolder::IsAIDriven(ObjectGuid guid) const
{
    auto itr = _entries.find(guid);
    return itr != _entries.end() && itr->second.state == ManagedState::Bot;
}

bool GuildBotHolder::IsLoggingIn(ObjectGuid guid) const
{
    auto itr = _entries.find(guid);
    return itr != _entries.end() &&
        (itr->second.state == ManagedState::Loading || itr->second.state == ManagedState::Loaded);
}

uint32 GuildBotHolder::GetOwner(ObjectGuid guid) const
{
    auto itr = _entries.find(guid);
    return itr != _entries.end() ? itr->second.ownerAccountId : 0;
}

Player* GuildBotHolder::GetAIDrivenPlayer(ObjectGuid guid) const
{
    auto itr = _entries.find(guid);
    if (itr == _entries.end() || itr->second.state != ManagedState::Bot)
        return nullptr;
    return itr->second.homeSession->GetPlayer();
}

Player* GuildBotHolder::GetPlannedPlayer(ObjectGuid guid) const
{
    auto itr = _entries.find(guid);
    if (itr == _entries.end())
        return nullptr;
    if (itr->second.state == ManagedState::Bot)
        return itr->second.homeSession->GetPlayer();
    if (itr->second.state == ManagedState::Controlled && itr->second.watched)
        return ObjectAccessor::FindConnectedPlayer(guid);
    return nullptr;
}

std::vector<ManagedInfo> GuildBotHolder::List(uint32 ownerAccountId) const
{
    std::vector<ManagedInfo> result;
    for (auto const& [guid, entry] : _entries)
    {
        if (ownerAccountId && entry.ownerAccountId != ownerAccountId)
            continue;

        ManagedInfo info;
        info.guid = guid;
        info.ownerAccountId = entry.ownerAccountId;
        info.characterAccountId = entry.characterAccountId;
        info.state = entry.state;
        info.watched = entry.state == ManagedState::Controlled && entry.watched;
        sCharacterCache->GetCharacterNameByGuid(guid, info.name);
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<ManagedInfo> GuildBotHolder::ListControlled() const
{
    std::vector<ManagedInfo> result;
    for (auto const& [guid, entry] : _entries)
    {
        if (entry.state != ManagedState::Controlled)
            continue;

        ManagedInfo info;
        info.guid = guid;
        info.ownerAccountId = entry.ownerAccountId;
        info.characterAccountId = entry.characterAccountId;
        info.state = entry.state;
        info.watched = entry.watched;
        result.push_back(std::move(info));
    }
    return result;
}

void GuildBotHolder::Update()
{
    // 1. Drive the logins in flight; the callbacks only record their outcome.
    std::vector<ObjectGuid> guids;
    guids.reserve(_entries.size());
    for (auto const& [guid, entry] : _entries)
        if (entry.state == ManagedState::Loading)
            guids.push_back(guid);

    for (ObjectGuid const& guid : guids)
    {
        auto itr = _entries.find(guid);
        if (itr != _entries.end())
            itr->second.homeSession->ProcessQueryCallbacks();
    }

    // 2. Finish them, outside any session's callback processor.
    guids.clear();
    for (auto const& [guid, entry] : _entries)
        if (entry.state == ManagedState::Loaded || entry.state == ManagedState::Failed)
            guids.push_back(guid);

    for (ObjectGuid const& guid : guids)
    {
        auto itr = _entries.find(guid);
        if (itr == _entries.end())
            continue;

        if (itr->second.state == ManagedState::Failed)
        {
            LOG_ERROR("module", "mod-guild-adventure: guild bot {} could not be loaded", guid.ToString());
            WorldSession* session = itr->second.homeSession;
            _entries.erase(itr);
            if (session->GetPlayer())
                session->LogoutPlayer(false);
            delete session;
            continue;
        }

        Player* bot = itr->second.homeSession->GetPlayer();
        Adopt(bot, itr->second);
        LOG_INFO("module", "mod-guild-adventure: guild bot {} is in the world", bot->GetName());
    }

    // 3. Callbacks, teleport acks and queued packets of the AI-driven bots (PlayerbotHolder::UpdateSessions).
    UpdateSessions();

    // 4. Watch mode: keep the watching clients' movement controls locked.
    UpdateWatchLocks();

    // 5. Placeholder AIs nobody needs any more.
    UpdatePlaceholders();
}
