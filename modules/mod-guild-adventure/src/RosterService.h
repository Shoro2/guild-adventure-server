/*
 * RosterService: the guild and its bots (the design log sections 2.1, 2.3).
 *
 * - Founds the adventure guild on the owner's first login (the leader is the owner's own character, D14) and
 *   creates or links the WoW guild with the leader as guild master.
 * - Owns the pool of bot accounts per owner (D11) and recruits bots: validates the request like the core's
 *   character creation, creates the character on a bot account, adds it to the guild and logs it in.
 * - Logs the active bots in when the owner enters the world with the leader, benches and activates bots,
 *   keeps the roster snapshot (level, zone, money) and guild membership of bots in the world up to date.
 *
 * Bots are logged in and out through GuildBotHolder (owned by the take-control code); this service never
 * moves players between sessions. World thread only.
 */

#ifndef MOD_GUILD_ADVENTURE_ROSTER_SERVICE_H
#define MOD_GUILD_ADVENTURE_ROSTER_SERVICE_H

#include "AsyncCallbackProcessor.h"
#include "DatabaseEnvFwd.h"
#include "ObjectGuid.h"
#include "Transaction.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

class Player;

namespace GuildAdventure
{
    struct AdventureGuild;
    struct GuildBot;
    enum class BotState : uint8;

    // Outcome of a roster request; the token is the protocol error code (docs/protocol.md section 8.5).
    enum class RosterError : uint8
    {
        None,
        BadRequest,
        Locked,
        NoSlot,
        NameInvalid,
        NameTaken,
        InvalidCombo,
        UnknownBot,
        Busy,
        Internal
    };

    char const* ToToken(RosterError error);

    struct RecruitRequest
    {
        uint8 race = 0;
        uint8 classId = 0;
        uint8 gender = 0;
        std::string name;
    };

    struct RosterResult
    {
        RosterError error = RosterError::None;
        std::string message; // English text for the player
        ObjectGuid bot;

        explicit operator bool() const { return error == RosterError::None; }
    };

    // The roster state of a bot as the protocol shows it (docs/protocol.md section 8.2).
    char const* GetBotStateToken(AdventureGuild const& guild, GuildBot const& bot);
}

class RosterService
{
public:
    static RosterService& Instance();

    // The owner entered the world with one of their own characters (not a guild bot).
    void OnOwnerCharacterLogin(Player* player);

    // Recruitment: checks synchronously; on success the character is created asynchronously and 'done' runs on
    // the world thread when it exists (or failed). Nothing is created when the check fails.
    GuildAdventure::RosterResult Recruit(GuildAdventure::AdventureGuild& guild, GuildAdventure::RecruitRequest request,
        std::function<void(GuildAdventure::RosterResult const&)> done);

    GuildAdventure::RosterResult SetBotState(GuildAdventure::AdventureGuild& guild, ObjectGuid bot, GuildAdventure::BotState state);

    // Dismissing a bot (the owner, 2026-11-28: [quote left out]): what the guild storage takes of its items (not
    // soulbound) goes there, its gold into the guild bank, then the character is deleted for good and its slot is free.
    // A bot that is not in the world is logged in for it first. 'done' runs on the world thread once it is gone (or the
    // dismissal failed), at once when it was in the world. Refused while the owner plays it, while it logs in and while
    // it is on a dungeon run.
    GuildAdventure::RosterResult Dismiss(GuildAdventure::AdventureGuild& guild, ObjectGuid bot,
        std::function<void(GuildAdventure::RosterResult const&)> done);
    [[nodiscard]] bool IsBeingDismissed(ObjectGuid bot) const { return _pendingDismiss.count(bot) != 0; }

    // Logs in every active bot of the guild that is not in the world. master may be null.
    void LoginActiveBots(GuildAdventure::AdventureGuild& guild, Player* master);
    // A leader with an order plays on without its owner (docs/orders.md "The leader"): logs it in on a home session
    // when the owner's account has no session at all (a restart inside the offline window, `.ga admin login`).
    // Returns true when a login started. The owner's own login always wins: it takes the running leader over.
    bool LoginPlannedLeader(GuildAdventure::AdventureGuild& guild);

    // A guild bot's player while it is in the world (AI-driven or controlled), else nullptr.
    [[nodiscard]] Player* GetBotPlayer(ObjectGuid bot) const;
    [[nodiscard]] uint32 GetPendingRecruits(uint32 ownerAccount) const;

    void Update(uint32 diff);

private:
    RosterService() = default;

    bool EnsureWowGuild(GuildAdventure::AdventureGuild& guild, Player* leader);
    GuildAdventure::RosterResult CheckRecruit(GuildAdventure::AdventureGuild const& guild, GuildAdventure::RecruitRequest& request) const;
    uint32 GetOrCreateBotAccount(uint32 ownerAccount, std::string& error);
    uint32 CreateBotAccount(uint32 ownerAccount, uint8 poolIndex, std::string& error);
    uint8 FindFreeSlot(GuildAdventure::AdventureGuild const& guild) const;
    void FinishRecruit(uint32 ownerAccount, ObjectGuid guid, uint32 botAccount, uint8 slot, bool success);
    void RefreshBotsInWorld();

    struct PendingRecruit
    {
        uint32 ownerAccount = 0;
        uint32 botAccount = 0;
        uint8 slot = 0;
        std::string name;
        std::function<void(GuildAdventure::RosterResult const&)> done;
    };

    AsyncCallbackProcessor<TransactionCallback> _transactions;
    std::unordered_map<ObjectGuid, PendingRecruit> _pendingRecruits;
    // Bots being dismissed that had to log in first (Dismiss); finished by Update once they are in the world.
    struct PendingDismiss
    {
        uint32 ownerAccount = 0;
        uint32 since = 0; // getMSTime
        std::function<void(GuildAdventure::RosterResult const&)> done;
    };
    std::unordered_map<ObjectGuid, PendingDismiss> _pendingDismiss;
    void UpdateDismissals();
    GuildAdventure::RosterResult FinishDismiss(GuildAdventure::AdventureGuild& guild, ObjectGuid guid, Player* player);
    std::unordered_set<ObjectGuid> _guildChecked; // bots whose guild membership was checked since they entered the world
    uint32 _refreshTimer = 0;
    // Guilds playerbots does not count as real yet (BotDriver::IsRealGuild): how often its list was read again for them.
    void CheckRealGuilds();
    uint32 _realGuildTimer = 0;
    std::unordered_map<uint32, uint8> _realGuildReloads;
};

#define sRosterService RosterService::Instance()

#endif
