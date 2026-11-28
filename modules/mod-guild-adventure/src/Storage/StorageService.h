/*
 * StorageService: the guild storage (the owner, 2026-11-12: [quote left out]; [quote left out]). Design: docs/storage.md.
 *
 * One unlimited store per adventure guild that every guild character (the leader and its bots) uses, the idea of
 * ForgottenLand's mod-endless-storage (the owner's own module) without its AIO/Eluna layer and widened from "materials of
 * one character" to "every item without its own state, shared by the guild": a kind of item is its entry plus its
 * random property ("of the Bear"), and the store keeps an amount per kind. Items that carry their own state never go
 * in (quest items, soulbound, conjured, timed, enchanted, damaged, wrapped, written-on, used charges, a bag with
 * something in it): RefuseReason says why. One exception (DL1, docs/storage.md "Soulbound loot"): a bind-on-pickup
 * weapon or armour piece a guild character looted and never put on (MarkUnworn at loot time, ForgetUnworn on equip).
 *
 * Ways in: the loot filter (LootService) puts looted items there, a character deposits from its bags (the addon's
 * "Deposit materials" and Alt-click, or .ga storage deposit), disenchanting puts its materials there. Ways out: the
 * owner takes items into the bags of the character he plays (the addon), and spells take missing reagents from it
 * (core patch CP-7, OnPlayerCheckReagent / OnPlayerTakeReagent): any guild character, the leader and the bots, crafts
 * from the storage without taking the materials out first.
 *
 * The owner of a character is found through its WoW guild (Player::GetGuildId, readable on the character's map
 * thread): every guild character is a member of its adventure guild's WoW guild. The map from WoW guild to owner is
 * refreshed on the world thread.
 *
 * Thread safety: the store is behind one mutex, because loot and reagents are handled on map threads (auto loot,
 * spells) as well as on the world thread (loot filter, protocol). Changes are written to ga_guild_storage in one
 * transaction every few seconds (Flush), and the addon gets them as deltas (TakeChanges).
 */

#ifndef MOD_GUILD_ADVENTURE_STORAGE_SERVICE_H
#define MOD_GUILD_ADVENTURE_STORAGE_SERVICE_H

#include "Define.h"
#include "ObjectGuid.h"
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class Item;
class Player;
struct ItemTemplate;

namespace GuildAdventure
{
    struct StorageItem
    {
        uint32 entry = 0;
        int32 randomProperty = 0;  // Item::GetItemRandomPropertyId: >0 property, <0 suffix, 0 none
        uint32 amount = 0;
        uint8 itemClass = 0;
        uint8 subClass = 0;
        uint32 reserve = 0;        // how many the bots leave in the storage (ListAll, TakeChanges)
    };

    // Category of an item in the storage window (docs/protocol.md §7.10); the addon shows the same list. With the item's
    // entry the Darkmoon Faire cards and decks are a category of their own ("darkmoon", StorageService::IsDarkmoonCard),
    // and so are the reputation turn-ins ("reputation", StorageService::IsReputationTurnIn).
    char const* StorageCategory(uint8 itemClass, uint8 subClass, uint32 entry = 0);
}

class StorageService
{
public:
    static StorageService& Instance();

    // Startup (world thread, after GuildAdventureMgr::LoadState): ga_guild_storage.
    void Load();
    // World thread, every tick: refreshes the WoW guild -> owner map and writes the changed rows every few seconds.
    void Update(uint32 diff);
    // Shutdown: writes what is left synchronously.
    void FlushNow();

    // --- what may go in ---
    // Why an item kind can never go into the storage (nullptr = it can): quest items, conjured, timed.
    static char const* RefuseReason(ItemTemplate const* proto);
    // Why this item cannot go in (nullptr = it can): the kind's reasons plus its own state.
    static char const* RefuseReason(Item const* item);
    // The materials of mod-endless-storage's "Deposit All Materials": stackable trade goods and gems, recipes,
    // stackable food and drink.
    static bool IsMaterial(ItemTemplate const* proto);
    // A Darkmoon Faire card or deck (the owner, 2026-11-25: [quote left out]): quest items to the game, but they go into the guild storage. Found in the item and spell
    // data (any thread after startup): a deck is a quest item that starts a quest and that eight or more quest items
    // make with their use spell (the Ace through Eight of the set); the cards are those eight. Map fragments and the
    // halves of a book make their quest item too, but in twos and threes.
    static bool IsDarkmoonCard(uint32 entry);
    // A quest item a repeatable (or daily, weekly) quest that gives reputation asks for: the Argent Dawn's Core of
    // Elements, Bone Fragments and Scourgestones, the Darkmoon Faire's Evil Bat Eyes (the owner, 2026-11-25: into the guild
    // storage, the design log section 7 RT1). The storage takes them, soulbound ones too; a category of their own.
    static bool IsReputationTurnIn(uint32 entry);

