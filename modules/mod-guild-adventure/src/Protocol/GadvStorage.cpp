/*
 * The GADV part of the guild storage, the loot filter and crafting from the storage (docs/protocol.md §6, §7.10-§7.13;
 * docs/storage.md). Members of GadvProtocol, kept apart from GadvProtocol.cpp.
 */

#include "GadvProtocol.h"
#include "AuctionService.h"
#include <map>
#include "CraftService.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildBotHolder.h"
#include "Item.h"
#include "LootService.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StorageService.h"
#include "StringFormat.h"
#include <algorithm>

using namespace GuildAdventure;
using GuildAdventure::Gadv::Message;
namespace LR = GuildAdventure::LootRules;

namespace
{
    constexpr std::size_t ItemsPerRecord = 10;
    constexpr std::size_t LogLinesSent = 30;
    constexpr std::size_t MaxRecipes = 150;
    constexpr uint32 StoragePushMs = 1000;

    std::optional<int64> RequestOf(Message const& message)
    {
        return message.GetInt("r");
    }

    // "entry:randomProperty:amount:class:subclass:sellPrice:disenchantSkill:inventoryType:reserve" (docs/protocol.md
    // §7.10); the skill is -1 for a kind the owner cannot have disenchanted from the storage; the inventory type (the slot
    // it goes in: head, chest, one-hand ...) lets the Storage tab filter by slot; the reserve is what the bots leave. A
    // tenth field names the category when the class does not say it: "darkmoon" for the Darkmoon Faire cards and decks,
    // "reputation" for the reputation turn-ins.
    std::string StorageTuple(StorageItem const& item)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.entry);
        std::string tuple = Acore::StringFormat("{}:{}:{}:{}:{}:{}:{}:{}:{}", item.entry, item.randomProperty, item.amount,
            item.itemClass, item.subClass, proto ? proto->SellPrice : 0, LootService::StorageDisenchantSkill(proto),
            proto ? proto->InventoryType : 0, item.reserve);
        if (StorageService::IsDarkmoonCard(item.entry))
            tuple += ":darkmoon";
        else if (StorageService::IsReputationTurnIn(item.entry))
            tuple += ":reputation";
        return tuple;
    }

    Message EnchanterRecord(std::string const& opcode, uint32 owner)
    {
        uint32 skill = 0;
        Player* enchanter = LootService::BestEnchanter(owner, skill);
        return Message(opcode).Set("ench", enchanter ? int64(skill) : int64(-1))
            .Set("enchn", enchanter ? enchanter->GetName() : std::string());
    }

    // Beside `it` a record carries `qi` (2026-11-25), one "quality:itemLevel" per tuple in the same order, for the Storage
    // tab's quality filter and its sort by item level (an older addon ignores the key).
    std::vector<Message> StorageRecords(std::string const& opcode, std::vector<StorageItem> const& items)
    {
        std::vector<Message> records;
        std::string tuples, qualities;
        std::size_t inRecord = 0;
        for (StorageItem const& item : items)
        {
            if (inRecord)
            {
                tuples += ',';
                qualities += ',';
            }
            tuples += StorageTuple(item);
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.entry);
            qualities += Acore::StringFormat("{}:{}", proto ? proto->Quality : 1, proto ? proto->ItemLevel : 0);
            if (++inRecord == ItemsPerRecord)
            {
                records.push_back(Message(opcode).Set("it", tuples).Set("qi", qualities));
                tuples.clear();
                qualities.clear();
                inRecord = 0;
            }
        }
        if (inRecord)
            records.push_back(Message(opcode).Set("it", tuples).Set("qi", qualities));
        return records;
    }

    Message LootLogRecord(LootLogEntry const& e)
    {
        Message record("LOOT_LOG");
        record.Set("t", e.time).Set("g", e.character.GetCounter()).Set("e", e.entry).Set("rp", e.randomProperty)
            .Set("n", e.count).Set("o", e.outcome);
        if (e.money)
            record.Set("m", e.money);
        if (!e.why.empty())
            record.Set("why", e.why);
        return record;
    }

    // Client bag 0 slots 1-16 = server bag 255 slots 23-38; client bags 1-4 = server bag slots 19-22, slot n = n - 1
    // (mod-loot-filter's addressing).
    bool ClientToServer(int64 cbag, int64 cslot, uint8& bag, uint8& slot)
    {
        if (cbag == 0)
        {
            if (cslot < 1 || cslot > 16)
                return false;
            bag = INVENTORY_SLOT_BAG_0;
            slot = uint8(INVENTORY_SLOT_ITEM_START + cslot - 1);
            return true;
        }
        if (cbag < 1 || cbag > 4 || cslot < 1 || cslot > 36)
            return false;
        bag = uint8(INVENTORY_SLOT_BAG_START + cbag - 1);
        slot = uint8(cslot - 1);
        return true;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Blocks
// ---------------------------------------------------------------------------------------------------------------

void GadvProtocol::SendStorage(uint32 accountId)
{
    std::vector<StorageItem> items = sStorageService.ListAll(accountId);
    // The deltas pushed so far are part of this block now.
    sStorageService.TakeChanges(accountId);
    auto const [kinds, total] = sStorageService.GetTotals(accountId);
    SendBlock(accountId, "STORAGE", StorageRecords("STORAGE_ITEMS", items),
        EnchanterRecord("STORAGE_BEGIN", accountId).Set("kinds", kinds).Set("items", total));
}

void GadvProtocol::SendLoot(uint32 accountId)
{
    LootSettings const s = sLootService.GetSettingsOf(accountId);
    Settings const& server = GetSettings();
    std::vector<int64> allowed;
    for (uint8 action = 0; action < LR::ActionCount; ++action)
        if (LootService::AllowedMask() & (1u << action))
            allowed.push_back(action);

    Send(accountId, Message("LOOT_SETTINGS").Set("on", s.filterEnabled).Set("dai", s.defaultAi).Set("dpl", s.defaultPlayer)
        .Set("al", s.autoLoot).Set("sold", s.totalSold).Set("de", s.totalDisenchanted).Set("del", s.totalDeleted)
        .Set("st", s.totalStored).Set("max", server.lootMaxRules).Set("allow", Gadv::JoinInts(allowed))
        .Set("srv", server.lootFilterEnable).Set("salr", server.autoLootEnable).Set("reag", server.storageReagents)
        .Set("junk", int64(LootService::JunkQualityOfGuild(accountId))).Set("kwg", LootService::KeepWornGearOfGuild(accountId))
        .Set("junkok", server.lootFilterEnable && (LootService::AllowedMask() & (1u << LR::ActionSell)) != 0));

    std::vector<Message> records;
    for (LR::Rule const& rule : sLootService.GetRules(accountId))
        records.push_back(Message("LOOT_RULE").Set("id", rule.id).Set("pos", rule.position).Set("act", rule.action)
            .Set("en", rule.enabled).Set("lg", rule.logic).Set("c", LR::EncodeConditions(rule.conditions)));
    SendBlock(accountId, "LOOT_RULES", records, Message("LOOT_RULES_BEGIN"));
}

void GadvProtocol::SendLootLog(uint32 accountId)
{
    auto const& log = sLootService.GetLog(accountId);
    sLootService.TakeNewLog(accountId);
    std::vector<Message> records;
    std::size_t const from = log.size() > LogLinesSent ? log.size() - LogLinesSent : 0;
    for (std::size_t i = from; i < log.size(); ++i)
        records.push_back(LootLogRecord(log[i]));
    SendBlock(accountId, "LOOT_LOG", records, Message("LOOT_LOG_BEGIN"));
}

void GadvProtocol::SendRecipes(uint32 accountId, Player* crafter)
{
    struct Recipe
    {
        uint32 spell = 0;
        uint32 item = 0;
        uint32 max = 0;
        std::string name;
    };
    std::vector<Recipe> recipes;
    for (auto const& [spellId, spell] : crafter->GetSpellMap())
    {
        if (spell->State == PLAYERSPELL_REMOVED || !spell->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || !info->HasAttribute(SPELL_ATTR0_IS_TRADESKILL) || !CraftService::IsCraftRecipe(info))
            continue;
        Recipe recipe;
        recipe.spell = spellId;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (info->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM || info->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM_2)
            {
                recipe.item = info->Effects[i].ItemType;
                break;
            }
        ItemTemplate const* proto = recipe.item ? sObjectMgr->GetItemTemplate(recipe.item) : nullptr;
        recipe.name = proto ? proto->Name1 : (info->SpellName[0] ? info->SpellName[0] : "");
        recipe.max = CraftService::CraftableCount(crafter, info);
        recipes.push_back(std::move(recipe));
    }
    // What can be crafted first, then by name.
    std::sort(recipes.begin(), recipes.end(), [](Recipe const& a, Recipe const& b)
    {
        if ((a.max > 0) != (b.max > 0))
            return a.max > 0;
        return a.name < b.name;
    });
    if (recipes.size() > MaxRecipes)
        recipes.resize(MaxRecipes);

    std::vector<Message> records;
    for (Recipe const& recipe : recipes)
        records.push_back(Message("RECIPE").Set("s", recipe.spell).Set("i", recipe.item).Set("max", recipe.max)
            .Set("nm", recipe.name));
    SendBlock(accountId, "RECIPES", records, Message("RECIPES_BEGIN").Set("g", crafter->GetGUID().GetCounter())
        .Set("busy", sCraftService.IsCrafting(crafter->GetGUID())));
}

// ---------------------------------------------------------------------------------------------------------------
// Pushes
// ---------------------------------------------------------------------------------------------------------------

void GadvProtocol::PushStorageChanges(uint32 diff)
{
    _storageTimer += diff;
    if (_storageTimer < StoragePushMs)
        return;
    _storageTimer = 0;

    for (uint32 owner : sStorageService.GetChangedOwners())
    {
        std::vector<StorageItem> changes = sStorageService.TakeChanges(owner);
        if (changes.empty() || !FindConnectedLink(owner))
            continue;
        auto const [kinds, total] = sStorageService.GetTotals(owner);
        std::vector<Message> records = StorageRecords("STORAGE_DELTA", changes);
        for (std::size_t i = 0; i < records.size(); ++i)
        {
            if (i + 1 == records.size())
                records[i].Set("kinds", kinds).Set("items", total);
            Send(owner, records[i]);
        }
    }

    PushEnchanters();

    for (uint32 owner : sLootService.GetOwnersWithNewLog())
    {
        std::vector<LootLogEntry> entries = sLootService.TakeNewLog(owner);
        if (!FindConnectedLink(owner))
            continue;
        // A burst (a raid of looting bots) is cut to the newest lines.
        std::size_t const from = entries.size() > LogLinesSent ? entries.size() - LogLinesSent : 0;
        for (std::size_t i = from; i < entries.size(); ++i)
            Send(owner, LootLogRecord(entries[i]));
    }
}

void GadvProtocol::PushLoot(uint32 ownerAccount)
{
    if (FindConnectedLink(ownerAccount))
        SendLoot(ownerAccount);
}

// ---------------------------------------------------------------------------------------------------------------
// Intents
// ---------------------------------------------------------------------------------------------------------------

void GadvProtocol::OnStorageTake(uint32 accountId, Player* player, Message const& message)
{
    StorageService::OwnerOrder ownerOrder; // the reserve is for the bots, not for the owner's own Take
    std::optional<int64> const r = RequestOf(message);
    std::optional<int64> const entry = message.GetInt("e");
    int32 const randomProperty = int32(message.GetInt("rp").value_or(0));
    int64 const count = message.GetInt("n").value_or(1);
    if (!entry || *entry <= 0 || *entry > 0x7FFFFFFF || count < 1 || count > 100000)
    {
        Error(accountId, r, "BAD_REQUEST", "STORAGE_TAKE needs e (item) and n (1-100000).");
        return;
    }
    std::string error;
    uint32 const taken = sStorageService.Withdraw(player, uint32(*entry), randomProperty, uint32(count), error);
    if (!taken)
    {
        Error(accountId, r, "STORAGE_REFUSED", Acore::StringFormat("Nothing taken: {}.", error), error);
        return;
    }
    sLootService.LogStorageMove(sStorageService.GetOwnerOf(player), player->GetGUID(), player->GetName(), uint32(*entry), randomProperty,
        taken, false, Acore::StringFormat("taken by {} (Take)", player->GetName()));
    Ack(accountId, r);
}

void GadvProtocol::OnStorageDeposit(uint32 accountId, Player* player, Message const& message)
{
    std::optional<int64> const r = RequestOf(message);
    if (message.GetBool("all").value_or(false))
    {
        std::vector<StorageItem> moved;
        auto const [stacks, items] = sStorageService.DepositMaterials(player, &moved);
        if (!stacks)
        {
            Error(accountId, r, "STORAGE_REFUSED", "No materials in the bags.", "nothing");
            return;
        }
        for (StorageItem const& kind : moved)
            sLootService.LogStorageMove(sStorageService.GetOwnerOf(player), player->GetGUID(), player->GetName(), kind.entry,
                kind.randomProperty, kind.amount, true, Acore::StringFormat("deposited by {} (Deposit materials)", player->GetName()));
        PushNotice(accountId, Acore::StringFormat("Deposited {} stacks ({} items) into the guild storage.", stacks, items), false);
        Ack(accountId, r);
        return;
    }

    uint8 bag = 0;
    uint8 slot = 0;
    if (!ClientToServer(message.GetInt("b").value_or(-1), message.GetInt("s").value_or(-1), bag, slot))
    {
        Error(accountId, r, "BAD_REQUEST", "STORAGE_DEPOSIT needs all=1, or b (bag 0-4) and s (slot).");
        return;
    }
    Item* item = player->GetItemByPos(bag, slot);
    std::string error = "empty slot";
    uint32 const entry = item ? item->GetEntry() : 0;
    int32 const randomProperty = item ? item->GetItemRandomPropertyId() : 0;
    uint32 const deposited = item ? sStorageService.DepositItem(player, item, error) : 0;
    if (!deposited)
    {
        Error(accountId, r, "STORAGE_REFUSED", Acore::StringFormat("Not stored: {}.", error), error);
        return;
    }
    sLootService.LogStorageMove(sStorageService.GetOwnerOf(player), player->GetGUID(), player->GetName(), entry, randomProperty, deposited,
        true, Acore::StringFormat("deposited by {} (Deposit)", player->GetName()));
    Ack(accountId, r);
}

// The Storage tab's "Sell junk": sell=0 answers what would be sold (record STORAGE_JUNK, then the ACK), sell=1 sells it
// into the character the owner plays (docs/storage.md "Junk").
void GadvProtocol::PushEnchanters()
{
    for (auto const& [accountId, link] : _links)
    {
        if (!link.connected)
        {
            _enchanters.erase(accountId);
            continue;
        }
        uint32 skill = 0;
        Player* enchanter = LootService::BestEnchanter(accountId, skill);
        std::pair<uint32, std::string> const now = enchanter ? std::make_pair(skill + 1, enchanter->GetName())
            : std::make_pair(0u, std::string());
        auto itr = _enchanters.find(accountId);
        if (itr != _enchanters.end() && itr->second == now)
            continue;
        bool const first = itr == _enchanters.end();
        _enchanters[accountId] = now;
        if (!first)
            Send(accountId, EnchanterRecord("STORAGE_ENCH", accountId));
    }
}

// The Storage tab's Sell / Sell all on one kind (docs/storage.md "Selling and disenchanting"): at once at the vendor
// price, the copper to the character the owner plays, a line in the storage's log.
void GadvProtocol::OnStorageSell(uint32 accountId, Player* player, Message const& message)
{
    StorageService::OwnerOrder ownerOrder;
    std::optional<int64> const r = RequestOf(message);
    if (!sStorageService.GetOwnerOf(player))
    {
        Error(accountId, r, "STORAGE_REFUSED", "This character is no character of the adventure guild.", "not_guild");
        return;
    }
    std::optional<int64> const entry = message.GetInt("e");
    if (!entry || *entry <= 0)
    {
        Error(accountId, r, "BAD_REQUEST", "STORAGE_SELL needs e (item), rp and n (0 = all).");
        return;
    }
    uint32 const count = uint32(std::clamp<int64>(message.GetInt("n").value_or(0), 0, int64(INT32_MAX)));
    uint64 money = 0;
    std::string error;
    uint32 const sold = sLootService.SellStored(accountId, player, uint32(*entry), int32(message.GetInt("rp").value_or(0)),
        count, money, error);
    if (!sold)
    {
        char const* text = error == "no_price" ? "No vendor buys this." : error == "gold_limit" ? "Nothing sold: the guild has no guild bank." :
            "Not in the guild storage.";
        Error(accountId, r, "STORAGE_REFUSED", text, error);
        return;
    }
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(uint32(*entry));
    PushNotice(accountId, Acore::StringFormat("{} sold {} x {} from the guild storage for {} (into the guild bank).", player->GetName(), sold,
        proto ? proto->Name1 : std::to_string(*entry),
        Acore::StringFormat("{}g {}s {}c", money / 10000, (money % 10000) / 100, money % 100)), false);
    Ack(accountId, r);
}

// The Storage tab's Delete / Delete all on a kind no vendor buys (2026-11-13): destroyed, a line in the storage's log.
void GadvProtocol::OnStorageDelete(uint32 accountId, Player* player, Message const& message)
{
    StorageService::OwnerOrder ownerOrder;
    std::optional<int64> const r = RequestOf(message);
    if (!sStorageService.GetOwnerOf(player))
    {
        Error(accountId, r, "STORAGE_REFUSED", "This character is no character of the adventure guild.", "not_guild");
        return;
    }
    std::optional<int64> const entry = message.GetInt("e");
    if (!entry || *entry <= 0)
    {
        Error(accountId, r, "BAD_REQUEST", "STORAGE_DELETE needs e (item), rp and n (0 = all).");
        return;
    }
    uint32 const count = uint32(std::clamp<int64>(message.GetInt("n").value_or(0), 0, int64(INT32_MAX)));
    std::string error;
    uint32 const deleted = sLootService.DeleteStored(accountId, player, uint32(*entry), int32(message.GetInt("rp").value_or(0)), count,
        error);
    if (!deleted)
    {
        Error(accountId, r, "STORAGE_REFUSED", error == "has_price" ? "A vendor buys this: sell it instead." :
            "Not in the guild storage.", error);
        return;
    }
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(uint32(*entry));
    PushNotice(accountId, Acore::StringFormat("{} deleted {} x {} from the guild storage.", player->GetName(), deleted,
        proto ? proto->Name1 : std::to_string(*entry)), false);
    Ack(accountId, r);
}

// The Storage tab's Disenchant on one green or blue piece: at once, by the guild's best enchanter in the world whose
// skill is enough; the materials go into the storage.
// The Storage tab's Reserve on a kind (the owner, 2026-11-13): the bots leave that many in the storage (0: no reserve).
void GadvProtocol::OnStorageReserve(uint32 accountId, Player* player, Message const& message)
{
    std::optional<int64> const r = RequestOf(message);
    uint32 const owner = sStorageService.GetOwnerOf(player);
    if (!owner)
    {
        Error(accountId, r, "STORAGE_REFUSED", "This character is no character of the adventure guild.", "not_guild");
        return;
    }
    std::optional<int64> const entry = message.GetInt("e");
    ItemTemplate const* proto = entry && *entry > 0 && *entry <= 0x7FFFFFFF ? sObjectMgr->GetItemTemplate(uint32(*entry)) : nullptr;
    int64 const reserve = message.GetInt("n").value_or(-1);
    if (!proto || reserve < 0 || reserve > 1000000)
    {
        Error(accountId, r, "BAD_REQUEST", "STORAGE_RESERVE needs e (item), rp and n (0-1000000).");
        return;
    }
    int32 const randomProperty = int32(message.GetInt("rp").value_or(0));
    uint32 const before = sStorageService.GetReserve(owner, proto->ItemId, randomProperty);
    sStorageService.SetReserve(owner, proto->ItemId, randomProperty, uint32(reserve));
    LOG_INFO("module.ga.activity", "activity: {} sets the reserve of {} in the guild storage of owner {}: {} -> {}", player->GetName(),
        proto->Name1, owner, before, reserve);
    PushNotice(accountId, reserve ? Acore::StringFormat("The bots leave {} x {} in the guild storage.", reserve, proto->Name1)
        : Acore::StringFormat("No reserve for {} any more.", proto->Name1), false);
    Ack(accountId, r);
}

void GadvProtocol::OnStorageDisenchant(uint32 accountId, Player* player, Message const& message)
{
    StorageService::OwnerOrder ownerOrder;
    std::optional<int64> const r = RequestOf(message);
    if (!sStorageService.GetOwnerOf(player))
    {
        Error(accountId, r, "STORAGE_REFUSED", "This character is no character of the adventure guild.", "not_guild");
        return;
    }
    uint32 const entry = uint32(std::max<int64>(message.GetInt("e").value_or(0), 0));
    StorageDisenchant const result = sLootService.DisenchantFromStorage(accountId, entry,
        int32(message.GetInt("rp").value_or(0)), player->GetName());
    if (!result.done)
    {
        Error(accountId, r, "STORAGE_REFUSED", result.error == "nothing" ? std::string("Not in the guild storage.") : result.text,
            result.error);
        return;
    }
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    PushNotice(accountId, Acore::StringFormat("{} disenchanted {}: {} into the guild storage.", result.enchanter,
        proto ? proto->Name1 : std::to_string(entry), result.materials), false);
    Ack(accountId, r);
}

void GadvProtocol::OnStorageSellJunk(uint32 accountId, Player* player, Message const& message)
{
    std::optional<int64> const r = RequestOf(message);
    if (!sStorageService.GetOwnerOf(player))
    {
        Error(accountId, r, "STORAGE_REFUSED", "This character is no character of the adventure guild.", "not_guild");
        return;
    }
    bool const sell = message.GetBool("sell").value_or(false);
    if (LootService::JunkQualityOfGuild(accountId) < 0)
    {
        Error(accountId, r, "STORAGE_REFUSED", "Junk is not sold for this guild (Settings: Loot).", "off");
        return;
    }
    JunkSale const sale = sLootService.SellStoredJunk(accountId, player, sell);
    Send(accountId, Message("STORAGE_JUNK").Set("kinds", sale.kinds).Set("items", sale.items).Set("money", sale.money)
        .Set("to", std::string("the guild bank")).Set("sold", sell).Set("cap", sale.capped));
    if (sell)
    {
        if (!sale.items)
        {
            Error(accountId, r, "STORAGE_REFUSED", sale.capped ? "Nothing sold: the guild has no guild bank." : "No junk in the guild storage.",
                sale.capped ? "gold_limit" : "nothing");
            return;
        }
        PushNotice(accountId, Acore::StringFormat("{} sold {} junk items from the guild storage for {} (into the guild bank).", player->GetName(),
            sale.items, Acore::StringFormat("{}g {}s {}c", sale.money / 10000, (sale.money % 10000) / 100, sale.money % 100)), false);
        SendLoot(accountId);
    }
    Ack(accountId, r);
}

void GadvProtocol::OnLootRuleSet(uint32 accountId, Message const& message)
{
    std::optional<int64> const r = RequestOf(message);
    LR::Rule rule;
    std::optional<int64> const id = message.GetInt("id");
    std::optional<int64> const action = message.GetInt("act");
    if (!id || *id < 0 || !action || *action < 0 || *action >= LR::ActionCount
        || !LR::DecodeConditions(message.GetString("c"), rule.conditions))
    {
        Error(accountId, r, "LOOT_REFUSED", "The rule is not valid.", "INVALID");
        return;
    }
    rule.id = uint32(*id);
    rule.action = uint8(*action);
    rule.position = uint8(std::clamp<int64>(message.GetInt("pos").value_or(0), 0, 255));
    rule.enabled = message.GetBool("en").value_or(true);
    rule.logic = uint8(std::clamp<int64>(message.GetInt("lg").value_or(0), 0, 255));
    std::string error;
    if (!sLootService.SetRule(accountId, rule, error))
    {
        char const* text = error == "LIMIT" ? "The guild has as many loot rules as it may have." :
            error == "ACTION" ? "This server does not allow that action." :
            error == "NOT_FOUND" ? "No such rule." : "The rule is not valid.";
        Error(accountId, r, "LOOT_REFUSED", text, error);
        return;
    }
    SendLoot(accountId);
    Ack(accountId, r);
}

void GadvProtocol::OnLootRuleDelete(uint32 accountId, Message const& message)
{
    std::optional<int64> const r = RequestOf(message);
    std::optional<int64> const id = message.GetInt("id");
    if (!id || *id <= 0 || !sLootService.DeleteRule(accountId, uint32(*id)))
    {
        Error(accountId, r, "LOOT_REFUSED", "No such rule.", "NOT_FOUND");
        return;
    }
    SendLoot(accountId);
    Ack(accountId, r);
}

void GadvProtocol::OnLootRuleMove(uint32 accountId, Message const& message)
{
    std::optional<int64> const r = RequestOf(message);
    std::optional<int64> const id = message.GetInt("id");
    std::optional<int64> const pos = message.GetInt("pos");
    if (!id || *id <= 0 || !pos || *pos < 1 || *pos > 255 || !sLootService.MoveRule(accountId, uint32(*id), uint32(*pos)))
    {
        Error(accountId, r, "LOOT_REFUSED", "No such rule.", "NOT_FOUND");
        return;
    }
    SendLoot(accountId);
    Ack(accountId, r);
}

void GadvProtocol::OnLootRuleEnable(uint32 accountId, Message const& message)
{
    std::optional<int64> const r = RequestOf(message);
    std::optional<int64> const id = message.GetInt("id");
    std::optional<bool> const enabled = message.GetBool("en");
    if (!id || *id <= 0 || !enabled || !sLootService.EnableRule(accountId, uint32(*id), *enabled))
    {
        Error(accountId, r, "LOOT_REFUSED", "No such rule.", "NOT_FOUND");
        return;
    }
    SendLoot(accountId);
    Ack(accountId, r);
}

void GadvProtocol::OnLootOptions(uint32 accountId, Message const& message)
{
    std::optional<int64> const r = RequestOf(message);
    LootSettings const current = sLootService.GetSettingsOf(accountId);
    int64 const ai = message.GetInt("dai").value_or(current.defaultAi);
    int64 const own = message.GetInt("dpl").value_or(current.defaultPlayer);
    if (ai < 0 || ai >= LR::ActionCount || own < 0 || own >= LR::ActionCount
        || !(LootService::AllowedMask() & (1u << ai)) || !(LootService::AllowedMask() & (1u << own)))
    {
        Error(accountId, r, "LOOT_REFUSED", "This server does not allow that action.", "ACTION");
        return;
    }
    sLootService.SetOptions(accountId, message.GetBool("on").value_or(current.filterEnabled), uint8(ai), uint8(own),
        message.GetBool("al").value_or(current.autoLoot));
    SendLoot(accountId);
    Ack(accountId, r);
}

void GadvProtocol::OnCraft(uint32 accountId, Player* player, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = RequestOf(message);
    std::optional<int64> const spellId = message.GetInt("s");
    int64 const count = message.GetInt("n").value_or(1);
    if (!spellId || *spellId <= 0 || *spellId > 0x7FFFFFFF || count < 1 || count > 100)
    {
        Error(accountId, r, "BAD_REQUEST", "CRAFT needs s (spell) and n (1-100).");
        return;
    }

    // The character the owner plays, or one of its guild bots in the world (g).
    Player* crafter = player;
    if (std::optional<int64> const g = message.GetInt("g"))
    {
        ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(uint32(*g));
        if (guid != player->GetGUID())
        {
            crafter = guild.FindBot(guid) ? ObjectAccessor::FindPlayer(guid) : nullptr;
            if (!crafter || !crafter->IsInWorld())
            {
                Error(accountId, r, "UNKNOWN_BOT", "That guild bot is not in the world.");
                return;
            }
        }
    }

    // "stop" (n=0 is not allowed above): a separate flag ends a running queue.
    if (message.GetBool("stop").value_or(false))
    {
        sCraftService.Stop(crafter->GetGUID(), "stopped by you");
        Ack(accountId, r);
        return;
    }

    std::string error;
    std::string text;
    if (!sCraftService.Start(crafter, uint32(*spellId), uint32(count), error, text))
    {
        Error(accountId, r, "CRAFT_REFUSED", text, error);
        return;
    }
    PushNotice(accountId, text, false);
    if (crafter != player)
        if (AdventureGuild const* own = sGuildAdventureMgr.FindGuild(accountId))
            PushBot(*own, crafter->GetGUID(), false);
    Ack(accountId, r);
}

// ---------------------------------------------------------------------------------------------------------------
// Auction house surplus (AuctionService)
// ---------------------------------------------------------------------------------------------------------------

void GadvProtocol::SendAuctions(uint32 accountId)
{
    std::vector<Message> records;
    for (AuctionRule const& rule : sAuctionService.GetRules(accountId))
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(rule.entry);
        records.push_back(Message("AUCTION_RULE").Set("e", rule.entry).Set("rp", rule.randomProperty).Set("keep", rule.keep)
            .Set("stack", rule.stack).Set("price", rule.price).Set("on", rule.enabled)
            .Set("auto", proto ? AuctionService::DefaultPrice(proto) : 0u)
            .Set("listed", sAuctionService.GetListedCount(accountId, rule.entry, rule.randomProperty)));
    }
    SendBlock(accountId, "AUCTION_RULES", records, Message("AUCTION_RULES_BEGIN").Set("srv", GetSettings().auctionEnable)
        .Set("hours", GetSettings().auctionHours).Set("every", GetSettings().auctionIntervalSeconds));

    // Rules for whole storage categories (2026-11-13).
    std::vector<Message> categories;
    for (AuctionCategoryRule const& rule : sAuctionService.GetCategoryRules(accountId))
        categories.push_back(Message("AUCTION_CAT").Set("cat", rule.category).Set("keep", rule.keep).Set("stack", rule.stack)
            .Set("on", rule.enabled));
    SendBlock(accountId, "AUCTION_CATS", categories, Message("AUCTION_CATS_BEGIN"));

    // What of each kind is in the auction house now (a category rule's kinds have no AUCTION_RULE of their own).
    std::map<std::pair<uint32, int32>, uint32> listed;
    for (AuctionListing const& listing : sAuctionService.GetListings(accountId))
        listed[{ listing.entry, listing.randomProperty }] += listing.count;
    std::vector<Message> listings;
    for (auto const& [key, count] : listed)
        listings.push_back(Message("AUCTION_LISTED").Set("e", key.first).Set("rp", key.second).Set("n", count));
    SendBlock(accountId, "AUCTION_LISTINGS", listings, Message("AUCTION_LISTINGS_BEGIN"));
}

