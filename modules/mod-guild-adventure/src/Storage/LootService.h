/*
 * LootService: what happens to the loot of guild characters (docs/storage.md "Loot filter", "Auto loot").
 *
 * Loot filter: a port of ForgottenLand's mod-loot-filter (the owner's own module) without its Paragon part, with the rules
 * per adventure guild instead of per character. Every item a guild character loots (the leader and its bots; by hand,
 * through playerbots' own looting or through auto loot) is checked against the guild's rules one world tick after the
 * loot, and kept, sold, disenchanted (materials to the guild storage), deleted or put into the guild storage. An item
 * no rule decides gets the guild's default: "to the storage" for a character the AI plays (the owner: everything the bots
 * loot goes straight into the guild storage), "keep" for the character the owner plays himself. Never touched: quest
 * items; and for a character the AI plays, what it will use itself (playerbots' item usage: an equipment upgrade, food,
 * drink, potions it keeps, ammunition, a quest starter it can take), unless a rule keeps it anyway.
 *
 * Junk (GuildAdventure.LootFilter.SellJunk, the owner 2026-11-12: [quote left out]): loot no rule decides that is junk is sold at once, before the
 * guild's default applies. Junk = grey items, and white weapons and armour no guild character in the world would wear,
 * and white misc junk; never crafting materials (trade goods, anything a profession recipe uses), quest items and items
 * a quest asks for, consumables, bags, ammunition, recipes, gems, keys, tools or items without a vendor price. A rule of
 * the guild decides first; what a bot will use itself stays with it anyway. SellStoredJunk does the same for what lies
 * in the guild storage already, on the owner's request (the Storage tab's "Sell junk").
 *
 * Auto loot: a port of ForgottenLand's mod-auto-loot for guild characters: their own kills within a few yards are
 * looted (and skinned by a skinner), mining and herb nodes in reach are gathered, every few hundred milliseconds,
 * without walking to each corpse. It runs in the character's own update (map thread); the items it takes go through
 * the loot filter like any other loot. Off per guild (the addon) or for the server (GuildAdventure.AutoLoot.Enable).
 *
 * Threads: the loot hook and auto loot fire on map threads and only queue (rules and actions run on the world thread);
 * the guild's settings that auto loot reads are behind a mutex.
 */

#ifndef MOD_GUILD_ADVENTURE_LOOT_SERVICE_H
#define MOD_GUILD_ADVENTURE_LOOT_SERVICE_H

#include "LootRules.h"
#include "ObjectGuid.h"
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Creature;
class Item;
class Player;
struct ItemTemplate;

namespace GuildAdventure
{
    struct LootSettings
    {
        bool filterEnabled = true;
        uint8 defaultAi = LootRules::ActionStore;     // loot no rule decides, character the AI plays
        uint8 defaultPlayer = LootRules::ActionKeep;  // loot no rule decides, character the owner plays
        bool autoLoot = true;
        uint64 totalSold = 0;                         // copper
        uint32 totalDisenchanted = 0;
        uint32 totalDeleted = 0;
        uint32 totalStored = 0;
    };

    // One line of the guild's recent loot (the addon's "Recent loot" list).
    struct LootLogEntry
    {
        uint32 time = 0;
        ObjectGuid character;
        uint32 entry = 0;
        int32 randomProperty = 0;
        uint32 count = 0;
        uint8 outcome = LootRules::ActionKeep;        // what happened (an action number)
        uint64 money = 0;                             // sold: copper
        std::string why;                              // short reason when the outcome is not what the rule asked for
    };

    constexpr std::size_t LootLogSize = 50;

    // What of the guild storage is junk (LootService::SellStoredJunk).
    struct JunkSale
    {
        uint32 kinds = 0;
        uint32 items = 0;
        uint64 money = 0;       // copper
        bool capped = false;    // the character's gold limit stopped the sale early
    };

    // What the owner's Disenchant on a storage item did (LootService::DisenchantFromStorage).
    struct StorageDisenchant
    {
        uint32 done = 0;            // how many were disenchanted (0: refused, see error)
        std::string enchanter;      // who did it
        std::string materials;      // "2 x Strange Dust, 1 x Lesser Magic Essence"
        std::string error;          // not_disenchantable, no_enchanter, skill, nothing
        std::string text;           // a line for the chat
    };
}

class LootService
{
public:
    static LootService& Instance();

    // Startup (world thread): rules and settings.
    void Load();
    // World thread, every tick: the loot of the last tick, the totals, the chat lines of the owner's character.
    void Update(uint32 diff);
    void FlushNow();

