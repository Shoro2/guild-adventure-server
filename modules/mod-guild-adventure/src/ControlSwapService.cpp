/*
 * ControlSwapService implementation. See ControlSwapService.h and docs/take-control.md §5.2 (Path R), §5.6
 * (logout, disconnect, shutdown) and §5.8 (Path W, experiment).
 */

#include "ControlSwapService.h"
#include "CharacterCache.h"
#include "Channel.h"
#include "CharacterPackets.h"
#include "Chat.h"
#include "ClientChannel.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GossipDef.h"
#include "Group.h"
#include "Guild.h"
#include "GuildBotHolder.h"
#include "GuildPackets.h"
#include "LFGMgr.h"
#include "Log.h"
#include "AccountMgr.h"
#include "AreaDefines.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "PetDefines.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <algorithm>
#include <cctype>

using namespace GuildAdventure;

namespace
{
    // How long each phase may take before the swap is given up.
    constexpr std::chrono::seconds PrepareTimeout{ 10 }; // database answers for the list row and account data
    constexpr std::chrono::minutes GlueTimeout{ 10 };    // the owner sits at the character screen
    constexpr std::chrono::seconds AttachTimeout{ 15 };  // the target finishes a teleport, account data arrives
    // Path W: the client's loading screen has no server-side limit (a stuck client is closed by its player, which
    // ends the swap); after this long a warning is logged once.
    constexpr std::chrono::seconds TransferWarnAfter{ 60 };

    // The 3.3.5a character screen shows at most ten characters (CharactersPerRealm default, client limit).
    constexpr uint8 MaxCharactersInList = 10;

    std::chrono::seconds Elapsed(std::chrono::steady_clock::time_point since)
    {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - since);
    }

    // Client spell-modifier table slots (SMSG_SET_FLAT_SPELL_MODIFIER / SMSG_SET_PCT_SPELL_MODIFIER: effect bit
    // 0..95 x modifier op), packed into one key.
    constexpr uint32 SpellModEffectBits = 96;

    uint32 SpellModKey(bool pct, uint32 opType, uint32 effectBit)
    {
        return (uint32(pct) << 16) | (opType << 8) | effectBit;
    }

    // The summed modifier values of 'player' per client slot, non-zero values only. The same sums the core sends
    // when a client re-attaches to a character in the world (WorldSession::HandlePlayerLoginToCharInWorld,
    // CharacterHandler.cpp:1216-1249).
    std::unordered_map<uint32, int32> CollectSpellModValues(Player const* player)
    {
        std::unordered_map<uint32, int32> values;
        for (uint32 opType = SPELLMOD_DAMAGE; opType < MAX_SPELLMOD; ++opType)
        {
            SpellModContainer const& mods = player->GetSpellModList(opType);
            if (mods.empty())
                continue;

            for (uint32 effectBit = 0; effectBit < SpellModEffectBits; ++effectBit)
            {
                uint32 const bit = 1u << (effectBit % 32);
                uint32 const word = effectBit / 32;
                flag96 const mask(word == 0 ? bit : 0, word == 1 ? bit : 0, word == 2 ? bit : 0);

                int32 flat = 0;
                int32 pct = 0;
                for (SpellModifier const* mod : mods)
                {
                    if (!(mod->mask & mask))
                        continue;

                    if (mod->type == SPELLMOD_FLAT)
                        flat += mod->value;
                    else if (mod->type == SPELLMOD_PCT)
                        pct += mod->value;
                }

                if (flat)
                    values[SpellModKey(false, opType, effectBit)] = flat;
                if (pct)
                    values[SpellModKey(true, opType, effectBit)] = pct;
            }
        }
        return values;
    }

    // Read access to Player::m_channels (protected, no public getter) without a core patch: a pointer to member
    // formed through a derived class may be applied to any Player ([class.protected]); the class is never
    // instantiated.
    struct PlayerChannelList : Player
    {
        static Player::JoinedChannelsList const& Of(Player const* player)
        {
            return player->*(&PlayerChannelList::m_channels);
        }
    };

    // SMSG_CHANNEL_NOTIFY "you joined" / "you left", laid out as Channel::MakeYouJoined / MakeYouLeft
    // (Channel.cpp:971-983; those builders are private).
    void SendChannelNotice(WorldSession* session, std::string const& name, uint32 channelId, uint8 flags, bool constant, bool joined)
    {
        WorldPacket data(SMSG_CHANNEL_NOTIFY, 1 + name.size() + 1 + 4 + 4);
        data << uint8(joined ? CHAT_YOU_JOINED_NOTICE : CHAT_YOU_LEFT_NOTICE);
        data << name;
        if (joined)
        {
            data << uint8(flags);
            data << uint32(channelId);
            data << uint32(0);
        }
        else
        {
            data << uint32(channelId);
            data << uint8(constant);
        }
        session->SendPacket(&data);
    }
}

char const* GuildAdventure::ToString(SwapPhase phase)
{
    switch (phase)
    {
        case SwapPhase::Requested: return "requested";
        case SwapPhase::Preparing: return "preparing";
        case SwapPhase::AtGlue:       return "at character screen";
        case SwapPhase::Attaching:    return "attaching";
        case SwapPhase::Transferring: return "loading the world";
    }
    return "?";
}

char const* GuildAdventure::ToString(SwapPath path)
{
    switch (path)
    {
        case SwapPath::Relog:  return "relog";
        case SwapPath::Reload: return "reload";
        case SwapPath::Switch: return "switch";
    }
    return "?";
}

char const* GuildAdventure::ToString(SwapPathMode mode)
{
    switch (mode)
    {
        case SwapPathMode::Relog:  return "relog";
        case SwapPathMode::Reload: return "reload";
        case SwapPathMode::Auto:   return "auto";
        case SwapPathMode::Switch: return "switch";
    }
    return "?";
}

std::optional<SwapPathMode> GuildAdventure::ParseSwapPathMode(std::string_view text)
{
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower == "relog")
        return SwapPathMode::Relog;
    if (lower == "reload")
        return SwapPathMode::Reload;
    if (lower == "auto")
        return SwapPathMode::Auto;
    if (lower == "switch")
        return SwapPathMode::Switch;
    return std::nullopt;
}

char const* GuildAdventure::ToString(SwapRefusal refusal)
{
    switch (refusal)
    {
        case SwapRefusal::None:         return "NONE";
        case SwapRefusal::Busy:         return "BUSY";
        case SwapRefusal::Same:         return "SAME";
        case SwapRefusal::NotInWorld:   return "NOT_IN_WORLD";
        case SwapRefusal::Offline:      return "OFFLINE";
        case SwapRefusal::NotOwner:     return "NOT_OWNER";
        case SwapRefusal::Loading:      return "LOADING";
        case SwapRefusal::Dead:         return "DEAD";
        case SwapRefusal::InCombat:     return "IN_COMBAT";
        case SwapRefusal::Flight:       return "FLIGHT";
        case SwapRefusal::Vehicle:      return "VEHICLE";
        case SwapRefusal::Charmed:      return "CHARMED";
        case SwapRefusal::ControlsUnit: return "CONTROLS_UNIT";
        case SwapRefusal::Duel:         return "DUEL";
        case SwapRefusal::Trade:        return "TRADE";
        case SwapRefusal::Battleground: return "BATTLEGROUND";
        case SwapRefusal::Lfg:          return "LFG";
        case SwapRefusal::GameMaster:   return "GM_MODE";
        case SwapRefusal::SelfBot:      return "SELF_BOT";
        case SwapRefusal::PathUnavailable: return "PATH_UNAVAILABLE";
        case SwapRefusal::NotGuildBot:  return "NOT_GUILD_BOT";
        case SwapRefusal::NoOrder:      return "NO_ORDER";
        case SwapRefusal::ShuttingDown: return "SHUTDOWN";
        case SwapRefusal::Cancelled:    return "CANCELLED";
        case SwapRefusal::Internal:     return "INTERNAL";
    }
    return "INTERNAL";
}

char const* GuildAdventure::ToString(SwapEventType type)
{
    switch (type)
    {
        case SwapEventType::Started:      return "started";
        case SwapEventType::LeavingWorld: return "leaving world";
        case SwapEventType::Completed:    return "completed";
        case SwapEventType::Failed:       return "failed";
        case SwapEventType::ReturnedToAI: return "returned to AI";
        case SwapEventType::WatchChanged: return "watch changed";
    }
    return "?";
}

namespace
{
    SwapRequestResult Refuse(SwapRefusal refusal, std::string message)
    {
        SwapRequestResult result;
        result.refusal = refusal;
        result.message = std::move(message);
        return result;
    }
}

ControlSwapService& ControlSwapService::Instance()
{
    static ControlSwapService instance;
    return instance;
}

void ControlSwapService::LoadConfig()
{
    _allowInCombat = sConfigMgr->GetOption<bool>("GuildAdventure.SwapAllowedInCombat", false);

    std::string const path = sConfigMgr->GetOption<std::string>("GuildAdventure.Swap.Path", "relog");
    if (std::optional<SwapPathMode> mode = ParseSwapPathMode(path))
        _defaultPathMode = *mode;
    else
    {
        LOG_ERROR("server.loading", "mod-guild-adventure: GuildAdventure.Swap.Path = \"{}\" is not relog, reload, switch or auto; using relog", path);
        _defaultPathMode = SwapPathMode::Relog;
    }

    _seamlessMaxDistance = sConfigMgr->GetOption<float>("GuildAdventure.Swap.SeamlessMaxDistance", 533.0f);
    if (_seamlessMaxDistance < 0.0f)
        _seamlessMaxDistance = 0.0f;
}

// ---------------------------------------------------------------------------------------------------------------
// Requests and checks
// ---------------------------------------------------------------------------------------------------------------

SwapRequestResult ControlSwapService::RequestSwap(WorldSession* owner, ObjectGuid target, std::optional<SwapPathMode> mode)
{
    if (_shuttingDown)
        return Refuse(SwapRefusal::ShuttingDown, "the server is shutting down");

    if (!owner || !owner->GetPlayer())
        return Refuse(SwapRefusal::NotInWorld, "you are not in the world");

    uint32 const account = owner->GetAccountId();
    if (auto itr = _swaps.find(account); itr != _swaps.end())
        return Refuse(SwapRefusal::Busy, Acore::StringFormat("a swap to {} is already in progress ({})", NameOf(itr->second.to),
            ToString(itr->second.phase)));

    Player* from = owner->GetPlayer();
    if (from->GetGUID() == target)
        return Refuse(SwapRefusal::Same, "you already play that character");

    if (!sGuildBotHolder.IsManaged(target))
        return Refuse(SwapRefusal::Offline, Acore::StringFormat("{} is not one of your guild bots in the world (.ga bot login {} first)",
            NameOf(target), NameOf(target)));

    if (sGuildBotHolder.GetOwner(target) != account)
        return Refuse(SwapRefusal::NotOwner, Acore::StringFormat("{} belongs to another guild owner", NameOf(target)));

    Player* to = sGuildBotHolder.GetAIDrivenPlayer(target);
    if (SwapRequestResult result = CanSwap(owner, from, to); !result)
        return result;

    SwapPathMode const pathMode = mode.value_or(_defaultPathMode);
    SwapRequestResult accepted = ResolvePath(owner, pathMode, from, to);
    if (!accepted)
        return accepted;

    Swap swap;
    swap.ownerAccountId = account;
    swap.from = from->GetGUID();
    swap.to = target;
    swap.toAccountId = sCharacterCache->GetCharacterAccountIdByGuid(target);
    swap.token = ++_nextToken;
    swap.phase = SwapPhase::Requested;
    swap.phaseSince = Clock::now();
    swap.mode = pathMode;
    swap.path = accepted.path;
    _swaps.emplace(account, std::move(swap));

    LOG_INFO("module", "mod-guild-adventure: account {} requests to switch from {} to {} (path {}, asked for {})", account,
        from->GetName(), NameOf(target), ToString(accepted.path), ToString(pathMode));

    // Watch mode ends with the swap: the character stops where it stands until the hand-over gives it to the AI on its
    // home session, so the swap's guards see what they checked here.
    EndWatch(account, from);

    SwapEvent event;
    event.type = SwapEventType::Started;
    event.ownerAccountId = account;
    event.from = from->GetGUID();
    event.to = target;
    event.path = accepted.path;
    Emit(std::move(event));
    return accepted;
}

