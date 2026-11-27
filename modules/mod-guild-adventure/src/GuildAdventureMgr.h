/*
 * GuildAdventureMgr: the module's persistent state (adventure guilds, bot accounts, bots, orders, profession
 * plans, owned unlocks, events) and the world-database templates (zones, unlocks, renown curve and sources).
 *
 * Everything lives in memory and is written through to the characters database on change (asynchronous
 * statements). It is loaded once at startup on the world thread. All access happens on the world thread;
 * hooks that fire on map threads queue their work instead (GuildAdventurePlayerScript).
 *
 * Data model: data/sql/db-characters/base/ga_characters.sql, data/sql/db-world/base/ga_world.sql,
 * The design log section 3.6.
 */

#ifndef MOD_GUILD_ADVENTURE_GUILD_ADVENTURE_MGR_H
#define MOD_GUILD_ADVENTURE_GUILD_ADVENTURE_MGR_H

#include "ObjectGuid.h"
#include "SharedDefines.h"
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

namespace GuildAdventure
{
    enum class BotState : uint8
    {
        Active  = 0, // logs in with the owner, executes its order
        Benched = 1  // stays offline, keeps its slot
    };

    char const* ToString(BotState state);

    // Order types (docs/protocol.md section 8.1). The token is what the protocol and ga_bot_order use.
    enum class OrderType : uint8
    {
        Auto,
        Idle,
        Follow,
        LevelZone,
        Grind,
        Quest,
        Gather,
        Craft,
        Dungeon,
        Train,
        Vendor,
        Farm       // farm an item (docs/orders.md "Farm orders"): the order's zone id is the item
    };

    char const* ToToken(OrderType type);
    std::optional<OrderType> OrderTypeFromToken(std::string const& token);
    bool IsOrderImplemented(OrderType type); // accepted by this server version (STATE^orders)
    bool OrderNeedsZone(OrderType type);

    struct BotOrder
    {
        OrderType type = OrderType::Auto;
        uint32 zoneId = 0;
        uint8 levelCap = 0;       // AUTO limit, 0 = none
        int16 continent = -1;     // AUTO limit, -1 = anywhere
        bool noDungeons = false;  // AUTO limit
        uint64 goldCap = 0;       // AUTO limit, copper per trip, 0 = no limit
        uint32 farmTarget = 0;    // FARM: the stock of the item in the guild storage at which the order ends, 0 = none
        uint32 setAt = 0;
    };

    // A quest the order planner failed on (docs/orders.md "Failure rules"); persisted in ga_bot_quest_block.
    struct QuestBlock
    {
        uint8 failures = 0;
        bool blocked = false;     // never picked again
        std::string reason;
        uint32 updatedAt = 0;
    };

    struct ProfessionPlan
    {
        uint16 primary[2] = { 0, 0 };
        uint16 target[2] = { 0, 0 };
        std::vector<uint16> secondary;
    };

    // Last known in-world values of a bot, kept for the roster while it is offline.
    struct BotSnapshot
    {
        uint8 level = 1;
        uint32 xp = 0;
        uint32 zoneId = 0;
        uint32 mapId = 0;
        uint32 money = 0;
        // Gear level (Readiness::GearLevel) and professions ("skillId:value:max,...") for the roster's columns:
        // loaded at startup, then refreshed while the bot is in the world.
        uint16 gearLevel = 0;
        std::string skills;
    };

    struct GuildBot
    {
        ObjectGuid guid;
        uint32 botAccount = 0;
        uint8 slot = 0;
        BotState state = BotState::Active;
        bool autoAdvance = true;
        uint32 recruitedAt = 0;
        uint32 kills = 0;
        uint32 deaths = 0;
        uint32 quests = 0;
        BotOrder order;
        ProfessionPlan professions;
        std::unordered_map<uint32, QuestBlock> questBlocks; // by quest id

        // Character data (from the character cache / last time in the world), not persisted by the module.
        std::string name;
        uint8 race = 0;
        uint8 classId = 0;
        uint8 gender = 0;
        BotSnapshot snapshot;
    };

