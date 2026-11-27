/*
 * ControlSwapService: "take control" of a guild bot without logging it out.
 *
 * Two client paths share the same hand-over primitives (GuildBotHolder::HandToAI / TakeFromAI):
 *   Path R ("warm relog", docs/take-control.md §5.2): the owner's current character is handed to the AI on the
 *     spot, the client returns to the character screen, the character list additionally shows the target bot,
 *     and the client's login request for it re-attaches the running bot to the owner's session instead of
 *     loading it from the database.
 *   Path W ("world reload", experiment, docs/take-control.md §5.8): both characters change sessions at once and
 *     the target is sent a far teleport to the spot it stands on. The client shows a loading screen, rebuilds its
 *     object manager and takes the target as its active player from the target's self create block; it never
 *     sees the character screen. Continents only (no instance, no transport, not Ebon Hold).
 * Both characters stay in the world throughout; no character changes account.
 *
 * Also handles logout and disconnect while the owner controls a guild bot (§5.6): the bot goes back to the AI,
 * the leader logs out (decision D12).
 *
 * Watch mode (§5.12): while the owner controls a guild bot, RequestWatch lets the AI play that same character on with
 * its order while the owner's client keeps it and only watches; RequestWatch(false) gives the controls back. No session
 * change, no loading screen. A swap away from a watched character ends watch mode first.
 *
 * Everything runs on the world thread (invariant I5): requests are queued by the command and the packet hook and
 * advanced in Update(), which runs from WorldScript::OnUpdate after the map updates.
 *
 * Other services (the GADV protocol, the event log) use the public API: RequestSwap, GetSwapPhase and the event
 * listeners (SwapEvent). They never change the swap state themselves.
 */

#ifndef MOD_GUILD_ADVENTURE_CONTROL_SWAP_SERVICE_H
#define MOD_GUILD_ADVENTURE_CONTROL_SWAP_SERVICE_H

#include "AsyncCallbackProcessor.h"
#include "DatabaseEnvFwd.h"
#include "ObjectGuid.h"
#include "QueryCallback.h"
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class Player;
class WorldPacket;
class WorldSession;

namespace GuildAdventure
{
    enum class SwapPhase : uint8
    {
        Requested, // queued by ".ga swap"; checked in the next Update
        Preparing, // both characters saved; the target's character-list row and account data are being loaded
        AtGlue,      // Path R: the old character is AI-driven, the client is at the character screen
        Attaching,   // Path R: the client asked to log into a guild character; it is attached in the next Update
        Transferring // Path W: the characters changed sessions; the client loads the world around the target
    };

    char const* ToString(SwapPhase phase);

    // How the owner's client gets from one character to the other.
    enum class SwapPath : uint8
    {
        Relog,  // Path R: character screen in between
        Reload, // Path W: loading screen only (experiment)
        Switch  // Path S: the world reload of Path W, but the client DLL keeps the loaded map: no loading screen
    };

    char const* ToString(SwapPath path);

    // What a request (or GuildAdventure.Swap.Path) asks for. Auto takes Path S where it is possible, else Path W
    // where that is possible, and Path R everywhere else.
    enum class SwapPathMode : uint8
    {
        Relog,
        Reload,
        Auto,
        Switch
    };

    char const* ToString(SwapPathMode mode);
    // "relog", "reload", "switch" or "auto" (case-insensitive); nullopt for anything else.
    std::optional<SwapPathMode> ParseSwapPathMode(std::string_view text);