std::optional<SwapPhase> ControlSwapService::GetSwapPhase(uint32 ownerAccountId) const
{
    auto itr = _swaps.find(ownerAccountId);
    if (itr == _swaps.end())
        return std::nullopt;
    return itr->second.phase;
}

void ControlSwapService::AddListener(Listener listener)
{
    if (listener)
        _listeners.push_back(std::move(listener));
}

SwapRequestResult ControlSwapService::CanSwap(WorldSession* owner, Player* from, Player* to) const
{
    if (!owner || !from || from->GetSession() != owner || !from->IsInWorld())
        return Refuse(SwapRefusal::NotInWorld, "you are not in the world");

    if (owner->PlayerLoading() || owner->IsLoggingOut())
        return Refuse(SwapRefusal::Loading, "you are logging in or out");

    // A GM-flagged character must not be handed to the AI (docs/take-control.md §5.1).
    if (from->IsGameMaster())
        return Refuse(SwapRefusal::GameMaster, "turn GM mode off first (.gm off)");

    if (!to)
        return Refuse(SwapRefusal::Offline, "the target is not driven by the AI right now");

    if (!to->IsInWorld())
        return Refuse(SwapRefusal::Loading, Acore::StringFormat("{} is not in the world (loading or changing maps)", to->GetName()));

    // A character playerbots itself drives ("self bot") or one that holds stock .playerbots altbots would lose
    // them: HandToAI replaces its playerbots master manager (GuildBotHolder.cpp). The watch-mode AI is the module's
    // own; the swap ends watch mode (RequestSwap).
    if (GET_PLAYERBOT_AI(from) && !sGuildBotHolder.IsWatched(from->GetGUID()) && !sGuildBotHolder.HasPlaceholder(from->GetGUID()))
        return Refuse(SwapRefusal::SelfBot, "your current character is driven by playerbots (self bot); turn that off first");

    if (PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(from))
        if (mgr->GetPlayerBotsBegin() != mgr->GetPlayerBotsEnd())
            return Refuse(SwapRefusal::SelfBot, "log out your .playerbots altbots first");

    if (SwapRequestResult result = CheckGuards(from, _allowInCombat); !result)
        return result;

    return CheckGuards(to, _allowInCombat);
}

SwapRequestResult ControlSwapService::CheckReloadPossible(Player* to)
{
    // Path W limits (docs/take-control.md §5.8). The world reload is a far teleport to the target's own spot:
    // - on an instanced map it would leave and re-enter the instance (instance scripts, binds, a possible new
    //   instance copy for newInstance = true);
    // - on a transport the destination is relative to the ship, which moves while the client loads;
    // - Player::TeleportTo refuses a death knight's far teleport out of Ebon Hold before the quest line is done.
    std::string const name = to->GetName();

    Map* map = to->FindMap();
    if (!map || map->Instanceable())
        return Refuse(SwapRefusal::PathUnavailable, Acore::StringFormat("{} is in an instance; only the relog path works there", name));

    if (to->GetTransport())
        return Refuse(SwapRefusal::PathUnavailable, Acore::StringFormat("{} is on a ship or zeppelin; only the relog path works there", name));

    if (to->GetMapId() == MAP_EBON_HOLD)
        return Refuse(SwapRefusal::PathUnavailable, Acore::StringFormat("{} is in Ebon Hold; only the relog path works there", name));

    return {};
}

SwapRequestResult ControlSwapService::CheckRelogPossible(WorldSession* owner, Player* to)
{
    // The character screen lists the owner's own characters plus the target (OnCharEnum); with a full list the
    // target cannot be shown, and the owner would be left at the character screen without it.
    if (to->GetCharacterAccountId() == owner->GetAccountId())
        return {};

    uint32 const own = AccountMgr::GetCharactersCount(owner->GetAccountId());
    if (own >= MaxCharactersInList)
        return Refuse(SwapRefusal::PathUnavailable, Acore::StringFormat("your account has {} characters, so the "
            "character screen has no room for {}; free a slot or take control where the world reload works", own,
            to->GetName()));

    return {};
}

SwapRequestResult ControlSwapService::CheckSwitchPossible(WorldSession* owner, Player* from, Player* to) const
{
    // Path S (docs/take-control.md §5.11) is Path W's world reload with the client DLL keeping the loaded map, so
    // Path W's limits apply first. The map stays loaded only around the old character: the target must stand on the
    // same map and close enough that the client has its terrain (Q2: one ADT tile, 533 yards).
    if (SwapRequestResult result = CheckReloadPossible(to); !result)
        return result;

    std::string const name = to->GetName();
    if (!sClientChannel.HasCapability(owner, CLIENT_CHANNEL_CAP_IN_WORLD_SWITCH | CLIENT_CHANNEL_CAP_PLAYER_RECORD))
        return Refuse(SwapRefusal::PathUnavailable, "your client has no client DLL with the in-world switch");

    if (!from || from->GetMapId() != to->GetMapId())
        return Refuse(SwapRefusal::PathUnavailable, Acore::StringFormat("{} is on another map", name));

    float const distance = from->GetExactDist(to);
    if (distance > _seamlessMaxDistance)
        return Refuse(SwapRefusal::PathUnavailable, Acore::StringFormat("{} is {:.0f} yards away (the in-world switch "
            "reaches {:.0f})", name, distance, _seamlessMaxDistance));

    return {};
}

SwapRequestResult ControlSwapService::ResolvePath(WorldSession* owner, SwapPathMode mode, Player* from, Player* to) const
{
    SwapRequestResult result;
    switch (mode)
    {
        case SwapPathMode::Relog:
            result = CheckRelogPossible(owner, to);
            result.path = SwapPath::Relog;
            break;
        case SwapPathMode::Reload:
            result = CheckReloadPossible(to);
            result.path = SwapPath::Reload;
            break;
        case SwapPathMode::Switch:
            result = CheckSwitchPossible(owner, from, to);
            result.path = SwapPath::Switch;
            break;
        case SwapPathMode::Auto:
            // Path S where it is possible. Without the DLL's character record the client would keep the old
            // character's name and class after Path W (docs/take-control.md §5.8, "T2 result"), so Auto takes
            // Path R then. An explicit Reload still runs (the experiment), with a warning in StartReload.
            if (CheckSwitchPossible(owner, from, to))
                result.path = SwapPath::Switch;
            else if (CheckReloadPossible(to) && sClientChannel.HasCapability(owner, CLIENT_CHANNEL_CAP_PLAYER_RECORD))
                result.path = SwapPath::Reload;
            else
            {
                result = CheckRelogPossible(owner, to);
                result.path = SwapPath::Relog;
            }
            break;
    }
    return result;
}

