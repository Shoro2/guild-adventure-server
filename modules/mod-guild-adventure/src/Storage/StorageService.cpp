/*
 * StorageService implementation. See StorageService.h and docs/storage.md.
 */

#include "StorageService.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Spell.h"
#include "Bag.h"
#include "BotSettings.h"
#include "DatabaseEnv.h"
#include "GuildAdventureConfig.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildAdventureUtil.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <limits>

using namespace GuildAdventure;

namespace
{
    constexpr uint32 FlushIntervalMs = 5 * IN_MILLISECONDS;
    constexpr uint32 OwnerRefreshMs = 2 * IN_MILLISECONDS;
    constexpr uint32 MaxAmount = 0x7FFFFFFF;

    // Enchantment slots a player (not the item's random property) fills: permanent and temporary enchants, gems,
    // the socket bonus and the prismatic socket. PROP_ENCHANTMENT_SLOT_* belong to the random property.
    constexpr EnchantmentSlot PlayerEnchantSlots[] = { PERM_ENCHANTMENT_SLOT, TEMP_ENCHANTMENT_SLOT, SOCK_ENCHANTMENT_SLOT,
        SOCK_ENCHANTMENT_SLOT_2, SOCK_ENCHANTMENT_SLOT_3, BONUS_ENCHANTMENT_SLOT, PRISMATIC_ENCHANTMENT_SLOT };

    // Every bag slot the deposits look at: the backpack and the four bags (never the bank, the keyring or equipment).
    template <typename Fn>
    void ForEachBagItem(Player* player, Fn&& fn)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                fn(item);
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                    if (Item* item = bag->GetItemByPos(uint8(i)))
                        fn(item);
    }

    bool IsInBags(Player* player, Item* item)
    {
        if (item->IsEquipped())
            return false;
        uint8 const bag = item->GetBagSlot();
        uint8 const slot = item->GetSlot();
        if (bag == INVENTORY_SLOT_BAG_0)
            return slot >= INVENTORY_SLOT_ITEM_START && slot < INVENTORY_SLOT_ITEM_END;
        return bag >= INVENTORY_SLOT_BAG_START && bag < INVENTORY_SLOT_BAG_END && player->GetBagByPos(bag);
    }
}

char const* GuildAdventure::StorageCategory(uint8 itemClass, uint8 subClass, uint32 entry)
{
    if (entry && StorageService::IsDarkmoonCard(entry))
        return "darkmoon";
    if (entry && StorageService::IsReputationTurnIn(entry))
        return "reputation";
    switch (itemClass)
    {
        case ITEM_CLASS_TRADE_GOODS:
            switch (subClass)
            {
                case ITEM_SUBCLASS_CLOTH:            return "cloth";
                case ITEM_SUBCLASS_LEATHER:          return "leather";
                case ITEM_SUBCLASS_METAL_STONE:      return "metal";
                case ITEM_SUBCLASS_HERB:             return "herb";
                case ITEM_SUBCLASS_MEAT:             return "meat";
                case ITEM_SUBCLASS_ELEMENTAL:        return "elemental";
                case ITEM_SUBCLASS_ENCHANTING:       return "enchanting";
                case ITEM_SUBCLASS_JEWELCRAFTING:    return "gems";
                default:                             return "trade";
            }
        case ITEM_CLASS_GEM:        return "gems";
        case ITEM_CLASS_RECIPE:     return "recipes";
        case ITEM_CLASS_CONSUMABLE: return "consumables";
        case ITEM_CLASS_WEAPON:     return "weapons";
        case ITEM_CLASS_ARMOR:      return "armor";
        case ITEM_CLASS_CONTAINER:
        case ITEM_CLASS_QUIVER:     return "bags";
        case ITEM_CLASS_REAGENT:
        case ITEM_CLASS_PROJECTILE: return "consumables";
        default:                    return "other";
    }
}

StorageService& StorageService::Instance()
{
    static StorageService instance;
    return instance;
}