    // Why a swap was refused or given up. ToString returns the token the GADV protocol uses (docs/protocol.md §8.5,
    // §8.6); the protocol layer may map tokens it does not know to a generic error.
    enum class SwapRefusal : uint8
    {
        None,         // accepted
        Busy,         // another swap of this owner is in progress
        Same,         // the owner already plays the target
        NotInWorld,   // the owner has no character in the world
        Offline,      // the target is not an AI-driven guild bot in the world (not logged in, loading, controlled)
        NotOwner,     // the target is a guild bot of another owner
        Loading,      // logging in or out, teleporting, changing maps
        Dead,
        InCombat,
        Flight,       // on a flight path
        Vehicle,
        Charmed,
        ControlsUnit, // charms or possesses another unit (mind control, Eyes of the Beast, ...)
        Duel,
        Trade,
        Battleground, // in a battleground or arena, or queued for one
        Lfg,          // in the dungeon finder's role check, queue, proposal or kick vote
        GameMaster,   // GM mode is on (a GM-flagged character must not be handed to the AI)
        SelfBot,      // the owner's character is a playerbots self bot or holds stock .playerbots altbots
        // Path W or S was asked for explicitly, but the target is in an instance, on a transport or in Ebon Hold, or
        // (Path S) on another map, too far away or the client has no DLL with the in-world switch
        PathUnavailable,
        NotGuildBot,  // watch mode: the owner's character is not one of the guild's characters (a test bot)
        NoOrder,      // watch mode: the owner's leader has no order (it would only hold its position)
        ShuttingDown,
        Cancelled,    // given up after it was accepted: the client left, picked another character, timed out
        Internal      // server-side failure; details in the server log
    };

    char const* ToString(SwapRefusal refusal);

    struct SwapRequestResult
    {
        SwapRefusal refusal = SwapRefusal::None;
        std::string message;            // English text for the player, empty when accepted
        SwapPath path = SwapPath::Relog; // accepted: the path the swap takes (Auto may still fall back to Relog)

        explicit operator bool() const { return refusal == SwapRefusal::None; }
    };

    enum class SwapEventType : uint8
    {
        Started,      // a swap from 'from' to 'to' was accepted
        LeavingWorld, // Path R only: the owner's client is sent to the character screen right after this event;
                      // the last moment a message reaches the owner's addon before the UI unloads. Path W keeps
                      // the UI loaded (loading screen only) and has no such event
        Completed,    // the owner's client session now plays 'to' ('from' is empty when the owner logged into an
                      // AI-driven guild character from the character screen without a swap)
        Failed,       // the swap was refused after it started, or given up; see refusal and message. Before
                      // LeavingWorld the owner still plays 'from'; after it the owner is at the character screen
                      // or gone
        ReturnedToAI, // no swap: the owner logged out or lost the connection while controlling the guild
                      // character 'from', which is AI-driven again
        WatchChanged  // watch mode of the owner's character 'from' went on or off (see 'watching'); delivered at once
    };

    char const* ToString(SwapEventType type);

    struct SwapEvent
    {
        SwapEventType type = SwapEventType::Started;
        uint32 ownerAccountId = 0;
        ObjectGuid from;
        ObjectGuid to;
        SwapPath path = SwapPath::Relog;         // the path of the swap (Completed: the path it really took)
        SwapRefusal refusal = SwapRefusal::None; // Failed only
        std::string message;                      // Failed only
        bool watching = false;                    // WatchChanged only
    };
}

class ControlSwapService
{
public:
    static ControlSwapService& Instance();

    void LoadConfig();

    using Listener = std::function<void(GuildAdventure::SwapEvent const&)>;

    // The owner's session wants to control 'target' (".ga swap <name>", GADV CONTROL_TAKE / CONTROL_RETURN). On
    // success the request is queued and advanced in the next Update; the outcome arrives as a SwapEvent. 'mode'
    // picks the client path; nullopt uses GuildAdventure.Swap.Path.
    GuildAdventure::SwapRequestResult RequestSwap(WorldSession* owner, ObjectGuid target,
        std::optional<GuildAdventure::SwapPathMode> mode = std::nullopt);
    // The phase of the owner's swap, nullopt when none is in progress.
    [[nodiscard]] std::optional<GuildAdventure::SwapPhase> GetSwapPhase(uint32 ownerAccountId) const;
    [[nodiscard]] std::string DescribeState(uint32 ownerAccountId) const;