SwapRequestResult ControlSwapService::CheckGuards(Player* player, bool allowInCombat)
{
    // Swap guards, the design log §2.8 and docs/take-control.md §4.4
    std::string const name = player->GetName();

    if (player->IsBeingTeleported())
        return Refuse(SwapRefusal::Loading, Acore::StringFormat("{} is teleporting", name));

    if (!player->IsAlive() || player->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        return Refuse(SwapRefusal::Dead, Acore::StringFormat("{} is dead", name));

    if (!allowInCombat && player->IsInCombat())
        return Refuse(SwapRefusal::InCombat, Acore::StringFormat("{} is in combat", name));

    if (player->IsInFlight())
        return Refuse(SwapRefusal::Flight, Acore::StringFormat("{} is on a flight path", name));

    if (player->GetVehicle())
        return Refuse(SwapRefusal::Vehicle, Acore::StringFormat("{} is on a vehicle", name));

    if (!player->GetCharmerGUID().IsEmpty())
        return Refuse(SwapRefusal::Charmed, Acore::StringFormat("{} is charmed", name));

    if (!player->GetCharmGUID().IsEmpty() || player->m_mover != player)
        return Refuse(SwapRefusal::ControlsUnit, Acore::StringFormat("{} controls another unit", name));

    if (player->duel)
        return Refuse(SwapRefusal::Duel, Acore::StringFormat("{} is in a duel", name));

    if (player->GetTradeData())
        return Refuse(SwapRefusal::Trade, Acore::StringFormat("{} is trading", name));

    if (Map* map = player->FindMap(); !map || map->IsBattlegroundOrArena())
        return Refuse(SwapRefusal::Battleground, Acore::StringFormat("{} is in a battleground or arena", name));

    if (player->InBattlegroundQueue())
        return Refuse(SwapRefusal::Battleground, Acore::StringFormat("{} is queued for a battleground or arena", name));

    // The dungeon finder keys its queue, proposals and kick votes by player GUID and talks to the client of that
    // player; a swap in the middle would leave the old client's dialogs dangling and hide them from the new one.
    // Inside an LFG dungeon (LFG_STATE_DUNGEON, LFG_STATE_FINISHED_DUNGEON) the swap is allowed.
    switch (sLFGMgr->GetState(player->GetGUID()))
    {
        case lfg::LFG_STATE_ROLECHECK:
        case lfg::LFG_STATE_QUEUED:
        case lfg::LFG_STATE_PROPOSAL:
        case lfg::LFG_STATE_BOOT:
            return Refuse(SwapRefusal::Lfg, Acore::StringFormat("{} is in the dungeon finder (queue, role check, proposal or vote)", name));
        default:
            break;
    }

    return {};
}

// ---------------------------------------------------------------------------------------------------------------
// Watch mode (docs/take-control.md §5.12)
// ---------------------------------------------------------------------------------------------------------------

SwapRequestResult ControlSwapService::RequestWatch(WorldSession* owner, bool on)
{
    if (!owner || !owner->GetPlayer() || !owner->GetPlayer()->IsInWorld())
        return Refuse(SwapRefusal::NotInWorld, "you are not in the world");

    Player* player = owner->GetPlayer();
    uint32 const account = owner->GetAccountId();
    ObjectGuid const guid = player->GetGUID();

    // Taking the controls back is always possible and needs no checks.
    if (!on)
    {
        EndWatch(account, player);
        return {};
    }

    if (sGuildBotHolder.IsWatched(guid))
        return {};

    if (_shuttingDown)
        return Refuse(SwapRefusal::ShuttingDown, "the server is shutting down");

    if (auto itr = _swaps.find(account); itr != _swaps.end())
        return Refuse(SwapRefusal::Busy, Acore::StringFormat("a swap to {} is in progress ({})", NameOf(itr->second.to),
            ToString(itr->second.phase)));

    // A character of the owner's guild with an order for the AI to play: a guild bot the owner controls now, or the
    // leader once it has an order (docs/orders.md "The leader"). A leader the owner logged into from the character
    // screen is no managed character yet; StartWatch makes it one. A character logged in with ".ga bot login" has no
    // order.
    bool const own = sGuildBotHolder.IsManaged(guid) ? sGuildBotHolder.GetOwner(guid) == account && !sGuildBotHolder.IsAIDriven(guid)
        : player->GetCharacterAccountId() == account;
    SwapRefusal const policy = !own ? SwapRefusal::NotGuildBot :
        _watchPolicy ? _watchPolicy(guid) : (player->GetCharacterAccountId() != account ? SwapRefusal::None : SwapRefusal::NoOrder);
    if (policy == SwapRefusal::NoOrder)
        return Refuse(SwapRefusal::NoOrder, Acore::StringFormat("{} has no order: give your leader an order first (Overview tab "
            "or .ga bot order {} AUTO); without one it only holds its position", player->GetName(), player->GetName()));
    if (policy != SwapRefusal::None)
        return Refuse(SwapRefusal::NotGuildBot, Acore::StringFormat("{} is not one of your guild's characters with an order; "
            "take control of a guild bot first, then let the AI play it", player->GetName()));

    if (owner->PlayerLoading() || owner->IsLoggingOut())
        return Refuse(SwapRefusal::Loading, "you are logging in or out");

    // (A placeholder AI, GuildBotHolder::StartPlaceholder, is the module's own: StartWatch replaces it.)
    if (GET_PLAYERBOT_AI(player) && !sGuildBotHolder.HasPlaceholder(guid))
        return Refuse(SwapRefusal::SelfBot, "your character is driven by playerbots (self bot); turn that off first");

    if (PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(player))
        if (mgr->GetPlayerBotsBegin() != mgr->GetPlayerBotsEnd())
            return Refuse(SwapRefusal::SelfBot, "log out your .playerbots altbots first");

    if (SwapRequestResult result = CheckWatchGuards(player); !result)
        return result;

    if (!sGuildBotHolder.StartWatch(player))
        return Refuse(SwapRefusal::Internal, "watch mode could not be started (see the server log)");

    SwapEvent event;
    event.type = SwapEventType::WatchChanged;
    event.ownerAccountId = account;
    event.from = guid;
    event.watching = true;
    EmitNow(std::move(event));
    return {};
}

void ControlSwapService::EndWatchOf(ObjectGuid guid)
{
    if (!sGuildBotHolder.IsWatched(guid))
        return;
    Player* player = ObjectAccessor::FindConnectedPlayer(guid);
    WorldSession* session = player ? player->GetSession() : nullptr;
    if (session)
        EndWatch(session->GetAccountId(), player);
}

void ControlSwapService::EndWatch(uint32 ownerAccountId, Player* player)
{
    if (!player || !sGuildBotHolder.StopWatch(player, true))
        return;

    SwapEvent event;
    event.type = SwapEventType::WatchChanged;
    event.ownerAccountId = ownerAccountId;
    event.from = player->GetGUID();
    event.watching = false;
    EmitNow(std::move(event));
}

SwapRequestResult ControlSwapService::CheckWatchGuards(Player* player)
{
    std::string const name = player->GetName();

    if (player->IsBeingTeleported())
        return Refuse(SwapRefusal::Loading, Acore::StringFormat("{} is teleporting", name));

    // A GM-flagged character must not be handed to the AI (as for a swap, docs/take-control.md §5.1).
    if (player->IsGameMaster())
        return Refuse(SwapRefusal::GameMaster, "turn GM mode off first (.gm off)");

    if (player->GetVehicle())
        return Refuse(SwapRefusal::Vehicle, Acore::StringFormat("{} is on a vehicle", name));

    if (!player->GetCharmerGUID().IsEmpty())
        return Refuse(SwapRefusal::Charmed, Acore::StringFormat("{} is charmed", name));

    if (!player->GetCharmGUID().IsEmpty() || player->m_mover != player)
        return Refuse(SwapRefusal::ControlsUnit, Acore::StringFormat("{} controls another unit", name));

    if (player->duel)
        return Refuse(SwapRefusal::Duel, Acore::StringFormat("{} is in a duel", name));

    if (player->GetTradeData())
        return Refuse(SwapRefusal::Trade, Acore::StringFormat("{} is trading", name));

    if (Map* map = player->FindMap(); !map || map->IsBattlegroundOrArena() || player->InBattlegroundQueue())
        return Refuse(SwapRefusal::Battleground, Acore::StringFormat("{} is in or queued for a battleground or arena", name));

    switch (sLFGMgr->GetState(player->GetGUID()))
    {
        case lfg::LFG_STATE_ROLECHECK:
        case lfg::LFG_STATE_QUEUED:
        case lfg::LFG_STATE_PROPOSAL:
        case lfg::LFG_STATE_BOOT:
            return Refuse(SwapRefusal::Lfg, Acore::StringFormat("{} is in the dungeon finder (queue, role check, proposal or vote)", name));
        default:
            break;
    }

    return {};
}

// ---------------------------------------------------------------------------------------------------------------
// World-thread driver
// ---------------------------------------------------------------------------------------------------------------

void ControlSwapService::Update()
{
    if (_shuttingDown)
        return;

    _queryProcessor.ProcessReadyCallbacks();

    HandBackFromClosedConnections();

    std::vector<uint32> owners;
    owners.reserve(_swaps.size());
    for (auto const& [account, swap] : _swaps)
        owners.push_back(account);

    for (uint32 account : owners)
    {
        auto itr = _swaps.find(account);
        if (itr == _swaps.end())
            continue;

        Swap& swap = itr->second;
        WorldSession* owner = sWorldSessionMgr->FindSession(account);

        bool keep = false;
        if (!owner)
        {
            // The client closed or lost its connection. Before AtGlue the old character is still on the owner's
            // session and the core handles it (offline session, CP-1 on its logout); from AtGlue on the owner
            // has no player any more, so the leader logs out (D12, docs/take-control.md §5.2). In Transferring
            // the target is out of the world, so the core does not park the session on its offline list
            // (WorldSession::HandleSocketClosed needs a player in the world); it logs the session out, which
            // finishes the world reload first (WorldSession::LogoutPlayer) and then runs CP-1: OnLogoutDetach
            // has handed the target back to the AI already (case 2, which also takes care of D12).
            LOG_INFO("module", "mod-guild-adventure: swap of account {} to {} cancelled, the client is gone ({})",
                account, NameOf(swap.to), ToString(swap.phase));
            if (swap.phase == SwapPhase::AtGlue || swap.phase == SwapPhase::Attaching)
                QueueLeaderLogout(account);
            Fail(nullptr, swap, SwapRefusal::Cancelled, "the client closed or lost its connection");
        }
        else
        {
            switch (swap.phase)
            {
                case SwapPhase::Requested: keep = AdvanceRequested(owner, swap); break;
                case SwapPhase::Preparing: keep = AdvancePreparing(owner, swap); break;
                case SwapPhase::AtGlue:       keep = AdvanceAtGlue(owner, swap);       break;
                case SwapPhase::Attaching:    keep = AdvanceAttaching(owner, swap);    break;
                case SwapPhase::Transferring: keep = AdvanceTransferring(owner, swap); break;
            }
        }

        if (!keep)
            _swaps.erase(account);
    }

    ProcessWatchOnLogin();
    UpdateArrivals();
    ProcessLeaderLogouts();
    FlushEvents();
}

void ControlSwapService::QueueWatchOnLogin(uint32 ownerAccountId, ObjectGuid guid)
{
    PendingWatch& pending = _watchOnLogin[ownerAccountId];
    pending.guid = guid;
    pending.since = Clock::now();
}

void ControlSwapService::ProcessWatchOnLogin()
{
    constexpr std::chrono::seconds WatchOnLoginTimeout{ 60 }; // the login or the attach did not end in the world in time

    for (auto itr = _watchOnLogin.begin(); itr != _watchOnLogin.end();)
    {
        uint32 const account = itr->first;
        PendingWatch const& pending = itr->second;
        WorldSession* session = sWorldSessionMgr->FindSession(account);
        Player* player = session ? session->GetPlayer() : nullptr;

        bool const ready = player && player->GetGUID() == pending.guid && player->IsInWorld() && !session->PlayerLoading() &&
            !player->IsBeingTeleported() && _swaps.find(account) == _swaps.end();
        if (!ready)
        {
            // Still logging in or attaching; given up when the owner plays another character or after the timeout.
            bool const other = player && player->GetGUID() != pending.guid && player->IsInWorld();
            if (other || !session || Elapsed(pending.since) > WatchOnLoginTimeout)
            {
                LOG_INFO("module", "mod-guild-adventure: account {} did not come into the world as {}, its auto mode is not "
                    "kept on", account, NameOf(pending.guid));
                itr = _watchOnLogin.erase(itr);
            }
            else
                ++itr;
            continue;
        }

        SwapRequestResult const result = RequestWatch(session, true);
        if (result)
        {
            LOG_INFO("module", "mod-guild-adventure: account {} logged into {}, its auto mode goes on (watch mode)", account,
                player->GetName());
            Notify(session, Acore::StringFormat("{} keeps playing its order while you watch. Press Take over on the "
                "control bar (or type /ga watch) to play it yourself.", player->GetName()));
        }
        else
        {
            LOG_INFO("module", "mod-guild-adventure: account {} logged into {}, its auto mode could not go on: {} ({})",
                account, player->GetName(), result.message, ToString(result.refusal));
            Notify(session, Acore::StringFormat("{}'s auto mode stopped at your login: {}. Press Watch on the control bar to "
                "hand it to the AI again.", player->GetName(), result.message));
        }
        itr = _watchOnLogin.erase(itr);
    }
}

namespace
{
    // How long a character the owner's client took over is watched, and how often its position is looked at.
    constexpr std::chrono::seconds ArrivalWatchTime{ 25 };
    constexpr std::chrono::milliseconds ArrivalSampleEvery{ 500 };

    // Where 'player' stands against the ground: the floor under it (vmaps and terrain, searched from 2 yd above it),
    // the highest surface at its spot, the terrain height there, and its movement state.
    std::string DescribeGround(Player* player)
    {
        Map* map = player->GetMap();
        float const x = player->GetPositionX();
        float const y = player->GetPositionY();
        float const z = player->GetPositionZ();
        float const floor = map->GetHeight(player->GetPhaseMask(), x, y, z + 2.0f, true, 100.0f);
        float const surface = map->GetHeight(player->GetPhaseMask(), x, y, MAX_HEIGHT, true, MAX_HEIGHT);
        float const terrain = map->GetGridHeight(x, y);
        std::string const floorText = floor > INVALID_HEIGHT ? Acore::StringFormat("{:.2f}", floor) : std::string("none");
        return Acore::StringFormat("map {} at ({:.2f}, {:.2f}, {:.2f}) o {:.3f}; floor {}, surface {:.2f}, terrain {:.2f}; "
            "movement flags 0x{:08X} / 0x{:04X}, fall time {}, spline {}, {}{}{}", player->GetMapId(), x, y, z,
            player->GetOrientation(), floorText, surface, terrain, player->GetUnitMovementFlags(),
            player->GetExtraUnitMovementFlags(), player->m_movementInfo.fallTime,
            player->movespline->Finalized() ? "finalized" : "running", player->IsMounted() ? "mounted, " : "",
            player->GetTransport() ? "on a transport, " : "", player->IsAlive() ? "alive" : "dead");
    }
}

void ControlSwapService::WatchArrival(uint32 ownerAccountId, Player* player, char const* how)
{
    LOG_INFO("module", "mod-guild-adventure: account {} takes over {} ({}): {}", ownerAccountId, player->GetName(), how,
        DescribeGround(player));

    _arrivals.erase(std::remove_if(_arrivals.begin(), _arrivals.end(),
        [ownerAccountId](Arrival const& arrival) { return arrival.ownerAccountId == ownerAccountId; }), _arrivals.end());
    Arrival arrival;
    arrival.ownerAccountId = ownerAccountId;
    arrival.guid = player->GetGUID();
    arrival.how = how;
    _arrivals.push_back(std::move(arrival));
}

void ControlSwapService::UpdateArrivals()
{
    if (_arrivals.empty())
        return;

    Clock::time_point const now = Clock::now();
    for (auto itr = _arrivals.begin(); itr != _arrivals.end();)
    {
        Arrival& arrival = *itr;
        WorldSession* session = sWorldSessionMgr->FindSession(arrival.ownerAccountId);
        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || player->GetGUID() != arrival.guid || now - arrival.since > ArrivalWatchTime)
        {
            if (player && player->GetGUID() == arrival.guid && player->IsInWorld())
                LOG_INFO("module", "mod-guild-adventure: {} {} s after the take-over ({}): {}", player->GetName(),
                    ArrivalWatchTime.count(), arrival.how, DescribeGround(player));
            itr = _arrivals.erase(itr);
            continue;
        }

        if (now < arrival.nextSample || !player->IsInWorld() || player->IsBeingTeleported())
        {
            ++itr;
            continue;
        }
        arrival.nextSample = now + ArrivalSampleEvery;

        // Falling, or below the surface with no floor under it: the client fell under the world.
        float const floor = player->GetMap()->GetHeight(player->GetPhaseMask(), player->GetPositionX(),
            player->GetPositionY(), player->GetPositionZ() + 2.0f, true, 100.0f);
        float const surface = player->GetMap()->GetHeight(player->GetPhaseMask(), player->GetPositionX(),
            player->GetPositionY(), MAX_HEIGHT, true, MAX_HEIGHT);
        bool const under = surface > INVALID_HEIGHT && player->GetPositionZ() < surface - 3.0f &&
            (floor <= INVALID_HEIGHT || player->GetPositionZ() < floor - 3.0f);
        bool const longFall = player->IsFalling() && player->m_movementInfo.fallTime > 3000; // a jump takes < 1 s
        if (under || longFall || !player->IsAlive())
        {
            LOG_INFO("module", "mod-guild-adventure: {} {} ms after the take-over ({}){}: {}", player->GetName(),
                std::chrono::duration_cast<std::chrono::milliseconds>(now - arrival.since).count(), arrival.how,
                under ? ", under the ground" : "", DescribeGround(player));
            if (!player->IsAlive())
            {
                itr = _arrivals.erase(itr);
                continue;
            }
        }
        ++itr;
    }
}

