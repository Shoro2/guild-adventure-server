/*
 * GuildBotHolder: owns the guild bots of every owner.
 *
 * Each guild character that the module manages has exactly one headless "home session" on the character's
 * own account. While the AI drives the character it sits on its home session and is a playerbots bot of this
 * holder; while the owner plays it, it sits on the owner's client session and its home session is parked
 * (no player, callbacks frozen). Bots stay in the world without an online master.
 *
 * Watch mode (docs/take-control.md §5.12): a controlled guild bot can also be driven by a PlayerbotAI while it stays
 * on the owner's client session. The order planner plays it as usual, the client keeps it as its active player and
 * only watches (its movement controls are locked, as under a fear effect), and the owner takes the controls back at
 * any time. No session changes and the client sees no loading screen either way.
 *
 * Placeholder AI (docs/take-control.md §5.14): a controlled character its owner plays by hand inside a dungeon group
 * of bots carries a PlayerbotAI that never runs. playerbots makes a party member without an AI the master of every
 * bot in the party; with the placeholder the owner's character counts as a bot for that rule and the group keeps
 * following its tank. For the module such a character is not AI-played (BotDriver::GetAI returns nullptr).
 *
 * Design: docs/take-control.md §4.2 (home sessions), §5.1 (primitives), invariants I1-I5.
 * Everything here runs on the world thread, outside map updates (I5).
 */

#ifndef MOD_GUILD_ADVENTURE_GUILD_BOT_HOLDER_H
#define MOD_GUILD_ADVENTURE_GUILD_BOT_HOLDER_H

#include "ObjectGuid.h"
#include "PlayerbotMgr.h"
#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;
class WorldSession;

namespace GuildAdventure
{
    enum class ManagedState : uint8
    {
        Loading,    // home session exists, login query holder in flight
        Loaded,     // the player is in the world on its home session, waiting to be adopted next tick
        Failed,     // the login failed; the entry is removed next tick
        Bot,        // AI-driven on its home session (in playerBots)
        Controlled  // attached to its owner's client session; home session parked
    };

    char const* ToString(ManagedState state);

    enum class LoginResult : uint8
    {
        Started,
        AlreadyManaged,
        AlreadyOnline,
        UnknownCharacter,
        QueryFailed
    };

    char const* ToString(LoginResult result);

    struct ManagedInfo
    {
        ObjectGuid guid;
        std::string name;
        uint32 ownerAccountId = 0;
        uint32 characterAccountId = 0;
        ManagedState state = ManagedState::Loading;
        bool watched = false; // Controlled only: the AI plays it, the owner's client watches (watch mode)
    };

    // A playerbots AI that exists and never runs (ObjectUpdateScripts.cpp keeps playerbots' update away from it).
    // Available: the stand-in for playerbots' update hook is in place. Set on the world thread; IsSet from any thread.
    namespace IdleAI
    {
        bool Available();
        void Set(ObjectGuid guid, bool idle);
        bool IsSet(ObjectGuid guid);
    }
}

class GuildBotHolder : public PlayerbotHolder
{
public:
    static GuildBotHolder& Instance();

    // Starts the asynchronous login of a character as a guild bot of ownerAccountId. masterGuid (may be empty)
    // becomes the bot's playerbots master once it is in the world.
    GuildAdventure::LoginResult Login(ObjectGuid guid, uint32 ownerAccountId, ObjectGuid masterGuid);

    // Logs an AI-driven guild bot out (save, remove from world) and deletes its home session.
    bool Logout(ObjectGuid guid);

    // Logs out every AI-driven guild bot of ownerAccountId whose character is on that same account (the leader;
    // decision D12: the leader logs out with the owner, the bots keep playing).
    void LogoutLeaderOf(uint32 ownerAccountId);

    // Shutdown: logs out every AI-driven bot and deletes all home sessions. Controlled characters are left to
    // the core, which logs them out through their client sessions.
    void LogoutAll();