void GadvProtocol::PushAuctions(uint32 ownerAccount)
{
    if (FindConnectedLink(ownerAccount))
        SendAuctions(ownerAccount);
}

void GadvProtocol::OnAuctionRuleSet(uint32 accountId, Message const& message)
{
    std::optional<int64> const r = RequestOf(message);
    std::optional<int64> const entry = message.GetInt("e");
    if (!entry || *entry <= 0 || *entry > 0x7FFFFFFF)
    {
        Error(accountId, r, "BAD_REQUEST", "AUCTION_RULE_SET needs e (item).");
        return;
    }
    AuctionRule rule;
    rule.entry = uint32(*entry);
    rule.randomProperty = int32(message.GetInt("rp").value_or(0));
    rule.keep = uint32(std::clamp<int64>(message.GetInt("keep").value_or(0), 0, 0x7FFFFFFF));
    rule.stack = uint32(std::clamp<int64>(message.GetInt("stack").value_or(0), 0, 0x7FFFFFFF));
    rule.price = uint32(std::clamp<int64>(message.GetInt("price").value_or(0), 0, 0x7FFFFFFF));
    rule.enabled = message.GetBool("on").value_or(true);
    std::string error;
    if (!sAuctionService.SetRule(accountId, rule, error))
    {
        char const* text = error == "NOT_AUCTIONABLE" ? "That item cannot be sold in the auction house." :
            error == "LIMIT" ? "The guild has as many auction rules as it may have." : "The rule is not valid.";
        Error(accountId, r, "AUCTION_REFUSED", text, error);
        return;
    }
    SendAuctions(accountId);
    Ack(accountId, r);
}