void StorageService::Load()
{
    std::lock_guard<std::mutex> lock(_lock);
    _items.clear();
    _dirty.clear();
    _changed.clear();

    uint32 rows = 0;
    uint32 dropped = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT owner_account, item_entry, random_property, amount FROM ga_guild_storage"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const owner = fields[0].Get<uint32>();
            uint32 const entry = fields[1].Get<uint32>();
            int32 const randomProperty = fields[2].Get<int32>();
            uint32 const amount = fields[3].Get<uint32>();
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            if (!proto || !amount)
            {
                ++dropped; // an item that no longer exists stays in the table, untouched
                continue;
            }
            StorageItem item;
            item.entry = entry;
            item.randomProperty = randomProperty;
            item.amount = amount;
            item.itemClass = uint8(proto->Class);
            item.subClass = uint8(proto->SubClass);
            _items[owner][{ entry, randomProperty }] = item;
            ++rows;
        } while (result->NextRow());
    }

    LOG_INFO("module", "mod-guild-adventure: loaded {} guild storage rows of {} guilds{}", rows, uint32(_items.size()),
        dropped ? Acore::StringFormat(" ({} rows of unknown or empty items ignored)", dropped) : std::string());

    // The reserves (docs/storage.md "Reserves").
    _reserves.clear();
    uint32 reserves = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT owner_account, item_entry, random_property, reserve FROM ga_storage_reserve "
        "WHERE reserve > 0"))
    {
        do
        {
            Field* fields = result->Fetch();
            _reserves[fields[0].Get<uint32>()][{ fields[1].Get<uint32>(), fields[2].Get<int32>() }] = fields[3].Get<uint32>();
            ++reserves;
        } while (result->NextRow());
    }
    LOG_INFO("module", "mod-guild-adventure: {} reserves in the guild storages", reserves);

    // Never-worn bind-on-pickup loot (DL1): marks of items that no longer exist (sold, destroyed) go first.
    std::lock_guard<std::mutex> unwornLock(_unwornLock);
    _unworn.clear();
    CharacterDatabase.DirectExecute("DELETE u FROM ga_unworn_loot u LEFT JOIN item_instance i ON i.guid = u.item_guid "
        "WHERE i.guid IS NULL");
    if (QueryResult result = CharacterDatabase.Query("SELECT item_guid, owner_account FROM ga_unworn_loot"))
    {
        do
        {
            Field* fields = result->Fetch();
            _unworn[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
        } while (result->NextRow());
    }
    LOG_INFO("module", "mod-guild-adventure: {} never-worn bind-on-pickup pieces in the bags of guild characters", uint32(_unworn.size()));

    // Gear that came out of the storage (the worn-gear rule leaves it alone): the same clean-up first.
    _fromStorage.clear();
    CharacterDatabase.DirectExecute("DELETE o FROM ga_storage_origin o LEFT JOIN item_instance i ON i.guid = o.item_guid "
        "WHERE i.guid IS NULL");
    if (QueryResult result = CharacterDatabase.Query("SELECT item_guid FROM ga_storage_origin"))
    {
        do
            _fromStorage.insert(result->Fetch()[0].Get<uint32>());
        while (result->NextRow());
    }
    LOG_INFO("module", "mod-guild-adventure: {} pieces of gear that came out of a guild storage", uint32(_fromStorage.size()));
}

void StorageService::Update(uint32 diff)
{
    _ownerTimer += diff;
    if (_ownerTimer >= OwnerRefreshMs || _ownerByGuild.empty())
    {
        _ownerTimer = 0;
        RefreshOwners();
    }

    _flushTimer += diff;
    if (_flushTimer >= FlushIntervalMs)
    {
        _flushTimer = 0;
        std::set<std::pair<uint32, Key>> dirty;
        std::set<ObjectGuid> touched;
        std::vector<std::pair<std::pair<uint32, Key>, uint32>> rows;
        {
            std::lock_guard<std::mutex> lock(_lock);
            dirty.swap(_dirty);
            touched.swap(_touched);
            for (auto const& [owner, key] : dirty)
            {
                uint32 amount = 0;
                auto guild = _items.find(owner);
                if (guild != _items.end())
                {
                    auto itr = guild->second.find(key);
                    if (itr != guild->second.end())
                        amount = itr->second.amount;
                }
                rows.push_back({ { owner, key }, amount });
            }
        }
        if (rows.empty() && touched.empty())
            return;

        // Absolute amounts in one transaction: a later flush always wins, whatever happened in between.
        uint32 const now = Now();
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        // The bags (and money) of the characters that deposited or took something, in the same transaction (Touch).
        for (ObjectGuid const& guid : touched)
            if (Player* player = ObjectAccessor::FindPlayer(guid); player && player->IsInWorld() && !player->IsDuringRemoveFromWorld())
                player->SaveInventoryAndGoldToDB(trans);
        for (auto const& [id, amount] : rows)
        {
            uint32 const owner = id.first;
            Key const& key = id.second;
            if (!amount)
                trans->Append("DELETE FROM ga_guild_storage WHERE owner_account = {} AND item_entry = {} AND random_property = {}",
                    owner, key.first, key.second);
            else
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(key.first);
                trans->Append("REPLACE INTO ga_guild_storage (owner_account, item_entry, random_property, amount, item_class, "
                    "item_subclass, updated_at) VALUES ({}, {}, {}, {}, {}, {}, {})", owner, key.first, key.second, amount,
                    proto ? proto->Class : 0, proto ? proto->SubClass : 0, now);
            }
        }
        CharacterDatabase.CommitTransaction(trans);
    }
}