    // Hook (any thread): a character looted an item (Player::StoreLootItem, auto loot).
    void OnLooted(Player* player, Item* item);
    // Bag sweep (world thread): every item in the bags of a character the AI plays goes through the loot filter as if
    // it had just been looted (what it carried from before, what it was given). Runs by itself for AI-played guild
    // characters whose bags fill up (Update); returns how many stacks it looked at.
    uint32 SweepBags(Player* player);
    // The Darkmoon Faire cards and decks an AI-played character carries go into the guild storage (Storage.DarkmoonCards,
    // StorageService::IsDarkmoonCard), one look a minute; returns the stacks moved.
    uint32 StoreDarkmoonCards(Player* player);
    // The reputation turn-ins (Storage.ReputationTurnIns, StorageService::IsReputationTurnIn) likewise, at the same look.
    uint32 StoreReputationTurnIns(Player* player);
    // Gear it has worn (the design log section 7 WG1, docs/storage.md "Gear it has worn"; world thread): a green or blue
    // weapon or armour piece in the bags of an AI-played guild character that it has worn (soulbound: the guild storage
    // cannot take it) and will not wear again is disenchanted by the character itself when its Enchanting is high
    // enough (the materials into the guild storage), else sold to the vendor it stands at (vendor: nullptr = no sale).
    // A log line per piece. Bot setting LootFilter.WornGear (0 keep, 1 disenchant or sell, 2 disenchant only). Runs
    // with the bag sweep (no vendor) and at the planner's vendor visits. Returns how many pieces went.
    uint32 HandleWornGear(Player* player, Creature* vendor);
    // Why the worn-gear rule leaves this piece with the character (nullptr: it may go).
    char const* WornGearKeeps(Player* player, Item* item) const;
    // Quest items no quest needs any more (the design log section 7 QI1, the owner 2026-11-18; HandleWornGear calls it first):
    // a grey or white quest item or quest starter in the bags of an AI-played guild character that no quest in its
    // log uses and no quest it can still do for experience asks for (a starter: whose quest gives none any more) is
    // sold to the vendor it stands at, or deleted where it stands when no vendor buys it. Bot setting
    // LootFilter.SpareQuestItems. Returns how many stacks went.
    uint32 HandleSpareQuestItems(Player* player, Creature* vendor);
    char const* QuestItemKeeps(Player* player, Item* item) const;

    // --- rules and settings (world thread; the protocol and the commands) ---
    std::vector<GuildAdventure::LootRules::Rule> const& GetRules(uint32 owner);
    GuildAdventure::LootSettings GetSettingsOf(uint32 owner) const;
    // Adds (rule.id == 0) or replaces a rule; error codes: LIMIT, NOT_FOUND, INVALID, ACTION.
    bool SetRule(uint32 owner, GuildAdventure::LootRules::Rule rule, std::string& error);
    bool DeleteRule(uint32 owner, uint32 ruleId);
    bool MoveRule(uint32 owner, uint32 ruleId, uint32 position);
    bool EnableRule(uint32 owner, uint32 ruleId, bool enabled);
    void ClearRules(uint32 owner);
    // Settings without the totals.
    void SetOptions(uint32 owner, bool filterEnabled, uint8 defaultAi, uint8 defaultPlayer, bool autoLoot);
    std::deque<GuildAdventure::LootLogEntry> const& GetLog(uint32 owner);
    // The actions the server allows (GuildAdventure.LootFilter.Allow*).
    static uint8 AllowedMask();

    // What the guild's rules would do with this item for this character right now (".ga loot test"): the action and
    // the deciding rule's position (0 = default), or ResultProtected.
    GuildAdventure::LootRules::Verdict Test(Player* player, Item* item);

    // Auto loot (the character's own update, map thread).
    void OnCharacterUpdate(Player* player, uint32 diff);
    // A creature `player` (or its group) has the loot of died: the corpse scan of auto loot runs for it and for the
    // members of its group on the map for a while (GuildAdventure.AutoLoot.ArmByKill, docs/performance.md PF-3).
    void OnKillCredit(Player* player, Creature* killed);
    bool IsAutoLootOn(uint32 owner) const;

    // Log entries that arrived since the last call, per owner (world thread; the protocol pushes them).
    std::vector<uint32> GetOwnersWithNewLog() const;
    std::vector<GuildAdventure::LootLogEntry> TakeNewLog(uint32 owner);

    // --- junk (world thread) ---
    // True if this kind is junk up to `maxQuality` (-1: nothing is) for the guild of `owner` (see above). With
    // `keepWorn` grey and white gear a guild character in the world would wear is not junk.
    bool IsJunk(uint32 owner, ItemTemplate const* proto, int32 randomProperty, int32 maxQuality, bool keepWorn);
    // The bot setting LootFilter.KeepWornGear for what this character loots, or for the guild.
    static bool KeepWornGear(Player const* looter);
    static bool KeepWornGearOfGuild(uint32 owner);
    // The character could put it on and wears nothing better in that place (the owner's own character; bots ask
    // mod-playerbots' item usage).
    static bool WouldWear(Player* player, ItemTemplate const* proto);
    // A tool the character works with: an item with a totem category (a pick, a knife, a hammer, a rod), a fishing
    // pole, or an item a recipe the character knows names as its tool (the Jeweler's Kit: no totem category in the
    // item data).
    bool IsToolOf(Player* player, ItemTemplate const* proto) const;