void GadvProtocol::OnAuctionCategorySet(uint32 accountId, Message const& message)
{
    std::optional<int64> const r = RequestOf(message);
    AuctionCategoryRule rule;
    rule.category = message.GetString("cat");
    rule.keep = uint32(std::clamp<int64>(message.GetInt("keep").value_or(0), 0, 0x7FFFFFFF));
    rule.stack = uint32(std::clamp<int64>(message.GetInt("stack").value_or(0), 0, 0x7FFFFFFF));
    rule.enabled = message.GetBool("on").value_or(true);
    std::string error;
    if (!sAuctionService.SetCategoryRule(accountId, rule, error))
    {
        char const* text = error == "CATEGORY" ? "There is no such storage category." :
            error == "LIMIT" ? "The guild has as many category rules as it may have." : "The rule is not valid.";
        Error(accountId, r, "AUCTION_REFUSED", text, error);
        return;
    }
    SendAuctions(accountId);
    Ack(accountId, r);
}

void GadvProtocol::OnAuctionCategoryDelete(uint32 accountId, Message const& message)
{
    std::optional<int64> const r = RequestOf(message);
    if (!sAuctionService.DeleteCategoryRule(accountId, message.GetString("cat")))
    {
        Error(accountId, r, "AUCTION_REFUSED", "No such category rule.", "NOT_FOUND");
        return;
    }
    SendAuctions(accountId);
    Ack(accountId, r);
}

void GadvProtocol::OnAuctionRuleDelete(uint32 accountId, Message const& message)
{
    std::optional<int64> const r = RequestOf(message);
    std::optional<int64> const entry = message.GetInt("e");
    if (!entry || !sAuctionService.DeleteRule(accountId, uint32(*entry), int32(message.GetInt("rp").value_or(0))))
    {
        Error(accountId, r, "AUCTION_REFUSED", "No such auction rule.", "NOT_FOUND");
        return;
    }
    SendAuctions(accountId);
    Ack(accountId, r);
}