    // Hand-over primitives (docs/take-control.md §5.1).
    // HandToAI: 'player' sits on the client session 'from'; it moves to its home session (created on first use)
    // and becomes a bot of this holder with the given master. ownerAccountId is recorded for new entries. It quiesces
    // the character first (Quiesce), so every way into the AI gets it: a swap, a logout, a disconnect.
    bool HandToAI(Player* player, WorldSession* from, uint32 ownerAccountId, Player* master);
    // Ends what the client started on 'player' (on its client session 'session') before someone else controls it:
    // movement, casts, loot, gossip and vendor windows, a trade, a duel, a charmed or possessed unit, a bind sight,
    // a vehicle seat. The swap guards keep most of these away from a swap; a logout or a disconnect can happen in
    // any of them (docs/take-control.md §5.6). Idempotent.
    static void Quiesce(Player* player, WorldSession* session);
    // Right before a client takes 'player' over (it learns its position from the server: the in-world login's
    // SMSG_LOGIN_VERIFY_WORLD and self create block, a world reload's destination): an AI-driven character has no
    // client physics, so it may stand under the ground (a straight movement through a slope, a fight's chase, a
    // saved spot under the terrain) or carry a fall that never ended, and a client put under the terrain falls
    // through the world. Puts it on the floor it stands in or under and clears the movement state a fresh client
    // never had (docs/take-control.md §5.13). Returns what it changed, empty when nothing.
    static std::string SettleForClient(Player* player);
    // TakeFromAI: 'player' is an AI-driven bot of this holder; its AI is removed and it moves to the client
    // session 'to'. The caller replays the in-world login for the client.
    bool TakeFromAI(Player* player, WorldSession* to);

    // Watch mode (docs/take-control.md §5.12). StartWatch: 'player' is a controlled character on its owner's client
    // session (a guild bot taken over, or the leader the owner logged into, which gets its entry here); it gets a
    // PlayerbotAI without a master (the order planner drives it like any guild bot) and the
    // client's movement controls are locked. StopWatch removes that AI again; restoreClientControl gives the client
    // its movement controls back (false when the character is about to change sessions anyway). Both run on the world
    // thread outside map updates; StopWatch is a no-op for a character that is not watched.
    bool StartWatch(Player* player);
    bool StopWatch(Player* player, bool restoreClientControl);
    [[nodiscard]] bool IsWatched(ObjectGuid guid) const;
    // The same from any thread (the map threads read it for the client's movement packets, which a watched client must
    // not send: ControlSwapScripts.cpp). Refreshed by StartWatch, StopWatch and every update.
    [[nodiscard]] bool IsWatchedAnyThread(ObjectGuid guid) const;

    // Placeholder AI (see the file comment). StartPlaceholder: 'player' is a controlled character on its owner's
    // client session that is played by hand (not watched; the leader the owner logged into gets its entry here, as in
    // StartWatch); it gets a PlayerbotAI that playerbots never updates. Nothing changes for the client. Call it before
    // the character joins a party of bots. StopPlaceholder removes it (the character must be out of that party).
    // The policy says which controlled characters need one (OrderService: a member of a dungeon group); the holder
    // asks it when a character comes into its owner's hands (TakeFromAI, the end of watch mode) and, every tick, for
    // the characters that have one: without the need the character leaves the bots' party and the placeholder goes.
    bool StartPlaceholder(Player* player);
    bool StopPlaceholder(Player* player);
    [[nodiscard]] bool HasPlaceholder(ObjectGuid guid) const;
    void SetPlaceholderPolicy(std::function<bool(ObjectGuid)> wants) { _placeholderPolicy = std::move(wants); }

    // Records 'master' as the character ownerAccountId plays now and makes it the playerbots master of every AI-driven
    // character of that owner that wants a master (the leader always; guild bots per the master policy).
    void SetMasterForOwner(uint32 ownerAccountId, Player* master);
    // Master policy (OrderService, docs/orders.md): which guild bots get the owner's character as their playerbots
    // master. Without a policy every character does (the take-control prototype). The leader is not asked: it always
    // has the owner's character as master while the AI holds it (it holds position, the design log section 2.8).
    void SetMasterPolicy(std::function<bool(ObjectGuid)> wantsMaster) { _masterPolicy = std::move(wantsMaster); }
    // Leader policy (OrderService, docs/orders.md "The leader"): true for a leader that has an order. Such a leader is
    // planned like a guild bot (master per the master policy, no "stay"); without one the leader holds its position with
    // the owner's character as master, as before.
    void SetLeaderPlanPolicy(std::function<bool(ObjectGuid)> planned) { _leaderPlanPolicy = std::move(planned); }
    // An AI-driven leader whose order went back to hold: master and "stay" again, as at its adoption.
    void ReapplyLeaderPolicy(ObjectGuid leader);
    // The character the owner of this managed character plays right now (in the world), nullptr if none.
    [[nodiscard]] Player* GetOwnerCharacter(ObjectGuid guid) const;
    // Clears every reference to 'player' as a master (called before a player is removed from the world).
    void ClearMaster(Player* player);
    // A controlled character really leaves the world (shutdown, the leader logging out with the owner): delete its
    // parked home session and forget it. No-op for characters in any other state.
    void ForgetControlled(ObjectGuid guid);