    // --- never-worn bind-on-pickup loot (DL1; any thread) ---
    // The kinds this is for: bind-on-pickup weapons and armour of green quality or better.
    static bool IsUnwornKind(ItemTemplate const* proto);
    // A guild character looted, won in a roll, got as a quest reward (or took out of the storage) this soulbound piece:
    // it is marked as never worn, kept in ga_unworn_loot across restarts. A piece a GM created never is.
    void MarkUnworn(Player* holder, Item* item, char const* how);
    // The piece was put on (OnPlayerEquip) or left the bags into the storage: the mark ends for good.
    void ForgetUnworn(Item const* item, char const* why);
    // Whether this soulbound piece may go into the storage: marked, never equipped since, in the bags of a character
    // of the guild it was marked for, and the bot setting Storage.UnwornBound is on for that character.
    bool TakesUnworn(Item const* item) const;
    // Whether the piece carries the never-worn mark at all (not equipped), whatever the setting says: a piece just
    // taken out of the storage is soulbound to the taker and marked (DungeonRoles' role store looks for it so).
    bool IsMarkedUnworn(Item const* item) const;

    // --- gear that came out of the storage (any thread) ---
    // A weapon or armour piece the storage handed to a guild character: kept in ga_storage_origin across restarts, so
    // the worn-gear rule (LootService::HandleWornGear) never disenchants or sells it, whatever it became since.
    void MarkFromStorage(Player* holder, Item* item);
    bool CameFromStorage(Item const* item) const;

    // --- owners ---
    // The adventure guild (owner account) a character's storage is, 0 if none. Any thread.
    uint32 GetOwnerOf(Player const* player) const;

    // --- reserves (the owner, 2026-11-13: [quote left out]; docs/storage.md "Reserves") ---
    // GetAmount, GetAmountOfEntry, List, Remove and Withdraw are what every automatic use calls (the bots' supplies,
    // quest items, crafting and its reagents, gear, ammunition, poisons, containers, disenchanting for orders, the
    // auction surplus, Sell junk): they see and take only what is above the reserve of a kind. While an OwnerOrder
    // lives on this thread the reserve does not count: the owner's own Take, Sell, Delete and Disenchant, and what he
    // allowed for a dungeon run (bot setting Storage.ReserveForDungeons).
    struct OwnerOrder
    {
        OwnerOrder();
        ~OwnerOrder();
        OwnerOrder(OwnerOrder const&) = delete;
        OwnerOrder& operator=(OwnerOrder const&) = delete;
    };
    static bool IsOwnerOrder();
    uint32 GetReserve(uint32 owner, uint32 entry, int32 randomProperty = 0) const;
    // Sets how many of a kind the bots leave in the storage (0: none); ga_storage_reserve, the addon gets a delta.
    void SetReserve(uint32 owner, uint32 entry, int32 randomProperty, uint32 reserve);

    // --- the store (any thread) ---
    // Adds 'count' of a kind; returns the new amount (0 if the kind cannot go in).
    uint32 Add(uint32 owner, ItemTemplate const* proto, int32 randomProperty, uint32 count);
    // Removes up to 'count' of a kind (of what is above its reserve, see above); returns how many were removed.
    uint32 Remove(uint32 owner, uint32 entry, int32 randomProperty, uint32 count);
    // What of a kind may be used (above its reserve, see above).
    uint32 GetAmount(uint32 owner, uint32 entry, int32 randomProperty = 0) const;
    // The same over every random property of the entry (a green "of the Bear" and "of the Eagle" together).
    uint32 GetAmountOfEntry(uint32 owner, uint32 entry) const;
    // The kinds with what may be used of each (kinds held back entirely by their reserve are left out).
    std::vector<GuildAdventure::StorageItem> List(uint32 owner) const;
    // What the storage holds, the reserves included (the addon, the owner's commands, counting).
    uint32 GetStored(uint32 owner, uint32 entry, int32 randomProperty = 0) const;
    uint32 GetStoredOfEntry(uint32 owner, uint32 entry) const;
    std::vector<GuildAdventure::StorageItem> ListAll(uint32 owner) const;
    // Kinds and items in a guild's storage.
    std::pair<uint32, uint64> GetTotals(uint32 owner) const;