void StorageService::FlushNow()
{
    // Same rows as Update, written synchronously (shutdown).
    std::set<std::pair<uint32, Key>> dirty;
    {
        std::lock_guard<std::mutex> lock(_lock);
        dirty.swap(_dirty);
    }
    uint32 const now = Now();
    for (auto const& [owner, key] : dirty)
    {
        uint32 amount = GetAmount(owner, key.first, key.second);
        if (!amount)
            CharacterDatabase.DirectExecute("DELETE FROM ga_guild_storage WHERE owner_account = {} AND item_entry = {} AND "
                "random_property = {}", owner, key.first, key.second);
        else
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(key.first);
            CharacterDatabase.DirectExecute("REPLACE INTO ga_guild_storage (owner_account, item_entry, random_property, amount, "
                "item_class, item_subclass, updated_at) VALUES ({}, {}, {}, {}, {}, {}, {})", owner, key.first, key.second,
                amount, proto ? proto->Class : 0, proto ? proto->SubClass : 0, now);
        }
    }
}

void StorageService::RefreshOwners()
{
    std::unordered_map<uint32, uint32> owners;
    for (auto const& [owner, guild] : sGuildAdventureMgr.GetGuilds())
        if (guild.guildId)
            owners[guild.guildId] = owner;

    std::lock_guard<std::mutex> lock(_lock);
    _ownerByGuild.swap(owners);
}

// ---------------------------------------------------------------------------------------------------------------
// What may go in
// ---------------------------------------------------------------------------------------------------------------

namespace
{
    // The Darkmoon Faire cards and decks (IsDarkmoonCard), found once from the item and spell data.
    std::once_flag DarkmoonOnce;
    std::unordered_set<uint32> DarkmoonItems;

    void FindDarkmoonCards()
    {
        std::unordered_map<uint32, std::vector<uint32>> cardsOf; // deck -> the quest items whose use makes it
        for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
        {
            if (proto.Class != ITEM_CLASS_QUEST)
                continue;
            for (auto const& use : proto.Spells)
            {
                if (use.SpellId <= 0 || use.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                    continue;
                SpellInfo const* spell = sSpellMgr->GetSpellInfo(uint32(use.SpellId));
                if (!spell)
                    continue;
                for (SpellEffectInfo const& effect : spell->Effects)
                {
                    if ((effect.Effect != SPELL_EFFECT_CREATE_ITEM && effect.Effect != SPELL_EFFECT_CREATE_ITEM_2) || !effect.ItemType
                        || effect.ItemType == entry)
                        continue;
                    ItemTemplate const* deck = sObjectMgr->GetItemTemplate(effect.ItemType);
                    if (deck && deck->Class == ITEM_CLASS_QUEST && deck->StartQuest)
                        cardsOf[deck->ItemId].push_back(entry);
                }
            }
        }
        uint32 decks = 0;
        for (auto& [deck, cards] : cardsOf)
        {
            std::sort(cards.begin(), cards.end());
            cards.erase(std::unique(cards.begin(), cards.end()), cards.end());
            if (cards.size() < 8)
                continue;
            ++decks;
            DarkmoonItems.insert(deck);
            DarkmoonItems.insert(cards.begin(), cards.end());
        }
        LOG_INFO("module", "mod-guild-adventure: {} Darkmoon Faire decks and {} cards go into the guild storage (category Darkmoon cards)",
            decks, DarkmoonItems.size() - decks);
    }
}

bool StorageService::IsDarkmoonCard(uint32 entry)
{
    std::call_once(DarkmoonOnce, FindDarkmoonCards);
    return DarkmoonItems.count(entry) != 0;
}

namespace
{
    // The reputation turn-ins (IsReputationTurnIn), found once from the quest data.
    std::once_flag TurnInOnce;
    std::unordered_set<uint32> TurnInItems;