bool ControlSwapService::AdvanceRequested(WorldSession* owner, Swap& swap)
{
    Player* from = owner->GetPlayer();
    Player* to = sGuildBotHolder.GetAIDrivenPlayer(swap.to);

    if (!from || from->GetGUID() != swap.from)
    {
        Fail(owner, swap, SwapRefusal::Cancelled, "your character changed");
        return false;
    }

    if (SwapRequestResult result = CanSwap(owner, from, to); !result)
    {
        Fail(owner, swap, result.refusal, result.message);
        return false;
    }

    // The target may have entered an instance, boarded a ship or walked off since the request (one tick ago).
    if (swap.path == SwapPath::Switch)
    {
        if (SwapRequestResult result = CheckSwitchPossible(owner, from, to); !result)
        {
            if (swap.mode != SwapPathMode::Auto)
            {
                Fail(owner, swap, result.refusal, result.message);
                return false;
            }

            SwapRequestResult const fallback = ResolvePath(owner, SwapPathMode::Auto, from, to);
            if (!fallback)
            {
                Fail(owner, swap, fallback.refusal, fallback.message);
                return false;
            }
            swap.path = fallback.path;
            LOG_INFO("module", "mod-guild-adventure: swap of account {} to {} falls back to the {} path: {}",
                swap.ownerAccountId, to->GetName(), ToString(swap.path), result.message);
        }
    }

    if (swap.path == SwapPath::Reload)
    {
        if (SwapRequestResult result = CheckReloadPossible(to); !result)
        {
            if (swap.mode != SwapPathMode::Auto)
            {
                Fail(owner, swap, result.refusal, result.message);
                return false;
            }

            if (SwapRequestResult const relog = CheckRelogPossible(owner, to); !relog)
            {
                Fail(owner, swap, relog.refusal, relog.message);
                return false;
            }
            LOG_INFO("module", "mod-guild-adventure: swap of account {} to {} falls back to the relog path: {}",
                swap.ownerAccountId, to->GetName(), result.message);
            swap.path = SwapPath::Relog;
        }
    }

    // Cheap insurance: a crash during the swap then loses nothing (docs/take-control.md §5.1 "Persistence").
    from->SaveToDB(false, false);
    to->SaveToDB(false, false);

    if (swap.path == SwapPath::Reload || swap.path == SwapPath::Switch)
        return StartReload(owner, swap);

    StartEnumQuery(swap);
    StartAccountDataQuery(swap);
    SetPhase(swap, SwapPhase::Preparing);

    Notify(owner, Acore::StringFormat("Switching to {}...", to->GetName()));
    return true;
}

bool ControlSwapService::AdvancePreparing(WorldSession* owner, Swap& swap)
{
    if (!swap.enumReady || !swap.accountDataReady)
    {
        if (Elapsed(swap.phaseSince) <= PrepareTimeout)
            return true;

        Fail(owner, swap, SwapRefusal::Internal, "the database did not answer in time");
        LOG_ERROR("module", "mod-guild-adventure: swap of account {} to {} timed out while preparing (list row {}, account data {})",
            swap.ownerAccountId, NameOf(swap.to), swap.enumReady, swap.accountDataReady);
        return false;
    }

    Player* from = owner->GetPlayer();
    Player* to = sGuildBotHolder.GetAIDrivenPlayer(swap.to);

    if (!from || from->GetGUID() != swap.from)
    {
        Fail(owner, swap, SwapRefusal::Cancelled, "your character changed");
        return false;
    }

    if (SwapRequestResult result = CanSwap(owner, from, to); !result)
    {
        Fail(owner, swap, result.refusal, result.message);
        return false;
    }

    if (swap.toAccountId != swap.ownerAccountId && swap.enumRow.empty())
    {
        Fail(owner, swap, SwapRefusal::Internal, Acore::StringFormat("the character screen entry for {} could not be built", to->GetName()));
        return false;
    }

    // Quiesce the old character (docs/take-control.md §4.4): stop what the client started, close its windows.
    // HandToAI (in OnLogoutDetach) does it again; here it happens before the client leaves the world.
    GuildBotHolder::Quiesce(from, owner);

    // Last chance for the protocol layer to tell the owner's addon which character comes next (GlueXML
    // auto-enter); the UI unloads with the logout below.
    SwapEvent leaving;
    leaving.type = SwapEventType::LeavingWorld;
    leaving.ownerAccountId = swap.ownerAccountId;
    leaving.from = swap.from;
    leaving.to = swap.to;
    leaving.path = swap.path;
    EmitNow(std::move(leaving));

    // Path R step 2: the client gets the stock logout handshake and returns to the character screen. LogoutPlayer
    // runs core patch CP-1, which lands in OnLogoutDetach below and hands the character to the AI instead of
    // removing it from the world. If that hand-over fails, the character logs out normally (saved, nothing lost).
    WorldPackets::Character::LogoutResponse response;
    response.LogoutResult = 0;
    response.Instant = true;
    owner->SendPacket(response.Write());

    owner->LogoutPlayer(true);

    if (swap.phase != SwapPhase::AtGlue || owner->GetPlayer())
    {
        LOG_ERROR("module", "mod-guild-adventure: swap of account {} to {}: the old character was not handed to the AI, it logged out",
            swap.ownerAccountId, NameOf(swap.to));
        Fail(owner, swap, SwapRefusal::Internal, "the previous character could not be handed to the AI and logged out");
        return false;
    }

    LOG_INFO("module", "mod-guild-adventure: account {} is at the character screen, {} waits for the login",
        swap.ownerAccountId, NameOf(swap.to));
    return true;
}

bool ControlSwapService::AdvanceAtGlue(WorldSession* owner, Swap& swap)
{
    if (owner->GetPlayer() || owner->PlayerLoading())
    {
        // The owner entered the world another way; OnPlayerLoginPacket normally cancels first.
        Fail(owner, swap, SwapRefusal::Cancelled, "you entered the world with another character");
        return false;
    }

    if (swap.listFull)
    {
        // The target could not be added to the character screen (CheckRelogPossible refuses this up front; a
        // character created in between gets here). The owner can still pick any listed character, the previous
        // one included: it is attached like any AI-driven guild character.
        Fail(owner, swap, SwapRefusal::PathUnavailable, Acore::StringFormat("the character screen has no room for {}",
            NameOf(swap.to)));
        return false;
    }

    if (Elapsed(swap.phaseSince) > GlueTimeout)
    {
        LOG_INFO("module", "mod-guild-adventure: account {} stayed at the character screen, swap to {} given up",
            swap.ownerAccountId, NameOf(swap.to));
        QueueLeaderLogout(swap.ownerAccountId);
        Fail(owner, swap, SwapRefusal::Cancelled, "no character was picked on the character screen in time");
        return false;
    }

    return true;
}