    // --- moving items (world thread, or the character's own map thread) ---
    // Moves one stack from the character's bags into the guild storage (the stack is destroyed). Returns the count
    // moved, 0 with 'error' set when it cannot.
    // Ammunition is only taken when a guild character made it (`crafted`; Ammunition.h): never from loot, a sweep
    // of the bags or a deposit by hand.
    // (takeBound: a piece the storage handed out in the same step that the equipping bound without it being worn, the
    // only soulbound piece besides never-worn loot that goes back; StorageSupply.)
    uint32 DepositItem(Player* player, Item* item, std::string& error, bool crafted = false, bool takeBound = false);
    // A guild bot that is dismissed (RosterService::Dismiss; the owner, 2026-11-28) leaves to the guild storage what it takes:
    // unlike DepositItem the item may be anywhere on the character (worn, in a bag, in its bank), ammunition goes in, and
    // wear is mended first (the storage keeps kinds, not their state). Soulbound pieces stay (they go with the character).
    // Returns the count moved, 0 with 'error' set; the item is destroyed.
    uint32 DepositDismissed(Player* player, Item* item, std::string& error);
    // Deposits every material (IsMaterial) in the character's bags; returns the stacks and items moved. `moved`, when
    // given, gets what of each kind went in (amount = how many).
    // `keep`: what the caller keeps in the bags whatever it is (a bot keeps what a quest in its log asks for).
    std::pair<uint32, uint32> DepositMaterials(Player* player, std::vector<GuildAdventure::StorageItem>* moved = nullptr,
        std::function<bool(Item*)> const& keep = {});
    // Takes up to 'count' of a kind into the character's bags (as many as fit); returns how many, 0 with 'error'.
    uint32 Withdraw(Player* player, uint32 entry, int32 randomProperty, uint32 count, std::string& error);
    // A character whose bags changed together with the storage (or with its money, for what the loot filter sold):
    // its inventory and money are written with the storage's next flush, in the same transaction. The storage is
    // written every few seconds, a character's bags only with its next save (up to a quarter of an hour later): a
    // server that is killed in between would otherwise come back with a deposited stack both in the storage and in
    // the bags, and without what was taken out. Any thread.
    void Touch(Player const* player);

    // --- deltas for the addon (world thread) ---
    // Owners whose storage changed since their last TakeChanges.
    std::vector<uint32> GetChangedOwners() const;
    // The kinds of 'owner' that changed (amount 0 = gone), and forgets them.
    std::vector<GuildAdventure::StorageItem> TakeChanges(uint32 owner);

    // --- core patch CP-7 (any thread): spells take the reagents the bags lack from the guild storage ---
    void OnCheckReagent(Player* player, uint32 itemId, uint32 count, bool& enough) const;
    static bool KeepsReserve(Player* player);
    void OnTakeReagent(Player* player, uint32 itemId, uint32& count);

private:
    StorageService() = default;

    using Key = std::pair<uint32, int32>; // entry, random property

    void MarkChanged(uint32 owner, Key const& key); // _lock held
    uint32 ReserveLocked(uint32 owner, Key const& key) const; // _lock held
    uint32 RemoveImpl(uint32 owner, uint32 entry, int32 randomProperty, uint32 count, bool keepReserve);
    // One activity line per character and kind now and then for a take its reserve refused.
    void NoteReserveRefusal(Player* player, uint32 entry, uint32 stored, uint32 reserve);
    void RefreshOwners();                           // world thread

    mutable std::mutex _lock;
    std::unordered_map<uint32, std::map<Key, GuildAdventure::StorageItem>> _items; // by owner account
    std::set<std::pair<uint32, Key>> _dirty;                                        // rows to write
    std::set<ObjectGuid> _touched;                                                  // characters to save with them
    std::unordered_map<uint32, std::set<Key>> _changed;                             // deltas for the addon
    std::unordered_map<uint32, std::map<Key, uint32>> _reserves;                    // by owner account
    std::map<std::pair<ObjectGuid, uint32>, uint32> _reserveNoted;                  // character, entry -> getMSTime
    std::unordered_map<uint32, uint32> _ownerByGuild;                               // WoW guild id -> owner account
    mutable std::mutex _unwornLock;
    std::unordered_map<uint32, uint32> _unworn;                                     // item guid counter -> owner account
    std::unordered_set<uint32> _fromStorage;                                        // item guid counters (under _unwornLock)
    uint32 _flushTimer = 0;
    uint32 _ownerTimer = 0;
};

#define sStorageService StorageService::Instance()

#endif