    // PlayerScript::OnPlayerCanMarkAccountOffline: WorldSession::LogoutPlayer marks every character of the session's
    // account offline. Several guild bots share one bot account (D11), so while another managed character of that
    // account is loading, AI-driven or controlled, only the character that leaves is marked offline and false is
    // returned (the core then skips its account-wide update).
    bool OnCanMarkAccountOffline(ObjectGuid guid, uint32 accountId);

    [[nodiscard]] bool IsManaged(ObjectGuid guid) const;
    [[nodiscard]] bool IsAIDriven(ObjectGuid guid) const;
    // The AI plays it: a guild bot in the world, or a character its owner watches (watch mode). What moves, places and
    // turns the members of a group asks this, not IsAIDriven, which leaves the watched character out.
    [[nodiscard]] bool IsAIPlayed(ObjectGuid guid) const { return IsAIDriven(guid) || IsWatched(guid); }
    // The module's login of this character is in flight (its query holder runs, or it waits to be adopted).
    [[nodiscard]] bool IsLoggingIn(ObjectGuid guid) const;
    [[nodiscard]] uint32 GetOwner(ObjectGuid guid) const;           // 0 if not managed
    [[nodiscard]] Player* GetAIDrivenPlayer(ObjectGuid guid) const; // nullptr unless state Bot
    // The character the order planner drives: AI-driven on its home session (state Bot), or watched on its owner's
    // client session (state Controlled, watch mode). nullptr otherwise.
    [[nodiscard]] Player* GetPlannedPlayer(ObjectGuid guid) const;
    [[nodiscard]] std::vector<GuildAdventure::ManagedInfo> List(uint32 ownerAccountId) const;
    // Every character in state Controlled, of all owners; without names (cheap enough for every tick).
    [[nodiscard]] std::vector<GuildAdventure::ManagedInfo> ListControlled() const;

    void Update(); // world thread, once per tick (WorldScript::OnUpdate)

protected:
    void OnBotLoginInternal(Player* const bot) override;

private:
    GuildBotHolder() = default;

    struct Entry
    {
        uint32 ownerAccountId = 0;
        uint32 characterAccountId = 0;
        WorldSession* homeSession = nullptr; // owned; headless, on characterAccountId
        GuildAdventure::ManagedState state = GuildAdventure::ManagedState::Loading;
        ObjectGuid masterGuid;               // master to set when the character is (re)adopted

        // Watch mode (state Controlled only)
        bool watched = false;
        bool watchTeleporting = false;       // last tick: a teleport was in progress (the client resets its controls)
        bool watchCrowdControlled = false;   // last tick: feared, confused or charmed (the core hands controls back after)
        bool watchFlying = false;            // last tick: on a flight path
        uint32 watchLockedAt = 0;            // getMSTime() of the last control lock sent to the client

        // Placeholder AI (state Controlled only, never together with watched)
        bool placeholder = false;
    };

    static WorldSession* CreateHomeSession(uint32 accountId);
    // The one place that rebinds a player between sessions (invariant I1).
    static void MoveToSession(Player* player, WorldSession* from, WorldSession* to);
    void Adopt(Player* player, Entry& entry);
    void ApplyLeaderPolicy(Player* player, Entry const& entry);
    // Removes the watch-mode AI of 'player' (entry in state Controlled, watched); see StopWatch.
    void EndWatch(Player* player, Entry& entry, bool restoreClientControl);
    // Watch mode: keeps the client's movement controls locked (the client unlocks them after every teleport, and the
    // core after a fear, confuse or charm effect ends).
    void UpdateWatchLocks();
    // Removes the placeholder AI of 'player' (nullptr: the character is gone, only the mark goes).
    void EndPlaceholder(ObjectGuid guid, Player* player, Entry& entry);
    // Placeholders nobody needs any more: out of the bots' party, then the AI goes.
    void UpdatePlaceholders();
    [[nodiscard]] bool WantsMaster(ObjectGuid guid, Entry const& entry) const;

    std::unordered_map<ObjectGuid, Entry> _entries;
    // The characters watched now, for IsWatchedAnyThread.
    mutable std::shared_mutex _watchedNowLock;
    std::unordered_set<ObjectGuid> _watchedNow;
    void RefreshWatchedNow();
    std::function<bool(ObjectGuid)> _masterPolicy;
    std::function<bool(ObjectGuid)> _leaderPlanPolicy;
    std::function<bool(ObjectGuid)> _placeholderPolicy;
};

#define sGuildBotHolder GuildBotHolder::Instance()

#endif