    // Watch mode (docs/take-control.md §5.12; ".ga watch", GADV CONTROL_WATCH). on = true: the AI plays the owner's
    // current character (a controlled guild bot) with its order, the client watches; on = false: the owner takes the
    // controls back. Takes effect at once and emits WatchChanged; switching to the state the character already has is
    // accepted and changes nothing. Turning it off is always possible.
    GuildAdventure::SwapRequestResult RequestWatch(WorldSession* owner, bool on);
    // Which characters may be watched besides the basic checks: those the order planner plays (the roster bots, the
    // leader while it has an order); the policy answers None, NotGuildBot or NoOrder. Without a policy every controlled
    // guild character of a bot account may.
    void SetWatchPolicy(std::function<GuildAdventure::SwapRefusal(ObjectGuid)> policy) { _watchPolicy = std::move(policy); }

    // A login into the leader while it plays an order keeps the AI playing it (watch mode) until the owner takes the
    // controls back (docs/orders.md "The leader"). The policy gets the account, the character and whether the AI was
    // playing it at the login; QueueWatchOnLogin starts watch mode once the owner's session plays it in the world.
    void SetWatchOnLoginPolicy(std::function<bool(uint32, ObjectGuid, bool)> policy) { _watchOnLoginPolicy = std::move(policy); }
    void QueueWatchOnLogin(uint32 ownerAccountId, ObjectGuid guid);
    // Ends watch mode of a guild character wherever it is played (the leader's order went back to hold); no-op otherwise.
    void EndWatchOf(ObjectGuid guid);
    // The leader with an order plays on in the offline window like the bots (docs/orders.md "The leader"; the D12
    // logout stays for a leader without one). The policy answers for an owner account and a character (empty: the
    // owner's leader, whichever it is): true when that character is the owner's leader and has an order.
    void SetLeaderStaysPolicy(std::function<bool(uint32, ObjectGuid)> stays) { _leaderStaysPolicy = std::move(stays); }
    // The leaders with an order, as (owner account, leader guid): when the owner's connection closes while it plays one,
    // the leader goes to the AI at once (as a guild bot does), instead of standing idle for the disconnect tolerance.
    void SetPlannedLeadersSource(std::function<std::vector<std::pair<uint32, ObjectGuid>>()> source)
    {
        _plannedLeaders = std::move(source);
    }

    // Registers a listener for swap events, normally once at startup. Listeners run on the world thread, outside
    // map updates: LeavingWorld right before the client leaves the world, every other event at the end of
    // Update. A listener may send packets and call RequestSwap, but must not call AddListener.
    void AddListener(Listener listener);

    // Core hooks
    bool OnLogoutDetach(WorldSession* session, Player* player);                      // CP-1, SessionScript
    void OnCharEnum(WorldSession* session, WorldPacket& data, uint8& characterCount); // CP-3, SessionScript
    bool OnPlayerLoginPacket(WorldSession* session, WorldPacket const& packet);      // ServerScript::CanPacketReceive
    bool OnAccountDataUpload(WorldSession* session, WorldPacket const& packet);      // ServerScript::CanPacketReceive
    void OnPlayerLogin(Player* player);                                              // PlayerScript
    void OnPlayerBeforeLogout(Player* player);                                       // PlayerScript

    void Update();     // WorldScript::OnUpdate
    void OnShutdown(); // WorldScript::OnShutdown

private:
    ControlSwapService() = default;

    using Clock = std::chrono::steady_clock;

    // Path W: what the owner's client still holds of the old character and the world reload does not replace
    // (docs/take-control.md §5.8). Captured right before the hand-over.
    struct ReloadState
    {
        struct ChannelInfo
        {
            std::string name;
            uint32 channelId = 0;
            uint8 flags = 0;
            bool constant = false;
        };

        std::unordered_map<uint32, int32> spellMods; // SpellModKey -> summed value, non-zero entries only
        std::vector<ChannelInfo> channels;          // chat channels the client shows as joined
        ObjectGuid groupGuid;                       // empty when not in a group
        bool hadPet = false;
        uint32 guildId = 0;
    };