bool ControlSwapService::AdvanceAttaching(WorldSession* owner, Swap& swap)
{
    if (owner->GetPlayer() || owner->PlayerLoading())
    {
        LOG_ERROR("module", "mod-guild-adventure: account {} already has a character, attach of {} dropped",
            swap.ownerAccountId, NameOf(swap.to));
        Fail(owner, swap, SwapRefusal::Internal, "your session already has a character");
        return false;
    }

    Player* to = sGuildBotHolder.GetAIDrivenPlayer(swap.to);
    bool const targetReady = to && to->IsInWorld() && !to->IsBeingTeleported();

    if (!swap.accountDataReady || !targetReady)
    {
        if (!sGuildBotHolder.IsAIDriven(swap.to) && !sGuildBotHolder.IsLoggingIn(swap.to))
        {
            owner->SendCharLoginFailed(LoginFailureReason::NoCharacter);
            Fail(owner, swap, SwapRefusal::Offline, Acore::StringFormat("{} left the world", NameOf(swap.to)));
            return false;
        }

        if (Elapsed(swap.phaseSince) <= AttachTimeout)
            return true;

        LOG_ERROR("module", "mod-guild-adventure: attach of {} to account {} timed out (account data {}, target ready {})",
            NameOf(swap.to), swap.ownerAccountId, swap.accountDataReady, targetReady);
        owner->SendCharLoginFailed(LoginFailureReason::Failed);
        Fail(owner, swap, SwapRefusal::Internal, "the target did not become ready in time");
        return false;
    }

    if (!Attach(owner, swap))
    {
        owner->SendCharLoginFailed(LoginFailureReason::Failed);
        Fail(owner, swap, SwapRefusal::Internal, "the target could not be attached");
    }

    return false;
}

bool ControlSwapService::Attach(WorldSession* owner, Swap& swap)
{
    Player* target = sGuildBotHolder.GetAIDrivenPlayer(swap.to);
    if (!target || !sGuildBotHolder.TakeFromAI(target, owner))
    {
        LOG_ERROR("module", "mod-guild-adventure: {} could not be taken from the AI for account {}", NameOf(swap.to), swap.ownerAccountId);
        return false;
    }

    // Per-character account data (UI layout, macros, key bindings of this character) replaces the cache of the
    // previous character; the reconnect path sends its times to the client (WorldSession.h:602, §5.1).
    owner->LoadAccountData(swap.accountData, PER_CHARACTER_CACHE_MASK);
    // The client takes the position the server holds: never under the ground (docs/take-control.md §5.13).
    if (std::string const settled = GuildBotHolder::SettleForClient(target); !settled.empty())
        LOG_INFO("module", "mod-guild-adventure: {} before account {} takes it over: {}", target->GetName(),
            swap.ownerAccountId, settled);
    WatchArrival(swap.ownerAccountId, target, "relog");
    owner->HandlePlayerLoginToCharInWorld(target);

    // The reconnect path assumes the client knew this character's guild already; the database login sends the
    // MOTD and the roster (Guild::SendLoginInfo, CharacterHandler.cpp:847-858). Send both, but not the "signed
    // on" broadcast: the character never left the world.
    if (Guild* guild = target->GetGuild())
    {
        WorldPackets::Guild::GuildEvent motd;
        motd.Type = GE_MOTD;
        motd.Params.emplace_back(guild->GetMOTD());
        owner->SendPacket(motd.Write());
        guild->HandleRoster(owner);
    }

    // Every other guild bot of this owner now answers to the character the owner plays.
    sGuildBotHolder.SetMasterForOwner(swap.ownerAccountId, target);

    Notify(owner, Acore::StringFormat("You now control {}. Your previous character keeps playing.", target->GetName()));
    LOG_INFO("module", "mod-guild-adventure: account {} now controls {}", swap.ownerAccountId, target->GetName());

    SwapEvent event;
    event.type = SwapEventType::Completed;
    event.ownerAccountId = swap.ownerAccountId;
    event.from = swap.from;
    event.to = swap.to;
    event.path = SwapPath::Relog;
    Emit(std::move(event));
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Path W: world reload (experiment, docs/take-control.md §5.8), and Path S: the same reload with the client DLL
// keeping the loaded map (in-world switch, §5.11)
// ---------------------------------------------------------------------------------------------------------------

bool ControlSwapService::StartReload(WorldSession* owner, Swap& swap)
{
    Player* from = owner->GetPlayer();
    Player* to = sGuildBotHolder.GetAIDrivenPlayer(swap.to);
    uint32 const account = swap.ownerAccountId;

    // What the client of the old character shows and the world reload does not replace; sent right below
    // (channels) and in FinishReload (spell modifiers, group, pet bar, guild).
    swap.reload = CaptureClientState(from);
    std::vector<ReloadState::ChannelInfo> const targetChannels = JoinedChannels(to);

    // Quiesce the old character as Path R does (docs/take-control.md §4.4); HandToAI below does it as well.
    GuildBotHolder::Quiesce(from, owner);

    swap.reloadStartedAt = Clock::now();
    bool const inWorld = swap.path == SwapPath::Switch;
    Notify(owner, Acore::StringFormat(inWorld ? "Switching to {}..." : "Switching to {} (loading screen)...",
        to->GetName()));

    // Without a client DLL that rewrites it, the client's record (UnitName / UnitClass("player")) stays the one of the
    // character it logged in with: the UI character of an earlier Path W swap, or else the current character.
    ObjectGuid recordCharacter = from->GetGUID();
    if (auto itr = _clientUiCharacter.find(account); itr != _clientUiCharacter.end())
        recordCharacter = itr->second;

    // 1. The old character goes to the AI with the target as its master (as in Path R), the target comes to the
    //    owner's session. The order matters: while the old character is adopted the target still sits on its
    //    headless home session, so nothing the AI sends to its master reaches the owner's client.
    if (!sGuildBotHolder.HandToAI(from, owner, account, to))
    {
        Fail(owner, swap, SwapRefusal::Internal, "the current character could not be handed to the AI");
        return false;
    }

    if (!sGuildBotHolder.TakeFromAI(to, owner))
    {
        LOG_ERROR("module", "mod-guild-adventure: Path W: {} could not be taken from the AI for account {}, rolling back",
            to->GetName(), account);
        if (!sGuildBotHolder.TakeFromAI(from, owner))
        {
            // Nothing reached the client yet, but the session has no character any more. Disconnecting is the
            // safe end state: the client logs in again, both characters stay in the world as bots.
            LOG_ERROR("module", "mod-guild-adventure: Path W rollback failed for account {}, disconnecting the client", account);
            owner->KickPlayer("mod-guild-adventure: Path W rollback failed");
            QueueLeaderLogout(account); // D12: the owner is gone
        }
        else
            sGuildBotHolder.SetMasterForOwner(account, from);

        Fail(owner, swap, SwapRefusal::Internal, "the target could not be taken from the AI");
        return false;
    }

    // 2. The client's record of the character it logged in with (UnitName / UnitClass("player")) is not replaced by
    //    the world reload (docs/take-control.md §5.8, "T2 result"); a client DLL with the character record rewrites
    //    it. Packets reach the client in order, so the record is written before the reload starts.
    bool const recordSent = SendCharacterRecord(owner, to);
    bool const recordStale = !recordSent && recordCharacter != to->GetGUID(); // back to the login character: fine
    if (recordStale)
        Notify(owner, Acore::StringFormat("Your client has no client DLL that updates the character record: until your "
            "next login it shows {}'s name and class.", NameOf(recordCharacter)));

    // 2b. Path S: the DLL is to keep the loaded map during the reload below (docs/take-control.md §5.11). It answers
    //     SWITCH_ACK before it processes the reload's packets; if it cannot (another map loaded, an older DLL), the
    //     reload simply runs as Path W with a loading screen. FinishReload reports which one it was.
    swap.switchNonce = 0;
    if (inWorld && sClientChannel.SendSwitch(owner, to->GetMapId(), swap.switchNonce) != SwitchSendResult::Sent)
    {
        LOG_WARN("module", "mod-guild-adventure: account {}: the client DLL took no SWITCH; {} comes with a loading "
            "screen", account, to->GetName());
        swap.path = SwapPath::Reload;
        swap.switchNonce = 0;
    }

    // 3. The world reload: a far teleport to the spot the target stands on (newInstance = true forces the far
    //    branch for the same map, Player::TeleportTo). The core sends SMSG_TRANSFER_PENDING and SMSG_NEW_WORLD to
    //    the owner's client and removes the target from the map; the client tears its world down, loads it again
    //    and acknowledges with MSG_MOVE_WORLDPORT_ACK. The core's handler (WorldSession::HandleMoveWorldportAck)
    //    then sends the target's login data and adds it to the map, which sends the self create block the client
    //    takes as its new active player. Its destination is never under the ground (docs/take-control.md §5.13).
    if (std::string const settled = GuildBotHolder::SettleForClient(to); !settled.empty())
        LOG_INFO("module", "mod-guild-adventure: {} before account {} takes it over: {}", to->GetName(), account, settled);
    WatchArrival(account, to, inWorld ? "switch" : "reload");
    if (!to->TeleportTo(to->GetMapId(), to->GetPositionX(), to->GetPositionY(), to->GetPositionZ(), to->GetOrientation(), 0, nullptr, true))
    {
        LOG_ERROR("module", "mod-guild-adventure: Path W: the world reload of {} was refused, rolling back", to->GetName());

        bool const backToAI = sGuildBotHolder.HandToAI(to, owner, account, nullptr);
        bool const backToOwner = backToAI && sGuildBotHolder.TakeFromAI(from, owner);
        if (backToOwner)
        {
            sGuildBotHolder.SetMasterForOwner(account, from);
            if (recordSent)
                SendCharacterRecord(owner, from); // the client plays the old character again
            Fail(owner, swap, SwapRefusal::Internal, "the server refused the world reload");
        }
        else
        {
            LOG_ERROR("module", "mod-guild-adventure: Path W rollback failed for account {} (back to AI {}), disconnecting the client",
                account, backToAI);
            owner->KickPlayer("mod-guild-adventure: Path W rollback failed");
            QueueLeaderLogout(account); // D12: the owner is gone
            Fail(nullptr, swap, SwapRefusal::Internal, "the server refused the world reload");
        }
        return false;
    }

    // The client keeps its chat channel list across the reload; leave the channels only the old character is on.
    for (ReloadState::ChannelInfo const& channel : swap.reload.channels)
    {
        bool const shared = std::any_of(targetChannels.begin(), targetChannels.end(),
            [&channel](ReloadState::ChannelInfo const& other) { return other.name == channel.name; });
        if (!shared)
            SendChannelNotice(owner, channel.name, channel.channelId, channel.flags, channel.constant, false);
    }

    SetPhase(swap, SwapPhase::Transferring);
    LOG_INFO("module", "mod-guild-adventure: account {} switches to {} through a world reload{}, waiting for the client "
        "({})", account, to->GetName(), swap.path == SwapPath::Switch ? " that keeps the map (in-world switch)" : "",
        recordSent ? "character record sent to the client DLL"
        : recordStale ? "no client DLL with the character record: the client keeps the old name and class"
        : "no client DLL with the character record; the client's record is this character's already");
    return true;
}

bool ControlSwapService::SendCharacterRecord(WorldSession* owner, Player* player)
{
    PlayerRecordInfo info;
    info.guid = player->GetGUID().GetRawValue();
    info.race = player->getRace(true); // the real race, as the character list has it
    info.classId = player->getClass();
    info.gender = player->getGender();
    info.level = uint8(std::min<uint32>(player->GetLevel(), 255));
    info.name = player->GetName();

    switch (sClientChannel.SendPlayerRecord(owner, info))
    {
        case PlayerRecordResult::Sent:
            return true;
        case PlayerRecordResult::Invalid:
            LOG_ERROR("module", "mod-guild-adventure: the character record of {} cannot be sent (guid {}, race {}, "
                "class {}, level {}, name of {} bytes)", info.name, info.guid, info.race, info.classId, info.level,
                info.name.size());
            return false;
        default:
            return false; // no DLL, or one without the character record
    }
}

bool ControlSwapService::AdvanceTransferring(WorldSession* owner, Swap& swap)
{
    Player* to = owner->GetPlayer();
    if (!to || to->GetGUID() != swap.to)
    {
        // The target left the owner's session while the client was loading (logout, CP-1 hand-back).
        Fail(owner, swap, SwapRefusal::Cancelled, "the character left your session while the world was loading");
        return false;
    }

    if (!to->IsInWorld() || to->IsBeingTeleportedFar())
    {
        if (!swap.transferWarned && Elapsed(swap.phaseSince) > TransferWarnAfter)
        {
            swap.transferWarned = true;
            LOG_WARN("module", "mod-guild-adventure: account {} has not finished loading the world around {} after {} s",
                swap.ownerAccountId, to->GetName(), Elapsed(swap.phaseSince).count());
        }
        return true;
    }

    FinishReload(owner, swap, to);
    return false;
}

void ControlSwapService::FinishReload(WorldSession* owner, Swap& swap, Player* target)
{
    // The world reload resent the target's spells and cooldowns, talents, action bars, reputations,
    // achievements, equipment sets, bind point, friends list, aura states, quest-giver states and the self create
    // block (Player::SendInitialPacketsBeforeAddToMap / AfterAddToMap); the client cleared its cooldowns itself
    // (LoadNewWorld). What it keeps from the old character is replaced here.

    // Spell modifiers: the client table is not part of the object manager, and the core sends modifiers only
    // when an aura changes.
    SendSpellModifiers(owner, target, swap.reload.spellMods);

    // World states: UpdateZone sends them only when the zone changes, and the target stays in its zone.
    uint32 zoneId = 0;
    uint32 areaId = 0;
    target->GetZoneAndAreaId(zoneId, areaId);
    target->SendInitWorldStates(zoneId, areaId);

    // Group frames: the client's party list is the old character's.
    if (Group* group = target->GetGroup())
        group->SendUpdateToPlayer(target->GetGUID());
    else if (!swap.reload.groupGuid.IsEmpty())
    {
        // The packet the core sends a member that left (Group.cpp:907-910): an empty list. The group counter is
        // not readable from outside the group; 0 is a T2 observation point (docs/take-control.md §5.8).
        WorldPacket data(SMSG_GROUP_LIST, 1 + 1 + 1 + 1 + 8 + 4 + 4 + 8);
        data << uint8(0x10) << uint8(0) << uint8(0) << uint8(0);
        data << swap.reload.groupGuid << uint32(0) << uint32(0) << uint64(0);
        owner->SendPacket(&data);
    }

    // Pet bar: the old character's pet bar stays on the client unless the target brings its own pet (resummoned
    // by the world reload, its bar follows when it is loaded).
    if (swap.reload.hadPet && target->GetPetGUID().IsEmpty() && !target->GetTemporaryUnsummonedPetNumber() && target->GetCharmGUID().IsEmpty())
        target->SendRemoveControlBar(); // SMSG_PET_SPELLS with an empty GUID, as Player::RemovePet sends it

    // Chat channels the target is on and the old character was not (the rest were left in StartReload).
    for (ReloadState::ChannelInfo const& channel : JoinedChannels(target))
    {
        bool const known = std::any_of(swap.reload.channels.begin(), swap.reload.channels.end(),
            [&channel](ReloadState::ChannelInfo const& other) { return other.name == channel.name; });
        if (!known)
            SendChannelNotice(owner, channel.name, channel.channelId, channel.flags, channel.constant, true);
    }

    // Guild: the roster for the new character, the MOTD only when the guild changed.
    if (Guild* guild = target->GetGuild())
    {
        if (guild->GetId() != swap.reload.guildId)
        {
            WorldPackets::Guild::GuildEvent motd;
            motd.Type = GE_MOTD;
            motd.Params.emplace_back(guild->GetMOTD());
            owner->SendPacket(motd.Write());
        }
        guild->HandleRoster(owner);
    }

    // The client keeps the per-character UI data (macros, key bindings, layout) of the character it logged in
    // with; uploads of it must not land on the target (OnAccountDataUpload).
    if (auto itr = _clientUiCharacter.find(swap.ownerAccountId); itr != _clientUiCharacter.end())
    {
        if (itr->second == target->GetGUID())
            _clientUiCharacter.erase(itr);
    }
    else if (!swap.from.IsEmpty())
        _clientUiCharacter[swap.ownerAccountId] = swap.from;

    // Every other guild bot of this owner now answers to the character the owner plays.
    sGuildBotHolder.SetMasterForOwner(swap.ownerAccountId, target);

    // Path S: did the client keep its map? Its SWITCH_ACK came before the reload's packets were processed.
    SwapPath taken = swap.path;
    std::string how = "world reload";
    if (swap.path == SwapPath::Switch)
    {
        std::optional<SwitchAckResult> const ack = sClientChannel.GetSwitchAck(owner, swap.switchNonce);
        if (ack && *ack == SwitchAckResult::Armed)
            how = "in-world switch, no loading screen";
        else
        {
            taken = SwapPath::Reload;
            how = Acore::StringFormat("world reload: the client DLL could not keep the map ({})",
                ack ? ToString(*ack) : "no SWITCH_ACK");
        }
    }

    auto const took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - swap.reloadStartedAt);
    Notify(owner, Acore::StringFormat("You now control {}. Your previous character keeps playing.", target->GetName()));
    LOG_INFO("module", "mod-guild-adventure: account {} now controls {} ({}, {} ms)", swap.ownerAccountId,
        target->GetName(), how, took.count());

    SwapEvent event;
    event.type = SwapEventType::Completed;
    event.ownerAccountId = swap.ownerAccountId;
    event.from = swap.from;
    event.to = swap.to;
    event.path = taken;
    Emit(std::move(event));
}