    void FindTurnIns()
    {
        for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
        {
            if (!quest->IsRepeatable() && !quest->IsDailyOrWeekly())
                continue;
            bool reputation = false;
            for (uint8 i = 0; i < QUEST_REPUTATIONS_COUNT; ++i)
                if (quest->RewardFactionId[i] && (quest->RewardFactionValueId[i] || quest->RewardFactionValueIdOverride[i]))
                    reputation = true;
            if (!reputation)
                continue;
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (ItemTemplate const* proto = quest->RequiredItemId[i] ? sObjectMgr->GetItemTemplate(quest->RequiredItemId[i]) : nullptr)
                    // Quest items only (what the storage refused, S3): Runecloth, which the Alliance's and the Horde's
                    // cloth quartermasters take, is a trade good and stays one.
                    if (proto->Class == ITEM_CLASS_QUEST || proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_QUEST_ITEM1)
                        TurnInItems.insert(proto->ItemId);
        }
        LOG_INFO("module", "mod-guild-adventure: {} kinds of reputation turn-ins go into the guild storage (category Reputation turn-ins)",
            TurnInItems.size());
    }
}

bool StorageService::IsReputationTurnIn(uint32 entry)
{
    std::call_once(TurnInOnce, FindTurnIns);
    return TurnInItems.count(entry) != 0;
}

char const* StorageService::RefuseReason(ItemTemplate const* proto)
{
    if (!proto)
        return "unknown item";
    // The Darkmoon Faire cards and decks are quest items to the game; they go in all the same (the owner, 2026-11-25).
    if ((proto->Class == ITEM_CLASS_QUEST || proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_QUEST_ITEM1
        || proto->StartQuest != 0) && !IsDarkmoonCard(proto->ItemId) && !IsReputationTurnIn(proto->ItemId))
        return "quest item";
    if (proto->HasFlag(ITEM_FLAG_CONJURED))
        return "conjured";
    if (proto->Duration > 0)
        return "timed";
    if (proto->Class == ITEM_CLASS_KEY)
        return "key";
    return nullptr;
}

char const* StorageService::RefuseReason(Item const* item)
{
    if (!item)
        return "no item";
    if (char const* reason = RefuseReason(item->GetTemplate()))
        return reason;
    // (A soulbound reputation turn-in, the Scourgestones, goes in all the same: it comes out bound to the one who takes
    // it, as a never-worn piece does. The owner, 2026-11-25, RT1.)
    if (item->IsBoundAccountWide() || (item->IsSoulBound() && !sStorageService.TakesUnworn(item) && !IsReputationTurnIn(item->GetEntry())))
        return "soulbound";
    if (item->IsWrapped())
        return "wrapped";
    if (!item->GetText().empty())
        return "has text";
    if (item->GetUInt32Value(ITEM_FIELD_DURABILITY) < item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
        return "damaged";
    for (EnchantmentSlot slot : PlayerEnchantSlots)
        if (item->GetEnchantmentId(slot))
            return "enchanted";
    ItemTemplate const* proto = item->GetTemplate();
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        if (proto->Spells[i].SpellId && item->GetSpellCharges(i) != proto->Spells[i].SpellCharges)
            return "used charges";
    if (item->IsBag())
        if (Bag const* bag = item->ToBag())
            if (!bag->IsEmpty())
                return "not empty";
    return nullptr;
}

bool StorageService::IsMaterial(ItemTemplate const* proto)
{
    if (!proto || RefuseReason(proto))
        return false;
    if (proto->Class == ITEM_CLASS_RECIPE)
        return true;
    if (proto->Class == ITEM_CLASS_CONSUMABLE && proto->SubClass == ITEM_SUBCLASS_FOOD && proto->GetMaxStackSize() > 1)
        return true;
    return (proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_GEM) && proto->GetMaxStackSize() > 1;
}

// ---------------------------------------------------------------------------------------------------------------
// The store
// ---------------------------------------------------------------------------------------------------------------

uint32 StorageService::GetOwnerOf(Player const* player) const
{
    if (!player || !IsModuleEnabled() || !GetSettings().storageEnable)
        return 0;
    uint32 const guildId = player->GetGuildId();
    if (!guildId)
        return 0;
    std::lock_guard<std::mutex> lock(_lock);
    auto itr = _ownerByGuild.find(guildId);
    return itr == _ownerByGuild.end() ? 0 : itr->second;
}

void StorageService::Touch(Player const* player)
{
    if (!player)
        return;
    std::lock_guard<std::mutex> lock(_lock);
    _touched.insert(player->GetGUID());
}

void StorageService::MarkChanged(uint32 owner, Key const& key)
{
    _dirty.insert({ owner, key });
    _changed[owner].insert(key);
}

uint32 StorageService::Add(uint32 owner, ItemTemplate const* proto, int32 randomProperty, uint32 count)
{
    if (!owner || !proto || !count || RefuseReason(proto))
        return 0;
    std::lock_guard<std::mutex> lock(_lock);
    Key const key{ proto->ItemId, randomProperty };
    StorageItem& item = _items[owner][key];
    item.entry = proto->ItemId;
    item.randomProperty = randomProperty;
    item.itemClass = uint8(proto->Class);
    item.subClass = uint8(proto->SubClass);
    item.amount = uint32(std::min<uint64>(uint64(item.amount) + count, MaxAmount));
    MarkChanged(owner, key);
    return item.amount;
}

namespace
{
    thread_local int t_ownerOrder = 0;
    constexpr uint32 ReserveNoteMs = 30 * MINUTE * IN_MILLISECONDS; // one refusal line per character and kind
}

StorageService::OwnerOrder::OwnerOrder()
{
    ++t_ownerOrder;
}

StorageService::OwnerOrder::~OwnerOrder()
{
    --t_ownerOrder;
}

bool StorageService::IsOwnerOrder()
{
    return t_ownerOrder > 0;
}

uint32 StorageService::ReserveLocked(uint32 owner, Key const& key) const
{
    auto guild = _reserves.find(owner);
    if (guild == _reserves.end())
        return 0;
    auto itr = guild->second.find(key);
    return itr == guild->second.end() ? 0 : itr->second;
}

uint32 StorageService::GetReserve(uint32 owner, uint32 entry, int32 randomProperty) const
{
    std::lock_guard<std::mutex> lock(_lock);
    return ReserveLocked(owner, { entry, randomProperty });
}

void StorageService::SetReserve(uint32 owner, uint32 entry, int32 randomProperty, uint32 reserve)
{
    if (!owner || !entry)
        return;
    reserve = std::min<uint32>(reserve, MaxAmount);
    {
        std::lock_guard<std::mutex> lock(_lock);
        Key const key{ entry, randomProperty };
        if (reserve)
            _reserves[owner][key] = reserve;
        else if (auto guild = _reserves.find(owner); guild != _reserves.end())
            guild->second.erase(key);
        // The addon's row shows it (a delta of the kind; nothing to show when the storage has none).
        if (auto items = _items.find(owner); items != _items.end() && items->second.count(key))
            _changed[owner].insert(key);
    }
    if (reserve)
        CharacterDatabase.Execute("REPLACE INTO ga_storage_reserve (owner_account, item_entry, random_property, reserve, updated_at) "
            "VALUES ({}, {}, {}, {}, UNIX_TIMESTAMP())", owner, entry, randomProperty, reserve);
    else
        CharacterDatabase.Execute("DELETE FROM ga_storage_reserve WHERE owner_account = {} AND item_entry = {} AND random_property = {}",
            owner, entry, randomProperty);
}

void StorageService::NoteReserveRefusal(Player* player, uint32 entry, uint32 stored, uint32 reserve)
{
    uint32 const now = getMSTime();
    {
        std::lock_guard<std::mutex> lock(_lock);
        auto& last = _reserveNoted[{ player->GetGUID(), entry }];
        if (last && getMSTimeDiff(last, now) < ReserveNoteMs)
            return;
        last = std::max<uint32>(now, 1);
    }
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    LOG_INFO("module.ga.activity", "activity: {} leaves the guild storage's {} x {} alone: the guild keeps {} of it in reserve",
        player->GetName(), stored, proto ? proto->Name1 : std::to_string(entry), reserve);
}

uint32 StorageService::Remove(uint32 owner, uint32 entry, int32 randomProperty, uint32 count)
{
    return RemoveImpl(owner, entry, randomProperty, count, !IsOwnerOrder());
}

uint32 StorageService::RemoveImpl(uint32 owner, uint32 entry, int32 randomProperty, uint32 count, bool keepReserve)
{
    if (!owner || !count)
        return 0;
    std::lock_guard<std::mutex> lock(_lock);
    auto guild = _items.find(owner);
    if (guild == _items.end())
        return 0;
    Key const key{ entry, randomProperty };
    auto itr = guild->second.find(key);
    if (itr == guild->second.end())
        return 0;
    uint32 const reserve = keepReserve ? ReserveLocked(owner, key) : 0;
    uint32 const spare = itr->second.amount > reserve ? itr->second.amount - reserve : 0;
    uint32 const removed = std::min(count, spare);
    itr->second.amount -= removed;
    if (!itr->second.amount)
        guild->second.erase(itr);
    if (removed)
        MarkChanged(owner, key);
    return removed;
}

uint32 StorageService::GetAmount(uint32 owner, uint32 entry, int32 randomProperty) const
{
    std::lock_guard<std::mutex> lock(_lock);
    auto guild = _items.find(owner);
    if (guild == _items.end())
        return 0;
    auto itr = guild->second.find({ entry, randomProperty });
    if (itr == guild->second.end())
        return 0;
    uint32 const reserve = IsOwnerOrder() ? 0 : ReserveLocked(owner, itr->first);
    return itr->second.amount > reserve ? itr->second.amount - reserve : 0;
}

uint32 StorageService::GetStored(uint32 owner, uint32 entry, int32 randomProperty) const
{
    std::lock_guard<std::mutex> lock(_lock);
    auto guild = _items.find(owner);
    if (guild == _items.end())
        return 0;
    auto itr = guild->second.find({ entry, randomProperty });
    return itr == guild->second.end() ? 0 : itr->second.amount;
}

uint32 StorageService::GetStoredOfEntry(uint32 owner, uint32 entry) const
{
    std::lock_guard<std::mutex> lock(_lock);
    auto guild = _items.find(owner);
    if (guild == _items.end())
        return 0;
    uint32 total = 0;
    for (auto itr = guild->second.lower_bound({ entry, std::numeric_limits<int32>::min() });
         itr != guild->second.end() && itr->first.first == entry; ++itr)
        total += itr->second.amount;
    return total;
}

uint32 StorageService::GetAmountOfEntry(uint32 owner, uint32 entry) const
{
    std::lock_guard<std::mutex> lock(_lock);
    auto guild = _items.find(owner);
    if (guild == _items.end())
        return 0;
    uint32 total = 0;
    bool const all = IsOwnerOrder();
    for (auto itr = guild->second.lower_bound({ entry, std::numeric_limits<int32>::min() });
         itr != guild->second.end() && itr->first.first == entry; ++itr)
    {
        uint32 const reserve = all ? 0 : ReserveLocked(owner, itr->first);
        total += itr->second.amount > reserve ? itr->second.amount - reserve : 0;
    }
    return total;
}

std::vector<StorageItem> StorageService::List(uint32 owner) const
{
    if (IsOwnerOrder())
        return ListAll(owner);
    std::vector<StorageItem> list;
    std::lock_guard<std::mutex> lock(_lock);
    auto guild = _items.find(owner);
    if (guild == _items.end())
        return list;
    list.reserve(guild->second.size());
    for (auto const& [key, item] : guild->second)
    {
        uint32 const reserve = ReserveLocked(owner, key);
        if (item.amount <= reserve)
            continue; // held back entirely
        StorageItem spare = item;
        spare.amount = item.amount - reserve;
        spare.reserve = reserve;
        list.push_back(spare);
    }
    return list;
}

std::vector<StorageItem> StorageService::ListAll(uint32 owner) const
{
    std::vector<StorageItem> list;
    std::lock_guard<std::mutex> lock(_lock);
    auto guild = _items.find(owner);
    if (guild == _items.end())
        return list;
    list.reserve(guild->second.size());
    for (auto const& [key, item] : guild->second)
    {
        StorageItem all = item;
        all.reserve = ReserveLocked(owner, key);
        list.push_back(all);
    }
    return list;
}

std::pair<uint32, uint64> StorageService::GetTotals(uint32 owner) const
{
    std::lock_guard<std::mutex> lock(_lock);
    auto guild = _items.find(owner);
    if (guild == _items.end())
        return { 0, 0 };
    uint64 items = 0;
    for (auto const& [key, item] : guild->second)
        items += item.amount;
    return { uint32(guild->second.size()), items };
}

// ---------------------------------------------------------------------------------------------------------------
// Never-worn bind-on-pickup loot (DL1, docs/storage.md "Soulbound loot")
// ---------------------------------------------------------------------------------------------------------------

bool StorageService::IsUnwornKind(ItemTemplate const* proto)
{
    return proto && proto->Bonding == BIND_WHEN_PICKED_UP && proto->Quality >= ITEM_QUALITY_UNCOMMON &&
        (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR) && !RefuseReason(proto);
}

void StorageService::MarkUnworn(Player* holder, Item* item, char const* how)
{
    if (!holder || !item || !item->IsSoulBound() || item->IsEquipped() || !IsUnwornKind(item->GetTemplate()))
        return;
    uint32 const owner = GetOwnerOf(holder);
    if (!owner)
        return;
    uint32 const counter = item->GetGUID().GetCounter();
    {
        std::lock_guard<std::mutex> lock(_unwornLock);
        if (!_unworn.emplace(counter, owner).second)
            return;
    }
    CharacterDatabase.Execute(Acore::StringFormat("REPLACE INTO ga_unworn_loot (item_guid, owner_account, character_guid, item_entry, "
        "marked_at) VALUES ({}, {}, {}, {}, UNIX_TIMESTAMP())", counter, owner, holder->GetGUID().GetCounter(), item->GetEntry()));
    LOG_DEBUG("module", "mod-guild-adventure: {} has a never-worn bind-on-pickup {} ({})", holder->GetName(),
        item->GetTemplate()->Name1, how);
}

void StorageService::ForgetUnworn(Item const* item, char const* why)
{
    if (!item)
        return;
    uint32 const counter = item->GetGUID().GetCounter();
    {
        std::lock_guard<std::mutex> lock(_unwornLock);
        if (!_unworn.erase(counter))
            return;
    }
    CharacterDatabase.Execute(Acore::StringFormat("DELETE FROM ga_unworn_loot WHERE item_guid = {}", counter));
    LOG_DEBUG("module", "mod-guild-adventure: the bind-on-pickup {} is no longer never-worn loot ({})", item->GetTemplate()->Name1, why);
}

bool StorageService::IsMarkedUnworn(Item const* item) const
{
    if (!item || item->IsEquipped())
        return false;
    std::lock_guard<std::mutex> lock(_unwornLock);
    return _unworn.count(item->GetGUID().GetCounter()) != 0;
}

bool StorageService::TakesUnworn(Item const* item) const
{
    if (!item || item->IsEquipped() || !IsUnwornKind(item->GetTemplate()))
        return false;
    uint32 owner = 0;
    {
        std::lock_guard<std::mutex> lock(_unwornLock);
        auto itr = _unworn.find(item->GetGUID().GetCounter());
        if (itr == _unworn.end())
            return false;
        owner = itr->second;
    }
    Player* holder = item->GetOwner();
    return holder && GetOwnerOf(holder) == owner && sBotSettings.IsOn(holder, Setting::StorageUnwornBound);
}

void StorageService::MarkFromStorage(Player* holder, Item* item)
{
    if (!holder || !item)
        return;
    ItemTemplate const* proto = item->GetTemplate();
    if (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR)
        return;
    uint32 const owner = GetOwnerOf(holder);
    if (!owner)
        return;
    uint32 const counter = item->GetGUID().GetCounter();
    {
        std::lock_guard<std::mutex> lock(_unwornLock);
        if (!_fromStorage.insert(counter).second)
            return;
    }
    CharacterDatabase.Execute(Acore::StringFormat("REPLACE INTO ga_storage_origin (item_guid, owner_account, character_guid, item_entry, "
        "taken_at) VALUES ({}, {}, {}, {}, UNIX_TIMESTAMP())", counter, owner, holder->GetGUID().GetCounter(), item->GetEntry()));
}

bool StorageService::CameFromStorage(Item const* item) const
{
    if (!item)
        return false;
    std::lock_guard<std::mutex> lock(_unwornLock);
    return _fromStorage.count(item->GetGUID().GetCounter()) != 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Moving items
// ---------------------------------------------------------------------------------------------------------------

uint32 StorageService::DepositItem(Player* player, Item* item, std::string& error, bool crafted, bool takeBound)
{
    uint32 const owner = GetOwnerOf(player);
    if (!owner)
    {
        error = "not a guild character";
        return 0;
    }
    if (!item || item->GetOwnerGUID() != player->GetGUID() || !IsInBags(player, item))
    {
        error = "not in the bags";
        return 0;
    }
    // the owner, 2026-11-13: [quote left out] (one guild's storage had taken
    // 11507 Heavy Shot from bag sweeps and weapon changes).
    if (!crafted && item->GetTemplate()->Class == ITEM_CLASS_PROJECTILE)
    {
        error = "ammunition is only stored when a guild character made it";
        return 0;
    }
    if (char const* reason = RefuseReason(item); reason && !(takeBound && std::string(reason) == "soulbound" &&
        !item->IsBoundAccountWide() && !RefuseReason(item->GetTemplate())))
    {
        error = reason;
        return 0;
    }

    uint32 const count = item->GetCount();
    if (!Add(owner, item->GetTemplate(), item->GetItemRandomPropertyId(), count))
    {
        error = "cannot be stored";
        return 0;
    }
    if (item->IsSoulBound() && takeBound)
        LOG_INFO("module.ga.activity", "activity: {} put {} back into the guild storage: it came out of it a moment ago and was bound "
            "by being put on, not worn", player->GetName(), item->GetTemplate()->Name1);
    else if (item->IsSoulBound())
    {
        // Only never-worn bind-on-pickup loot gets this far (RefuseReason): the mark ends with the instance.
        LOG_INFO("module.ga.activity", "activity: {} put a never-worn bind-on-pickup {} into the guild storage", player->GetName(),
            item->GetTemplate()->Name1);
        ForgetUnworn(item, "stored");
    }
    player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
    Touch(player);
    return count;
}

std::pair<uint32, uint32> StorageService::DepositMaterials(Player* player, std::vector<GuildAdventure::StorageItem>* moved,
    std::function<bool(Item*)> const& keep)
{
    std::vector<Item*> materials;
    ForEachBagItem(player, [&](Item* item)
    {
        if (IsMaterial(item->GetTemplate()) && !RefuseReason(item) && !(keep && keep(item)))
            materials.push_back(item);
    });

    uint32 stacks = 0;
    uint32 items = 0;
    for (Item* item : materials)
    {
        std::string error;
        uint32 const entry = item->GetEntry();
        int32 const randomProperty = item->GetItemRandomPropertyId();
        if (uint32 const count = DepositItem(player, item, error))
        {
            ++stacks;
            items += count;
            if (moved)
            {
                auto same = std::find_if(moved->begin(), moved->end(), [&](StorageItem const& kind)
                    { return kind.entry == entry && kind.randomProperty == randomProperty; });
                if (same != moved->end())
                    same->amount += count;
                else
                {
                    StorageItem kind;
                    kind.entry = entry;
                    kind.randomProperty = randomProperty;
                    kind.amount = count;
                    moved->push_back(kind);
                }
            }
        }
    }
    return { stacks, items };
}

uint32 StorageService::Withdraw(Player* player, uint32 entry, int32 randomProperty, uint32 count, std::string& error)
{
    uint32 const owner = GetOwnerOf(player);
    if (!owner)
    {
        error = "not a guild character";
        return 0;
    }
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    if (!proto)
    {
        error = "unknown item";
        return 0;
    }
    uint32 const stored = GetAmount(owner, entry, randomProperty);
    count = std::min(count, stored);
    if (!count)
    {
        // What its reserve keeps back (an automatic take; the owner's own Take is an OwnerOrder).
        uint32 const all = GetStored(owner, entry, randomProperty);
        if (all && !IsOwnerOrder())
        {
            error = "the guild keeps it in reserve";
            NoteReserveRefusal(player, entry, all, GetReserve(owner, entry, randomProperty));
            return 0;
        }
        error = "not in the storage";
        return 0;
    }

    // As many as fit: the core answers with the count that does not.
    ItemPosCountVec dest;
    uint32 noSpace = 0;
    InventoryResult result = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, count, &noSpace);
    if (result != EQUIP_ERR_OK && noSpace && noSpace < count)
    {
        count -= noSpace;
        dest.clear();
        result = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, count);
    }
    if (result != EQUIP_ERR_OK)
    {
        player->SendEquipError(result, nullptr, nullptr, entry);
        error = result == EQUIP_ERR_CANT_CARRY_MORE_OF_THIS ? "you cannot carry more of it" : "not enough bag space";
        return 0;
    }

    uint32 const taken = Remove(owner, entry, randomProperty, count);
    if (taken < count)
    {
        // Someone else took some in the meantime: give back what was taken and try again later.
        if (taken)
            Add(owner, proto, randomProperty, taken);
        error = "the storage changed, try again";
        return 0;
    }

    Item* item = player->StoreNewItem(dest, entry, true, randomProperty);
    if (!item)
    {
        Add(owner, proto, randomProperty, count);
        error = "could not create the item";
        return 0;
    }
    player->SendNewItem(item, count, true, false);
    // A bind-on-pickup piece is a new instance bound to the taker; until it puts it on, it may go back (DL1). And gear
    // that came out of the storage is never disenchanted or sold by the worn-gear rule.
    for (ItemPosCount const& pos : dest)
        if (Item* stored = player->GetItemByPos(pos.pos))
        {
            if (IsUnwornKind(proto))
                MarkUnworn(player, stored, "taken from the guild storage");
            MarkFromStorage(player, stored);
        }
    Touch(player);
    return count;
}

// ---------------------------------------------------------------------------------------------------------------
// Deltas
// ---------------------------------------------------------------------------------------------------------------

std::vector<uint32> StorageService::GetChangedOwners() const
{
    std::vector<uint32> owners;
    std::lock_guard<std::mutex> lock(_lock);
    for (auto const& [owner, keys] : _changed)
        if (!keys.empty())
            owners.push_back(owner);
    return owners;
}

std::vector<StorageItem> StorageService::TakeChanges(uint32 owner)
{
    std::vector<StorageItem> changes;
    std::lock_guard<std::mutex> lock(_lock);
    auto itr = _changed.find(owner);
    if (itr == _changed.end())
        return changes;
    auto guild = _items.find(owner);
    for (Key const& key : itr->second)
    {
        StorageItem item;
        item.entry = key.first;
        item.randomProperty = key.second;
        if (guild != _items.end())
        {
            auto found = guild->second.find(key);
            if (found != guild->second.end())
                item = found->second;
        }
        item.reserve = ReserveLocked(owner, key);
        if (!item.amount)
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(key.first))
            {
                item.itemClass = uint8(proto->Class);
                item.subClass = uint8(proto->SubClass);
            }
        changes.push_back(item);
    }
    _changed.erase(itr);
    return changes;
}