    // --- the owner's own orders on the guild storage (world thread; docs/storage.md "Selling and disenchanting") ---
    // Adds a line to the guild's recent loot, the storage's log: who, what, how many, what happened.
    void AddStorageLog(uint32 owner, ObjectGuid character, uint32 entry, int32 randomProperty, uint32 count, uint8 outcome,
        uint64 money, std::string why);
    // Outcomes of the storage's log beyond the loot actions (docs/protocol.md section 8.8): taken out of the guild
    // storage / put into it by an order of the owner (Take, Deposit, the auction house surplus).
    static constexpr uint8 LogTaken = 5;
    static constexpr uint8 LogDeposited = 6;
    static constexpr uint8 LogOpened = 7;      // a container a guild character opened (docs/storage.md "Containers")
    static constexpr uint8 LogBought = 8;      // bought at a vendor for the shopping list (docs/storage.md "Shopping list")

    // --- containers (the owner, 2026-11-13: [quote left out]; world thread) ---
    // How an item opens: with its own loot (a right click: bags of loot, crates, junkboxes), with a lock on that loot
    // (a lockbox: a rogue picks it), or with its use spell that hands out loot (the clams).
    enum class ContainerKind : uint8
    {
        None = 0,
        Loot,
        Locked,
        Spell,
    };
    static ContainerKind ContainerKindOf(ItemTemplate const* proto);
    // The lockpicking skill an item's lock needs; -1 when no lockpicking opens it (a key, no lock).
    static int32 PickLockSkill(ItemTemplate const* proto);
    // The lockpicking a key opens with (a blacksmith's skeleton key: its use spell opens a lock of the lockpicking kind
    // with this skill); -1 when the item is no such key. Only items that ask for Blacksmithing: never a Seaforium Charge
    // or another consumable whose spell opens a lock.
    static int32 KeySkill(ItemTemplate const* proto);
    // The weakest key in the character's bags it may use (a skeleton key asks for Blacksmithing) that opens a lock of
    // this lockpicking; nullptr when it has none.
    static Item* KeyFor(Player* player, int32 need);
    // Why this character cannot open the container now (nullptr: it can). Lasting reasons ("a key", "no lockpicking",
    // "lockpicking too low", "wrapped") are those IsLastingReason answers true for: such a container goes the usual way.
    char const* CannotOpen(Player* player, Item* item) const;
    static bool IsLastingReason(char const* reason);
    // Opens a container in the character's bags: a lock is picked with Pick Lock first, the loot (and its money) goes
    // into the bags and so through the loot filter like other loot, the container is used up (one of a stack for the
    // clams). `how` names where it came from for the log. Returns false when it could not.
    bool OpenContainer(Player* player, Item* item, char const* how);
    // A line in recent loot and in the activity log for an item the owner's order moved out of or into the guild storage
    // (2026-11-13: every movement of the storage leaves a line). `character` did it; `how` names the order.
    void LogStorageMove(uint32 owner, ObjectGuid character, std::string const& name, uint32 entry, int32 randomProperty,
        uint32 count, bool deposited, std::string const& how);
    // Sells up to `count` (0 = all) of a kind of the storage at its vendor price; the copper goes into the guild bank
    // (docs/storage.md section 10), `player` is who ordered it. Returns how many; 0 with `error` (nothing, no_price,
    // gold_limit = the adventure guild has no WoW guild).
    uint32 SellStored(uint32 owner, Player* player, uint32 entry, int32 randomProperty, uint32 count, uint64& money,
        std::string& error);
    // Destroys up to `count` (0 = all) of a kind of the storage that no vendor buys (the owner, 2026-11-13: [quote left out]); `player` is who ordered it.
    // Returns how many; 0 with `error` (nothing, has_price).
    uint32 DeleteStored(uint32 owner, Player* player, uint32 entry, int32 randomProperty, uint32 count, std::string& error);
    // The Enchanting skill a kind needs to be disenchanted out of the storage on the owner's order (green and blue
    // weapons and armour CraftOrders::CanDisenchant takes), -1 when it cannot.
    static int32 StorageDisenchantSkill(ItemTemplate const* proto);
    // The guild character in the world with the most Enchanting skill (nullptr when none), and that skill
    // (CraftOrders::BestEnchanter for the owner's guild).
    static Player* BestEnchanter(uint32 owner, uint32& skill);
    // The owner's Disenchant on one item of a kind: CraftOrders::DisenchantStored does it with the guild's best
    // enchanter in the world (materials into the storage, the activity line), and the storage's log gets a line that
    // names the enchanter, `forWhom` and the materials.
    GuildAdventure::StorageDisenchant DisenchantFromStorage(uint32 owner, uint32 entry, int32 randomProperty,
        std::string const& forWhom);
    // The highest quality that counts as junk for what this character loots, or for the guild (the storage, the
    // addon's built-in rule); -1 when junk is not sold (the bot settings LootFilter.SellJunk / .SellJunkQuality).
    static int32 JunkQuality(Player const* looter);
    static int32 JunkQualityOfGuild(uint32 owner);
    // What of the guild storage is junk now (the guild's setting); with `sell` it is sold at the vendor price and the
    // copper goes into the guild bank; `player` is who ordered it.
    GuildAdventure::JunkSale SellStoredJunk(uint32 owner, Player* player, bool sell);

private:
    // What an AI-played character carries of one kind (`kind`: an item entry is of it) through the loot filter now.
    uint32 StoreCarried(Player* player, bool (*kind)(uint32), char const* what);
    LootService() = default;