    struct Swap
    {
        uint32 ownerAccountId = 0;
        ObjectGuid from;            // the character the owner played when the swap started (empty for a direct login)
        ObjectGuid to;              // the guild character the owner takes over
        uint32 toAccountId = 0;     // characters.account of 'to'
        GuildAdventure::SwapPhase phase = GuildAdventure::SwapPhase::Requested;
        Clock::time_point phaseSince = Clock::now();
        uint32 token = 0;           // ties async query results to this swap and target
        GuildAdventure::SwapPathMode mode = GuildAdventure::SwapPathMode::Relog;
        GuildAdventure::SwapPath path = GuildAdventure::SwapPath::Relog;

        bool enumReady = false;
        std::vector<uint8> enumRow; // the target's SMSG_CHAR_ENUM row (Player::BuildEnumData)
        bool listFull = false;      // the character screen had no room for the target

        bool accountDataReady = false;
        PreparedQueryResult accountData; // the target's per-character account data

        ReloadState reload;          // Paths W and S
        bool transferWarned = false; // Paths W and S: the slow-loading warning was logged
        uint32 switchNonce = 0;      // Path S: the nonce of the SWITCH sent to the client DLL
        Clock::time_point reloadStartedAt; // Paths W and S: when StartReload ran (timing in the log)
    };

    GuildAdventure::SwapRequestResult CanSwap(WorldSession* owner, Player* from, Player* to) const;
    static GuildAdventure::SwapRequestResult CheckGuards(Player* player, bool allowInCombat);
    // Watch-mode guards: what the AI cannot take over (a vehicle, a charm, a duel, a trade, battlegrounds, the dungeon
    // finder, GM mode); combat, death and flight paths are fine (§5.12).
    static GuildAdventure::SwapRequestResult CheckWatchGuards(Player* player);
    // Ends watch mode of the owner's character 'player' (if on) and tells the listeners.
    void EndWatch(uint32 ownerAccountId, Player* player);
    // Path W limits for the target (docs/take-control.md §5.8); an empty result means Path W is possible.
    static GuildAdventure::SwapRequestResult CheckReloadPossible(Player* to);
    // Path R needs room on the owner's character screen for a target of another account (CP-3 adds it to the
    // list, which the client caps at 10 entries); an empty result means Path R is possible.
    static GuildAdventure::SwapRequestResult CheckRelogPossible(WorldSession* owner, Player* to);
    // Path S limits (docs/take-control.md §5.11): Path W possible, a client DLL with the in-world switch and the
    // character record, the target on the owner's map within GuildAdventure.Swap.SeamlessMaxDistance.
    GuildAdventure::SwapRequestResult CheckSwitchPossible(WorldSession* owner, Player* from, Player* to) const;
    // The path a request in 'mode' takes for 'to'; refuses an explicit Reload or Switch that is not possible. Auto
    // takes Path S, then Path W only for a client whose DLL rewrites the character record (§5.8), else Path R.
    GuildAdventure::SwapRequestResult ResolvePath(WorldSession* owner, GuildAdventure::SwapPathMode mode, Player* from,
        Player* to) const;

    // Phase handlers; each returns false when the swap is finished or cancelled and must be removed.
    bool AdvanceRequested(WorldSession* owner, Swap& swap);
    bool AdvancePreparing(WorldSession* owner, Swap& swap);
    bool AdvanceAtGlue(WorldSession* owner, Swap& swap);
    bool AdvanceAttaching(WorldSession* owner, Swap& swap);
    bool Attach(WorldSession* owner, Swap& swap);
    bool StartReload(WorldSession* owner, Swap& swap);       // Paths W and S: hand-over plus world reload
    // Path W: asks the owner's client DLL to write 'player' into the client's record of its logged-in character.
    static bool SendCharacterRecord(WorldSession* owner, Player* player);
    bool AdvanceTransferring(WorldSession* owner, Swap& swap);
    void FinishReload(WorldSession* owner, Swap& swap, Player* target);