ControlSwapService::ReloadState ControlSwapService::CaptureClientState(Player* player)
{
    ReloadState state;
    state.spellMods = CollectSpellModValues(player);
    state.channels = JoinedChannels(player);
    if (Group* group = player->GetGroup())
        state.groupGuid = group->GetGUID();
    state.hadPet = player->GetPet() != nullptr;
    state.guildId = player->GetGuildId();
    return state;
}

void ControlSwapService::SendSpellModifiers(WorldSession* session, Player* target, std::unordered_map<uint32, int32> const& clientValues)
{
    // Every slot the client may hold a value in gets the target's value: the target's non-zero slots, and zero for
    // the slots only the old character had.
    std::unordered_map<uint32, int32> values = CollectSpellModValues(target);
    for (auto const& [key, value] : clientValues)
        values.emplace(key, 0); // keeps the target's value where both have one

    for (auto const& [key, value] : values)
    {
        bool const pct = (key >> 16) != 0;
        WorldPacket data(pct ? SMSG_SET_PCT_SPELL_MODIFIER : SMSG_SET_FLAT_SPELL_MODIFIER, 1 + 1 + 4);
        data << uint8(key & 0xFF);        // effect bit
        data << uint8((key >> 8) & 0xFF); // modifier op
        data << int32(value);
        session->SendPacket(&data);
    }
}

std::vector<ControlSwapService::ReloadState::ChannelInfo> ControlSwapService::JoinedChannels(Player* player)
{
    std::vector<ReloadState::ChannelInfo> channels;
    for (Channel const* channel : PlayerChannelList::Of(player))
    {
        if (!channel)
            continue;

        ReloadState::ChannelInfo info;
        info.name = channel->GetName();
        info.channelId = channel->GetChannelId();
        info.flags = channel->GetFlags();
        info.constant = channel->IsConstant();
        channels.push_back(std::move(info));
    }

    // Built-in channels first, in ChatChannels.dbc order (General, Trade, LocalDefense, ...), custom channels
    // (id 0) after them by name: the order a login joins them in, so the client numbers them the usual way.
    std::sort(channels.begin(), channels.end(), [](ReloadState::ChannelInfo const& a, ReloadState::ChannelInfo const& b)
    {
        if (a.constant != b.constant)
            return a.constant;
        if (a.channelId != b.channelId)
            return a.channelId < b.channelId;
        return a.name < b.name;
    });
    return channels;
}

void ControlSwapService::SetPhase(Swap& swap, SwapPhase phase)
{
    LOG_DEBUG("module", "mod-guild-adventure: swap of account {} to {}: {} -> {}", swap.ownerAccountId, NameOf(swap.to),
        ToString(swap.phase), ToString(phase));
    swap.phase = phase;
    swap.phaseSince = Clock::now();
}

void ControlSwapService::Retarget(Swap& swap, ObjectGuid to)
{
    swap.to = to;
    swap.toAccountId = sCharacterCache->GetCharacterAccountIdByGuid(to);
    swap.token = ++_nextToken; // results still in flight for the old target are ignored
    swap.enumReady = false;
    swap.enumRow.clear();
    swap.listFull = false;
    swap.accountDataReady = false;
    swap.accountData = nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// Async queries (module-owned processor, driven by Update)
// ---------------------------------------------------------------------------------------------------------------

ControlSwapService::Swap* ControlSwapService::FindSwap(uint32 ownerAccountId, uint32 token)
{
    auto itr = _swaps.find(ownerAccountId);
    return (itr != _swaps.end() && itr->second.token == token) ? &itr->second : nullptr;
}

void ControlSwapService::StartEnumQuery(Swap& swap)
{
    swap.enumRow.clear();

    // The owner's own characters (the leader) are in the stock list anyway.
    if (swap.toAccountId == swap.ownerAccountId)
    {
        swap.enumReady = true;
        return;
    }

    swap.enumReady = false;

    // The same statement HandleCharEnumOpcode runs (CharacterHandler.cpp:250-265), for the target's account; the
    // row of the target is kept, built by the core's own Player::BuildEnumData.
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(
        sWorld->getBoolConfig(CONFIG_DECLINED_NAMES_USED) ? CHAR_SEL_ENUM_DECLINED_NAME : CHAR_SEL_ENUM);
    stmt->SetData(0, PET_SAVE_AS_CURRENT);
    stmt->SetData(1, swap.toAccountId);

    uint32 const ownerAccountId = swap.ownerAccountId;
    uint32 const token = swap.token;
    ObjectGuid const to = swap.to;

    _queryProcessor.AddCallback(CharacterDatabase.AsyncQuery(stmt).WithPreparedCallback(
        [this, ownerAccountId, token, to](PreparedQueryResult result)
        {
            Swap* swap = FindSwap(ownerAccountId, token);
            if (!swap)
                return;

            swap->enumReady = true;
            if (!result)
                return;

            do
            {
                if ((*result)[0].Get<uint32>() != to.GetCounter())
                    continue;

                WorldPacket row;
                if (Player::BuildEnumData(result, &row) && row.size())
                    swap->enumRow.assign(row.contents(), row.contents() + row.size());
                break;
            } while (result->NextRow());
        }));
}

void ControlSwapService::StartAccountDataQuery(Swap& swap)
{
    swap.accountDataReady = false;
    swap.accountData = nullptr;

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_PLAYER_ACCOUNT_DATA);
    stmt->SetData(0, swap.to.GetCounter());

    uint32 const ownerAccountId = swap.ownerAccountId;
    uint32 const token = swap.token;

    _queryProcessor.AddCallback(CharacterDatabase.AsyncQuery(stmt).WithPreparedCallback(
        [this, ownerAccountId, token](PreparedQueryResult result)
        {
            Swap* swap = FindSwap(ownerAccountId, token);
            if (!swap)
                return;

            swap->accountData = std::move(result); // may be empty: no saved data, LoadAccountData resets it
            swap->accountDataReady = true;
        }));
}