    struct GuildLoot
    {
        GuildAdventure::LootSettings settings;
        std::vector<GuildAdventure::LootRules::Rule> rules;   // evaluation order
        std::deque<GuildAdventure::LootLogEntry> log;          // newest last
        std::vector<GuildAdventure::LootLogEntry> newLog;      // not pushed yet
        uint64 pendingSold = 0;
        uint32 pendingDisenchanted = 0;
        uint32 pendingDeleted = 0;
        uint32 pendingStored = 0;
        bool loaded = false;
    };

    struct Looted
    {
        ObjectGuid character;
        ObjectGuid item;
    };

    // What the owner's own character looted in one tick, for one chat line.
    struct ChatBatch
    {
        uint32 kept = 0;
        uint32 stored = 0;
        uint32 sold = 0;
        uint64 money = 0;
        uint32 disenchanted = 0;
        uint32 deleted = 0;
    };

    GuildLoot& Guild(uint32 owner);   // world thread
    void Process(Looted const& looted, std::unordered_map<ObjectGuid, ChatBatch>& chat, bool fromBags = false);
    void SweepFullBags();
    // Every few seconds: AI-played guild characters open the containers in their bags, and (bot setting
    // Storage.OpenContainers) take those of the guild storage out to open them.
    void OpenContainersStep();
    // The containers of the guild storage a character takes out and opens now (at most `limit`); returns how many.
    uint32 OpenStoredContainers(uint32 owner, Player* player, uint32 limit);
    GuildAdventure::LootRules::ItemFacts Facts(Player* player, Item* item) const;
    void Act(Player* player, uint32 owner, GuildLoot& guild, Item* item, uint8 action, ChatBatch* chat);
    void SaveRules(uint32 owner, GuildLoot const& guild) const;
    void SaveSettings(uint32 owner, GuildLoot const& guild) const;
    void FlushTotals();
    // The character the AI plays (a bot on its home session, the leader while the owner plays a bot, a watched one).
    static bool IsAIPlayed(Player* player);
    // Junk by its kind alone (quality, class, the index below).
    bool IsJunkKind(ItemTemplate const* proto, int32 maxQuality) const;
    // White gear: whether a guild character in the world would wear it (an upgrade for it); cached for a minute.
    bool WornByGuild(uint32 owner, ItemTemplate const* proto, int32 randomProperty);
    // Items profession recipes use and items quests ask for (startup).
    void LoadNeededItems();

    std::unordered_map<uint32, GuildLoot> _guilds; // by owner account, world thread
    std::unordered_map<ObjectGuid, uint32> _lastSweep; // character -> getMSTime() of its last bag sweep
    std::unordered_map<ObjectGuid, uint32> _lastCardLook; // character -> getMSTime() of its last look for Darkmoon cards
    uint32 _sweepTimer = 0;
    uint32 _openTimer = 0;
    std::unordered_map<ObjectGuid, uint32> _lastStoredLockbox; // rogue -> getMSTime() of the last lockbox it took out
    uint32 _nextRuleId = 1;
    uint32 _totalsTimer = 0;

    mutable std::mutex _queueLock;
    std::vector<Looted> _queue;

    // The auto loot switch per owner, read on map threads.
    mutable std::mutex _autoLootLock;
    std::unordered_map<uint32, bool> _autoLoot;
    // Never junk: reagents of profession recipes, quest objectives and quest source items (world thread).
    std::unordered_set<uint32> _neededItems;
    std::unordered_set<uint32> _spellTools; // items a profession recipe names as its tool (SpellInfo::Totem)
    // (owner << 32 | item entry) -> getMSTime() of the check and whether a guild character would wear it.
    std::unordered_map<uint64, std::pair<uint32, bool>> _wornCache;
};

#define sLootService LootService::Instance()

#endif