// ---------------------------------------------------------------------------------------------------------------
// Core patch CP-7: reagents from the guild storage
// ---------------------------------------------------------------------------------------------------------------

// A spell's reagents: the character on the owner's client (no headless home session) crafts for him and may take into
// the reserve; a character the AI plays keeps it (an OwnerOrder on this thread aside).
bool StorageService::KeepsReserve(Player* player)
{
    if (IsOwnerOrder())
        return false;
    WorldSession* session = player ? player->GetSession() : nullptr;
    return !session || session->IsHeadless();
}

void StorageService::OnCheckReagent(Player* player, uint32 itemId, uint32 count, bool& enough) const
{
    if (enough || !GetSettings().storageReagents)
        return;
    uint32 const owner = GetOwnerOf(player);
    if (!owner)
        return;
    uint32 const inBags = player->GetItemCount(itemId);
    if (inBags >= count)
    {
        enough = true;
        return;
    }
    // The owner crafting with the character on his client takes into the reserve (his own press); a bot does not.
    uint32 const available = KeepsReserve(player) ? GetAmount(owner, itemId, 0) : GetStored(owner, itemId, 0);
    if (inBags + available >= count)
        enough = true;
}

void StorageService::OnTakeReagent(Player* player, uint32 itemId, uint32& count)
{
    if (!GetSettings().storageReagents)
        return;
    uint32 const owner = GetOwnerOf(player);
    if (!owner)
        return;
    uint32 const inBags = player->GetItemCount(itemId);
    if (inBags >= count)
        return;
    uint32 const taken = RemoveImpl(owner, itemId, 0, count - inBags, KeepsReserve(player));
    count -= taken;
    // Every withdrawal leaves a line: who, what, how many, what for (the spell being cast).
    if (taken)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        Spell const* spell = player->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        SpellInfo const* info = spell ? spell->GetSpellInfo() : nullptr;
        LOG_INFO("module.ga.activity", "activity: {} used {} x {} from the guild storage as a reagent{}", player->GetName(), taken,
            proto ? proto->Name1 : Acore::StringFormat("item {}", itemId),
            info && info->SpellName[0] ? Acore::StringFormat(" for {} (spell {})", info->SpellName[0], info->Id) : std::string());
    }
}