    struct GuildEvent
    {
        uint32 id = 0;
        uint32 time = 0;
        std::string type;    // protocol token (docs/protocol.md section 8.3)
        ObjectGuid bot;      // empty = guild event
        std::string message; // English, formatted by the server
        uint8 level = 0;
        uint32 zoneId = 0;
        int32 points = 0;
    };

    // An achievement the guild earned: the first guild character to complete it, once per guild
    // (AchievementService, ga_guild_achievement).
    struct GuildAchievement
    {
        uint32 id = 0;
        ObjectGuid earnedBy;
        uint32 earnedAt = 0;
        int32 renown = 0;    // renown it gave the guild
    };

    struct AdventureGuild
    {
        uint32 ownerAccount = 0;
        uint32 guildId = 0;           // WoW guild, 0 until it exists
        ObjectGuid leader;
        TeamId team = TEAM_ALLIANCE;
        uint8 unlockedSlots = 3;
        uint32 renownTotal = 0;
        uint8 renownLevel = 1;
        uint16 offlineWindowMin = 60;
        uint32 ownerLogoutAt = 0;     // 0 while the owner is online
        uint32 createdAt = 0;

        std::map<ObjectGuid, GuildBot> bots;
        // The leader's order (docs/orders.md "The leader"). The leader is no roster bot (no slot, no ga_bot row), but it
        // can be given an order like one: then the order planner plays it whenever the AI holds it (while the owner
        // plays a bot, in watch mode, in the offline window). leaderPlanned = false is the default "hold": the AI keeps
        // it where it is, as before. guid, name, race, class and gender are always set; order and questBlocks only
        // mean something while leaderPlanned is true (ga_bot_order / ga_bot_quest_block rows of the leader's guid).
        GuildBot leaderPlan;
        bool leaderPlanned = false;
        std::set<uint32> ownedUnlocks;
        std::deque<GuildEvent> recentEvents; // newest last, at most RecentEventCount
        std::map<uint32, GuildAchievement> achievements; // by achievement id, loaded by AchievementService

        GuildBot* FindBot(ObjectGuid guid);
        GuildBot const* FindBot(ObjectGuid guid) const;
        // A character the order planner may play: a roster bot, or the leader while it has an order.
        GuildBot* FindPlanned(ObjectGuid guid);
        GuildBot const* FindPlanned(ObjectGuid guid) const;
        // A character of the guild that can be in a dungeon group: a roster bot, or the leader (its record is
        // leaderPlan, also while it has no order).
        GuildBot* FindMember(ObjectGuid guid);
        GuildBot const* FindMember(ObjectGuid guid) const;
        uint8 UsedSlots() const { return uint8(bots.size()); }
    };

    struct ZoneTemplate
    {
        uint32 zoneId = 0;
        std::string name;
        uint8 minLevel = 0;
        uint8 maxLevel = 0;
        char faction = 'C';           // 'A', 'H', 'C'
        uint16 continent = 0;
        std::vector<OrderType> orders;

        bool AllowsTeam(TeamId team) const;
        bool AllowsOrder(OrderType type) const;
    };

    enum class UnlockKind : uint8
    {
        Slots,
        Offline,
        Feature
    };

    char const* ToToken(UnlockKind kind);

    struct UnlockTemplate
    {
        uint32 id = 0;
        uint8 renownLevel = 1;
        UnlockKind kind = UnlockKind::Feature;
        std::string value;
        uint64 goldCost = 0;
        std::string description;
    };

    struct BotAccount
    {
        uint32 accountId = 0;
        uint32 ownerAccount = 0;
        uint8 poolIndex = 0;
        uint32 createdAt = 0;
    };

    constexpr std::size_t RecentEventCount = 30;
}

class GuildAdventureMgr
{
public:
    static GuildAdventureMgr& Instance();

    // Startup (world thread): templates from the world database, state from the characters database.
    void LoadTemplates();
    void LoadState();