    static ReloadState CaptureClientState(Player* player);
    static void SendSpellModifiers(WorldSession* session, Player* target,
        std::unordered_map<uint32, int32> const& clientValues);
    static std::vector<ReloadState::ChannelInfo> JoinedChannels(Player* player);

    void StartEnumQuery(Swap& swap);
    void StartAccountDataQuery(Swap& swap);
    Swap* FindSwap(uint32 ownerAccountId, uint32 token);
    void SetPhase(Swap& swap, GuildAdventure::SwapPhase phase);
    void Retarget(Swap& swap, ObjectGuid to);

    // Gives up a swap: tells the owner (if in the world) and emits Failed.
    void Fail(WorldSession* owner, Swap const& swap, GuildAdventure::SwapRefusal refusal, std::string const& message);

    void Emit(GuildAdventure::SwapEvent event);    // queued, delivered by FlushEvents at the end of Update
    void EmitNow(GuildAdventure::SwapEvent event); // delivered at once (LeavingWorld)
    void FlushEvents();

    // A controlled guild character whose owner's connection closed goes back to the AI at once, instead of
    // standing idle on the core's offline session for DisconnectToleranceInterval seconds.
    void HandBackFromClosedConnections();
    // reconnect: the owner logs in again within the disconnect tolerance (only the log line differs).
    bool HandBackFromOfflineSession(WorldSession* offline, bool reconnect);
    void ReleaseOfflineSession(uint32 ownerAccountId);
    void QueueLeaderLogout(uint32 ownerAccountId);
    void ProcessLeaderLogouts();
    void ProcessWatchOnLogin();
    // A character the owner's client has just taken over (Path R attach, Paths W and S reload): where the client is
    // told it stands, then its position for a short while, so a client that falls under the world shows in the log.
    void WatchArrival(uint32 ownerAccountId, Player* player, char const* how);
    void UpdateArrivals();

    static std::string NameOf(ObjectGuid guid);
    static void Notify(WorldSession* session, std::string const& text);

    std::unordered_map<uint32, Swap> _swaps; // by owner account id
    // Path W: the character whose per-character UI data (macros, key bindings, layout) the owner's client still
    // holds, by owner account, while the client plays another character. Cleared by the next character login.
    std::unordered_map<uint32, ObjectGuid> _clientUiCharacter;
    std::vector<uint32> _pendingLeaderLogouts;
    std::vector<uint32> _leaderGiveWay;       // owners who logged into another character: their AI-played leader logs out
    std::vector<Listener> _listeners;
    std::function<GuildAdventure::SwapRefusal(ObjectGuid)> _watchPolicy;
    std::function<bool(uint32, ObjectGuid, bool)> _watchOnLoginPolicy;
    struct PendingWatch
    {
        ObjectGuid guid;
        Clock::time_point since = Clock::now();
    };
    std::unordered_map<uint32, PendingWatch> _watchOnLogin; // by owner account: watch mode to start after the login
    std::unordered_map<ObjectGuid, Clock::time_point> _lostWhilePlayed; // played by hand when its connection closed
    struct Arrival
    {
        uint32 ownerAccountId = 0;
        ObjectGuid guid;
        std::string how;
        Clock::time_point since = Clock::now();
        Clock::time_point nextSample = Clock::now();
    };
    std::vector<Arrival> _arrivals;
    std::function<bool(uint32, ObjectGuid)> _leaderStaysPolicy;
    std::function<std::vector<std::pair<uint32, ObjectGuid>>()> _plannedLeaders;
    std::vector<GuildAdventure::SwapEvent> _pendingEvents;
    QueryCallbackProcessor _queryProcessor;
    uint32 _nextToken = 0;
    GuildAdventure::SwapPathMode _defaultPathMode = GuildAdventure::SwapPathMode::Relog;
    bool _allowInCombat = false;
    float _seamlessMaxDistance = 533.0f; // GuildAdventure.Swap.SeamlessMaxDistance (yards)
    bool _shuttingDown = false;
};

#define sControlSwapService ControlSwapService::Instance()

#endif