// ---------------------------------------------------------------------------------------------------------------
// Core hooks
// ---------------------------------------------------------------------------------------------------------------

bool ControlSwapService::OnLogoutDetach(WorldSession* session, Player* player)
{
    // Shutdown and the bots' own home sessions log out normally.
    if (_shuttingDown || World::IsStopped() || !session || !player || session->IsHeadless())
        return false;

    uint32 const account = session->GetAccountId();
    ObjectGuid const guid = player->GetGUID();

    // 1. The logout this service started itself (Path R step 2).
    if (auto itr = _swaps.find(account); itr != _swaps.end())
    {
        Swap& swap = itr->second;
        if (swap.path == SwapPath::Relog && swap.phase == SwapPhase::Preparing && swap.from == guid)
        {
            Player* master = sGuildBotHolder.GetAIDrivenPlayer(swap.to);
            if (!sGuildBotHolder.HandToAI(player, session, account, master))
                return false;

            SetPhase(swap, SwapPhase::AtGlue);
            return true;
        }
    }

    // 3. The owner leaves while playing the leader, and the leader has an order (docs/orders.md "The leader"): it
    //    plays on in the offline window like the bots instead of logging out (D12 stays for a leader without one).
    if (player->GetCharacterAccountId() == account && _leaderStaysPolicy && _leaderStaysPolicy(account, guid))
    {
        if (!sGuildBotHolder.HandToAI(player, session, account, nullptr))
            return false;

        LOG_INFO("module", "mod-guild-adventure: account {} left its leader {}, it plays on with its order", account,
            player->GetName());

        SwapEvent event;
        event.type = SwapEventType::ReturnedToAI;
        event.ownerAccountId = account;
        event.from = guid;
        Emit(std::move(event));
        return true;
    }

    // 2. Logout or disconnect while the owner controls a guild bot of a bot account (docs/take-control.md §5.6):
    //    the bot goes back to the AI, the leader logs out (D12). The leader itself (a guild character on the
    //    owner's own account) logs out normally when the owner logs out while playing it, unless it has an order (3.).
    if (sGuildBotHolder.IsManaged(guid) && player->GetCharacterAccountId() != account)
    {
        uint32 const ownerAccountId = sGuildBotHolder.GetOwner(guid);
        if (!sGuildBotHolder.HandToAI(player, session, ownerAccountId, nullptr))
            return false;

        LOG_INFO("module", "mod-guild-adventure: account {} left {}, it keeps playing as a bot", account, player->GetName());
        QueueLeaderLogout(ownerAccountId);

        SwapEvent event;
        event.type = SwapEventType::ReturnedToAI;
        event.ownerAccountId = ownerAccountId;
        event.from = guid;
        Emit(std::move(event));
        return true;
    }

    return false;
}

void ControlSwapService::OnCharEnum(WorldSession* session, WorldPacket& data, uint8& characterCount)
{
    if (!session || session->IsHeadless())
        return;

    auto itr = _swaps.find(session->GetAccountId());
    if (itr == _swaps.end())
        return;

    Swap& swap = itr->second;
    if (swap.phase != SwapPhase::AtGlue || swap.toAccountId == swap.ownerAccountId || swap.enumRow.empty())
        return;

    if (characterCount >= MaxCharactersInList)
    {
        // No room on the character screen. The owner can still pick the previous character (it is attached the
        // same way); the swap target cannot be shown.
        LOG_ERROR("module", "mod-guild-adventure: account {} already lists {} characters, {} cannot be added to the character screen",
            swap.ownerAccountId, characterCount, NameOf(swap.to));
        swap.listFull = true;
        return;
    }

    data.append(swap.enumRow.data(), swap.enumRow.size());
    ++characterCount;
}

bool ControlSwapService::OnPlayerLoginPacket(WorldSession* session, WorldPacket const& packet)
{
    // Everything that is not "an authenticated client at the character screen" is left to the core.
    if (_shuttingDown || !session || session->IsHeadless() || session->GetPlayer() || session->PlayerLoading())
        return true;

    ObjectGuid guid;
    try
    {
        WorldPacket copy(packet);
        copy.rpos(0);
        copy >> guid;
    }
    catch (ByteBufferException const&)
    {
        return true; // the core handler reports the malformed packet
    }

    uint32 const account = session->GetAccountId();
    auto swapItr = _swaps.find(account);

    // Whatever the client logs into now, it loads that character's per-character UI data (Path W bookkeeping).
    _clientUiCharacter.erase(account);

    if (!sGuildBotHolder.IsManaged(guid))
    {
        // An ordinary character of the owner's own account: the core loads it. A swap waiting at the character
        // screen is given up, and the AI-driven leader does not keep playing without the owner (§5.2, D12).
        if (swapItr != _swaps.end() && swapItr->second.phase == SwapPhase::AtGlue)
        {
            LOG_INFO("module", "mod-guild-adventure: account {} picked another character, swap to {} given up",
                account, NameOf(swapItr->second.to));
            Fail(session, swapItr->second, SwapRefusal::Cancelled, "another character was picked on the character screen");
            _swaps.erase(swapItr);
            QueueLeaderLogout(account);
        }

        // A leader with an order that plays on without its owner (offline window, restart) gives way: its owner is
        // back and plays another character of the account. Picking the leader itself attaches it (below). The
        // logout runs in the next Update, outside the packet handler.
        if (_leaderStaysPolicy && _leaderStaysPolicy(account, ObjectGuid::Empty) &&
            std::find(_leaderGiveWay.begin(), _leaderGiveWay.end(), account) == _leaderGiveWay.end())
            _leaderGiveWay.push_back(account);
        return true;
    }

    // A managed character is in the world already, so the stock login would load a second copy; it is attached
    // instead, and only for its owner.
    if (sGuildBotHolder.GetOwner(guid) != account)
    {
        LOG_WARN("module", "mod-guild-adventure: account {} tried to log into {}, a guild bot of account {}",
            account, NameOf(guid), sGuildBotHolder.GetOwner(guid));
        session->SendCharLoginFailed(LoginFailureReason::DuplicateCharacter);
        return false;
    }

    if (swapItr != _swaps.end() && swapItr->second.phase == SwapPhase::Attaching)
        return false; // a second click while the first attach is queued

    // A reconnect within the disconnect tolerance leaves the previous character on an offline session.
    ReleaseOfflineSession(account);

    // The module may be logging the character in right now (a leader with an order after a restart, `.ga admin
    // login`): the owner's login waits for it and takes it over then (AdvanceAttaching).
    if (!sGuildBotHolder.IsAIDriven(guid) && !sGuildBotHolder.IsLoggingIn(guid))
    {
        session->SendCharLoginFailed(LoginFailureReason::DuplicateCharacter);
        return false;
    }

    if (swapItr == _swaps.end())
    {
        // No swap pending: the owner came back to the character screen (logout, reconnect) and picked a guild
        // character that is still in the world as a bot, typically the leader. Attach it the same way.
        Swap swap;
        swap.ownerAccountId = account;
        swap.to = guid;
        swap.toAccountId = sCharacterCache->GetCharacterAccountIdByGuid(guid);
        swap.token = ++_nextToken;
        swapItr = _swaps.emplace(account, std::move(swap)).first;
        StartAccountDataQuery(swapItr->second);

        // The AI plays the character now: a leader with an order keeps playing it after the attach (watch mode), unless
        // the owner was playing it by hand until its connection closed a moment ago (a reconnect).
        constexpr std::chrono::seconds ReconnectWindow{ 120 };
        auto const lost = _lostWhilePlayed.find(guid);
        bool const reconnect = lost != _lostWhilePlayed.end() && Elapsed(lost->second) <= ReconnectWindow;
        if (lost != _lostWhilePlayed.end())
            _lostWhilePlayed.erase(lost);
        if (!reconnect && _watchOnLoginPolicy && _watchOnLoginPolicy(account, guid, true))
            QueueWatchOnLogin(account, guid);
    }
    else if (swapItr->second.to != guid || swapItr->second.phase != SwapPhase::AtGlue)
    {
        // The owner picked a different guild character than requested (e.g. the previous one), or a swap that
        // never reached the character screen is replaced by this login.
        Retarget(swapItr->second, guid);
        StartAccountDataQuery(swapItr->second);
    }

    SetPhase(swapItr->second, SwapPhase::Attaching);
    LOG_INFO("module", "mod-guild-adventure: account {} logs into guild character {}, attaching", account, NameOf(guid));
    return false;
}

bool ControlSwapService::OnAccountDataUpload(WorldSession* session, WorldPacket const& packet)
{
    // After a Path W swap the owner's client still holds the per-character UI data (macros, key bindings, layout)
    // of the character it logged in with, but the session stores uploads under the character it plays now
    // (WorldSession::SetAccountData uses the GUID of the session's current player). Uploads of per-character
    // data are therefore dropped until the client logs into a character again; the client keeps its local copy,
    // which belongs to the character it logged in with. Global account data is stored as usual.
    if (!session || session->IsHeadless())
        return true;

    auto itr = _clientUiCharacter.find(session->GetAccountId());
    if (itr == _clientUiCharacter.end())
        return true;

    uint32 type = 0;
    try
    {
        WorldPacket copy(packet);
        copy.rpos(0);
        copy >> type;
    }
    catch (ByteBufferException const&)
    {
        return true; // the core handler reports the malformed packet
    }

    if (type >= NUM_ACCOUNT_DATA_TYPES || !((1u << type) & PER_CHARACTER_CACHE_MASK))
        return true;

    // Answer as the core does for a stored upload, so the client does not retry.
    WorldPacket data(SMSG_UPDATE_ACCOUNT_DATA_COMPLETE, 4 + 4);
    data << uint32(type);
    data << uint32(0);
    session->SendPacket(&data);

    LOG_DEBUG("module", "mod-guild-adventure: account {}: per-character account data {} not stored, the client's UI belongs to {}",
        session->GetAccountId(), type, NameOf(itr->second));
    return false;
}