    // --- templates ---
    std::vector<GuildAdventure::ZoneTemplate> const& GetZones() const { return _zones; }
    GuildAdventure::ZoneTemplate const* GetZone(uint32 zoneId) const;
    uint32 GetZoneVersion() const { return _zoneVersion; }
    std::vector<GuildAdventure::UnlockTemplate> const& GetUnlocks() const { return _unlocks; }
    GuildAdventure::UnlockTemplate const* GetUnlock(uint32 id) const;
    // Renown needed to advance from 'level' to the next one, 0 at the highest level.
    uint32 GetRenownToNext(uint8 level) const;
    uint8 GetMaxRenownLevel() const;
    int32 GetRenownPoints(std::string const& eventType) const;

    // --- guilds ---
    GuildAdventure::AdventureGuild* FindGuild(uint32 ownerAccount);
    GuildAdventure::AdventureGuild* FindGuildOfBot(ObjectGuid botGuid);
    GuildAdventure::AdventureGuild* FindGuildOfLeader(ObjectGuid leaderGuid);
    // The guild of a roster bot or of a leader (whether it has an order or not).
    GuildAdventure::AdventureGuild* FindGuildOfMember(ObjectGuid guid);
    std::unordered_map<uint32, GuildAdventure::AdventureGuild>& GetGuilds() { return _guilds; }

    GuildAdventure::AdventureGuild& CreateGuild(uint32 ownerAccount, Player* leader, uint8 slots, uint16 offlineWindowMin);
    void SaveGuild(GuildAdventure::AdventureGuild const& guild) const;

    // --- bots ---
    void AddBot(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot bot);
    void SaveBot(GuildAdventure::AdventureGuild const& guild, GuildAdventure::GuildBot const& bot) const;
    void SaveOrder(GuildAdventure::GuildBot const& bot) const;
    // The leader goes back to "hold" (no order): its order row and its quest blocks are deleted.
    void ClearLeaderOrder(GuildAdventure::AdventureGuild& guild) const;
    void SaveProfessions(GuildAdventure::GuildBot const& bot) const;
    void SaveQuestBlock(GuildAdventure::GuildBot const& bot, uint32 questId) const;
    void SaveUnlock(GuildAdventure::AdventureGuild const& guild, uint32 unlockId) const;

    // --- bot accounts ---
    std::vector<GuildAdventure::BotAccount> GetBotAccounts(uint32 ownerAccount) const;
    void AddBotAccount(GuildAdventure::BotAccount const& account);
    uint32 CountBotsOnAccount(uint32 accountId) const;

    // --- events ---
    // Records an event (memory + database) and returns it with its id.
    GuildAdventure::GuildEvent const& AddEvent(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildEvent event);
    // Deletes event rows older than keepDays (0 = none), except each owner's newest RecentEventCount, which are the
    // feed the addon shows. Asynchronous; called at startup and a few times a day.
    void PruneEventLog(uint32 keepDays) const;

private:
    GuildAdventureMgr() = default;

    void LoadBots();
    void LoadBotSnapshots();
    void LoadLeaders();
    void LoadOrders();
    void LoadProfessions();
    void LoadQuestBlocks();
    void LoadUnlocks();
    void LoadEvents();
    void LoadBotAccounts();

    std::vector<GuildAdventure::ZoneTemplate> _zones;
    std::unordered_map<uint32, std::size_t> _zoneIndex;
    uint32 _zoneVersion = 0;
    std::vector<GuildAdventure::UnlockTemplate> _unlocks;
    std::map<uint8, uint32> _renownToNext;
    std::unordered_map<std::string, int32> _renownSources;

    std::unordered_map<uint32, GuildAdventure::AdventureGuild> _guilds; // by owner account
    std::unordered_map<ObjectGuid, uint32> _botOwner;                  // bot guid -> owner account
    std::vector<GuildAdventure::BotAccount> _botAccounts;
    uint32 _nextEventId = 1;
};

#define sGuildAdventureMgr GuildAdventureMgr::Instance()

#endif