void ControlSwapService::OnPlayerLogin(Player* player)
{
    WorldSession* session = player ? player->GetSession() : nullptr;
    if (!session || session->IsHeadless())
        return;

    // The owner logged into a character from the database (e.g. the leader after D12): the guild bots that kept
    // playing answer to it again.
    sGuildBotHolder.SetMasterForOwner(session->GetAccountId(), player);
}

void ControlSwapService::OnPlayerBeforeLogout(Player* player)
{
    if (!player)
        return;

    // No AI may keep a pointer to a player that leaves the world.
    sGuildBotHolder.ClearMaster(player);

    // A controlled guild character that really logs out (shutdown, or the leader logging out with the owner)
    // takes its parked home session with it.
    sGuildBotHolder.ForgetControlled(player->GetGUID());

    // D12: when the owner's client character logs out, an AI-driven leader logs out as well.
    WorldSession* session = player->GetSession();
    if (session && !session->IsHeadless() && !_shuttingDown)
        QueueLeaderLogout(session->GetAccountId());
}

void ControlSwapService::OnShutdown()
{
    // From here on every logout is a normal one (OnLogoutDetach also checks World::IsStopped()).
    _shuttingDown = true;
    _swaps.clear();
    _clientUiCharacter.clear();
    _pendingLeaderLogouts.clear();
    _leaderGiveWay.clear();
    _watchOnLogin.clear();
    _lostWhilePlayed.clear();
    _pendingEvents.clear();
}

// ---------------------------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------------------------

void ControlSwapService::HandBackFromClosedConnections()
{
    // When the client's connection closes, the core moves the session to its offline list and keeps the
    // character in the world for DisconnectToleranceInterval seconds (WorldSessionMgr::UpdateSessions,
    // WorldSession::HandleSocketClosed) so the client can reconnect into it. A controlled guild character would
    // stand idle there; it goes back to its AI at once instead. A reconnect then finds it AI-driven and attaches
    // it like any other guild character. No core patch is needed for this (CP-6 stays unwritten).
    for (GuildAdventure::ManagedInfo const& info : sGuildBotHolder.ListControlled())
    {
        // The leader (on the owner's own account) is an ordinary character for the core: it waits on the offline
        // session for a reconnect and then logs out normally, like any player character.
        if (info.characterAccountId == info.ownerAccountId)
            continue;

        WorldSession* offline = sWorldSessionMgr->FindOfflineSession(info.ownerAccountId);
        if (offline && offline->GetPlayer() && offline->GetPlayer()->GetGUID() == info.guid)
            HandBackFromOfflineSession(offline, false);
    }

    // A leader with an order plays on without its owner (docs/orders.md "The leader"): to the AI at once as well, managed
    // or not (a leader the owner logged into from the character screen is no managed character).
    if (_plannedLeaders)
        for (auto const& [owner, leader] : _plannedLeaders())
            if (WorldSession* offline = sWorldSessionMgr->FindOfflineSession(owner))
                if (offline->GetPlayer() && offline->GetPlayer()->GetGUID() == leader)
                    HandBackFromOfflineSession(offline, false);
}

bool ControlSwapService::HandBackFromOfflineSession(WorldSession* offline, bool reconnect)
{
    Player* player = offline ? offline->GetPlayer() : nullptr;
    if (!player)
        return false;
    // A managed guild character, or the owner's leader with an order (which may not be managed yet).
    bool const plannedLeader = player->GetCharacterAccountId() == offline->GetAccountId() && _leaderStaysPolicy &&
        _leaderStaysPolicy(offline->GetAccountId(), player->GetGUID());
    if (!plannedLeader && !sGuildBotHolder.IsManaged(player->GetGUID()))
        return false;

    // Defensive: the core parks only sessions whose player is in the world (WorldSession::HandleSocketClosed), but
    // a far teleport may start after that (a script, a spell effect). It is finished first, as
    // WorldSession::LogoutPlayer does (WorldSession.cpp:690-691); the AI takes a character over only in the world.
    while (player->IsBeingTeleportedFar())
        offline->HandleMoveWorldportAck();

    if (offline->GetPlayer() != player || !player->IsInWorld())
        return false;

    // A controlled guild character: back to its AI. The offline session then has no player and the core deletes
    // it in its next session update.
    ObjectGuid const guid = player->GetGUID();
    uint32 const ownerAccountId = plannedLeader ? offline->GetAccountId() : sGuildBotHolder.GetOwner(guid);
    bool const wasWatched = sGuildBotHolder.IsWatched(guid);
    if (!sGuildBotHolder.HandToAI(player, offline, ownerAccountId, nullptr))
        return false;
    // The owner played it by hand when the connection closed: a reconnect soon after is no return to an auto mode.
    if (!wasWatched)
        _lostWhilePlayed[guid] = Clock::now();

    if (reconnect)
        LOG_INFO("module", "mod-guild-adventure: account {} logs in again within the disconnect tolerance, {} goes "
            "from the offline session to the AI first", offline->GetAccountId(), NameOf(guid));
    else
        LOG_INFO("module", "mod-guild-adventure: the connection of account {} closed, {} keeps playing as a bot",
            offline->GetAccountId(), NameOf(guid));

    // D12: the owner is gone, so the leader does not keep playing either (skipped if the owner is back and plays
    // a guild character by the time the queue runs).
    QueueLeaderLogout(ownerAccountId);

    SwapEvent event;
    event.type = SwapEventType::ReturnedToAI;
    event.ownerAccountId = ownerAccountId;
    event.from = guid;
    Emit(std::move(event));
    return true;
}

void ControlSwapService::ReleaseOfflineSession(uint32 ownerAccountId)
{
    WorldSession* offline = sWorldSessionMgr->FindOfflineSession(ownerAccountId);
    if (!offline || !offline->GetPlayer())
        return;

    if (HandBackFromOfflineSession(offline, true))
        return;

    // Any other character: what the stock reconnect does when the client picks a different character
    // (CharacterHandler.cpp:725-726). It logs out when the core deletes the kicked session.
    offline->KickPlayer("mod-guild-adventure: the owner logs into a guild character");
}

void ControlSwapService::Fail(WorldSession* owner, Swap const& swap, SwapRefusal refusal, std::string const& message)
{
    Notify(owner, "Swap cancelled: " + message + ".");

    SwapEvent event;
    event.type = SwapEventType::Failed;
    event.ownerAccountId = swap.ownerAccountId;
    event.from = swap.from;
    event.to = swap.to;
    event.path = swap.path;
    event.refusal = refusal;
    event.message = message;
    Emit(std::move(event));
}

void ControlSwapService::Emit(SwapEvent event)
{
    LOG_DEBUG("module", "mod-guild-adventure: swap event '{}' for account {} ({} -> {}, {}){}", ToString(event.type),
        event.ownerAccountId, NameOf(event.from), NameOf(event.to), ToString(event.path),
        event.type == SwapEventType::Failed ? Acore::StringFormat(": {} ({})", ToString(event.refusal), event.message) : "");
    _pendingEvents.push_back(std::move(event));
}

void ControlSwapService::EmitNow(SwapEvent event)
{
    LOG_DEBUG("module", "mod-guild-adventure: swap event '{}' for account {} ({} -> {})", ToString(event.type),
        event.ownerAccountId, NameOf(event.from), NameOf(event.to));
    for (Listener const& listener : _listeners)
        listener(event);
}

void ControlSwapService::FlushEvents()
{
    // A listener may cause new events (e.g. call RequestSwap); they are delivered in the same flush.
    while (!_pendingEvents.empty())
    {
        std::vector<SwapEvent> events;
        events.swap(_pendingEvents);
        for (SwapEvent const& event : events)
            for (Listener const& listener : _listeners)
                listener(event);
    }
}

void ControlSwapService::QueueLeaderLogout(uint32 ownerAccountId)
{
    if (std::find(_pendingLeaderLogouts.begin(), _pendingLeaderLogouts.end(), ownerAccountId) == _pendingLeaderLogouts.end())
        _pendingLeaderLogouts.push_back(ownerAccountId);
}

void ControlSwapService::ProcessLeaderLogouts()
{
    // The owner logged into another character of the account: an AI-played leader with an order logs out.
    std::vector<uint32> giveWay;
    giveWay.swap(_leaderGiveWay);
    for (uint32 owner : giveWay)
    {
        WorldSession* session = sWorldSessionMgr->FindSession(owner);
        Player* played = session ? session->GetPlayer() : nullptr;
        for (GuildAdventure::ManagedInfo const& info : sGuildBotHolder.List(owner))
            if (info.characterAccountId == owner && info.state == GuildAdventure::ManagedState::Bot &&
                (!played || played->GetGUID() != info.guid) && sGuildBotHolder.Logout(info.guid))
                LOG_INFO("module", "mod-guild-adventure: account {} logs into {}, its leader {} stops playing on its own "
                    "and logs out", owner, played ? played->GetName() : std::string("another character"), NameOf(info.guid));
    }

    if (_pendingLeaderLogouts.empty())
        return;

    std::vector<uint32> owners;
    owners.swap(_pendingLeaderLogouts);

    for (uint32 owner : owners)
    {
        // A swap of this owner may be about to attach the leader.
        if (_swaps.find(owner) != _swaps.end())
            continue;

        // The owner is back and plays a guild character: the leader keeps holding its position.
        if (WorldSession* session = sWorldSessionMgr->FindSession(owner))
            if (Player* player = session->GetPlayer())
                if (sGuildBotHolder.IsManaged(player->GetGUID()))
                    continue;

        // A leader with an order plays on in the offline window like the bots; the window's end logs it out
        // (OfflineService::EndWindow).
        if (_leaderStaysPolicy && _leaderStaysPolicy(owner, ObjectGuid::Empty))
            continue;

        sGuildBotHolder.LogoutLeaderOf(owner);
    }
}

std::string ControlSwapService::NameOf(ObjectGuid guid)
{
    std::string name;
    if (!sCharacterCache->GetCharacterNameByGuid(guid, name))
        name = guid.ToString();
    return name;
}

void ControlSwapService::Notify(WorldSession* session, std::string const& text)
{
    if (session && session->GetPlayer())
        ChatHandler(session).SendSysMessage("|cff00ccff[Guild Adventure]|r " + text);
}

std::string ControlSwapService::DescribeState(uint32 ownerAccountId) const
{
    auto itr = _swaps.find(ownerAccountId);
    if (itr == _swaps.end())
    {
        WorldSession* session = sWorldSessionMgr->FindSession(ownerAccountId);
        Player* player = session ? session->GetPlayer() : nullptr;
        if (player && sGuildBotHolder.IsWatched(player->GetGUID()))
            return Acore::StringFormat("no swap in progress; watch mode: the AI plays {}, you watch", player->GetName());
        return "no swap in progress";
    }

    Swap const& swap = itr->second;
    return Acore::StringFormat("swap to {} ({}), path {}: phase '{}' for {} s{}", NameOf(swap.to), swap.to.ToString(),
        ToString(swap.path), ToString(swap.phase), Elapsed(swap.phaseSince).count(), swap.listFull ? ", character screen full" : "");
}
