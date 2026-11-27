/*
 * LootService implementation: loot filter rules, loot routing and the guild's totals. Auto loot is in AutoLoot.cpp.
 * See LootService.h and docs/storage.md.
 */

#include "LootService.h"
#include "Bag.h"
#include "DungeonRoles.h"
#include "Farming.h"
#include "OrderService.h"
#include "BotDriver.h"
#include "BotSettings.h"
#include "Chat.h"
#include "Creature.h"
#include "Formulas.h"
#include "CraftOrders.h"
#include "DatabaseEnv.h"
#include "GuildAdventureConfig.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildAdventureUtil.h"
#include "GuildBank.h"
#include "GuildBotHolder.h"
#include "Item.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PerkService.h"
#include "Player.h"
#include "Professions.h"
#include "QuestDef.h"
#include "QuestSupport.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StorageService.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>
#include <cctype>

using namespace GuildAdventure;
namespace LR = GuildAdventure::LootRules;

namespace
{
    constexpr uint32 TotalsIntervalMs = 10 * IN_MILLISECONDS;
    // Bag sweep: AI-played characters with fewer free bag slots than this are swept, at most once per interval.
    constexpr uint32 SweepCheckMs = 5 * IN_MILLISECONDS;
    constexpr uint32 OpenCheckMs = 15 * IN_MILLISECONDS;
    constexpr uint32 SweepIntervalMs = 60 * IN_MILLISECONDS;
    constexpr uint32 SweepBelowFreeSlots = 8;
    // With room in the bags the sweep comes every half hour (the owner, 2026-11-25: [quote left out]). It ran only with fewer than
    // eight free slots, and at a vendor visit: at level 60 with four big bags and few vendor visits neither came, and
    // what the loot filter would store, sell or disenchant stayed in the bags (soulbound rewards nobody wore, off-hand
    // pieces under a staff, worn gear). And at once when a piece for the off hand lies under a two-hand weapon.
    constexpr uint32 RoomySweepIntervalMs = 30 * MINUTE * IN_MILLISECONDS;

    bool CarriesOffHandUnderTwoHand(Player* player)
    {
        if (!player->IsTwoHandUsed())
            return false;
        bool found = false;
        auto look = [&](Item* item)
        {
            found = found || (item && BotDriver::OffHandUnderTwoHand(player, item->GetTemplate()));
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END && !found; ++slot)
            look(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END && !found; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 i = 0; i < bag->GetBagSize() && !found; ++i)
                    look(bag->GetItemByPos(uint8(i)));
        return found;
    }
    constexpr char const* ChatTag = "|cff88aaff[Guild storage]|r ";
    // Whether white gear would be worn by a guild character is checked again after this time.
    constexpr uint32 WornCacheMs = 60 * IN_MILLISECONDS;

    std::string FormatMoney(uint64 copper)
    {
        uint64 const gold = copper / 10000;
        uint64 const silver = (copper % 10000) / 100;
        std::string result;
        if (gold > 0)
            result += std::to_string(gold) + "g ";
        if (silver > 0 || gold > 0)
            result += std::to_string(silver) + "s ";
        result += std::to_string(copper % 100) + "c";
        return result;
    }
}

LootService& LootService::Instance()
{
    static LootService instance;
    return instance;
}

// ---------------------------------------------------------------------------------------------------------------
// Load and save
// ---------------------------------------------------------------------------------------------------------------

void LootService::Load()
{
    LoadNeededItems();
    _guilds.clear();
    uint32 lastId = 0;

    if (QueryResult result = CharacterDatabase.Query("SELECT owner_account, filter_enabled, default_ai, default_player, auto_loot, "
        "total_sold, total_disenchanted, total_deleted, total_stored FROM ga_loot_settings"))
    {
        do
        {
            Field* f = result->Fetch();
            GuildLoot& guild = _guilds[f[0].Get<uint32>()];
            LootSettings& s = guild.settings;
            s.filterEnabled = f[1].Get<uint8>() != 0;
            s.defaultAi = std::min<uint8>(f[2].Get<uint8>(), LR::ActionCount - 1);
            s.defaultPlayer = std::min<uint8>(f[3].Get<uint8>(), LR::ActionCount - 1);
            s.autoLoot = f[4].Get<uint8>() != 0;
            s.totalSold = f[5].Get<uint64>();
            s.totalDisenchanted = f[6].Get<uint32>();
            s.totalDeleted = f[7].Get<uint32>();
            s.totalStored = f[8].Get<uint32>();
            guild.loaded = true;
        } while (result->NextRow());
    }

    uint32 rules = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT r.rule_id, r.owner_account, r.position, r.action, r.enabled, r.logic, "
        "c.slot, c.grp, c.type, c.op, c.value, c.value2, c.text FROM ga_loot_rule AS r LEFT JOIN ga_loot_condition AS c "
        "ON c.rule_id = r.rule_id ORDER BY r.owner_account, r.position, r.rule_id, c.slot"))
    {
        do
        {
            Field* f = result->Fetch();
            uint32 const ruleId = f[0].Get<uint32>();
            uint32 const owner = f[1].Get<uint32>();
            lastId = std::max(lastId, ruleId);
            GuildLoot& guild = _guilds[owner];
            if (guild.rules.empty() || guild.rules.back().id != ruleId)
            {
                LR::Rule rule;
                rule.id = ruleId;
                rule.position = f[2].Get<uint8>();
                rule.action = f[3].Get<uint8>();
                rule.enabled = f[4].Get<uint8>() != 0;
                rule.logic = f[5].Get<uint8>();
                guild.rules.push_back(std::move(rule));
                ++rules;
            }
            if (f[6].IsNull())
                continue;
            LR::Condition c;
            c.group = f[7].Get<uint8>();
            c.type = f[8].Get<uint8>();
            c.op = f[9].Get<uint8>();
            c.value = f[10].Get<uint32>();
            c.value2 = f[11].Get<uint32>();
            c.text = f[12].Get<std::string>();
            guild.rules.back().conditions.push_back(std::move(c));
        } while (result->NextRow());
    }

    uint32 invalid = 0;
    for (auto& [owner, guild] : _guilds)
    {
        std::size_t const before = guild.rules.size();
        guild.rules.erase(std::remove_if(guild.rules.begin(), guild.rules.end(),
            [](LR::Rule const& r) { return !LR::ValidRule(r); }), guild.rules.end());
        invalid += uint32(before - guild.rules.size());
        LR::SortByPosition(guild.rules);
    }
    _nextRuleId = lastId + 1;

    {
        std::lock_guard<std::mutex> lock(_autoLootLock);
        _autoLoot.clear();
        for (auto const& [owner, guild] : _guilds)
            _autoLoot[owner] = guild.settings.autoLoot;
    }

    LOG_INFO("module", "mod-guild-adventure: loaded {} loot filter rules of {} guilds{}", rules - invalid, uint32(_guilds.size()),
        invalid ? Acore::StringFormat(" ({} invalid rules ignored until the next save)", invalid) : std::string());
}

LootService::GuildLoot& LootService::Guild(uint32 owner)
{
    GuildLoot& guild = _guilds[owner];
    guild.loaded = true;
    return guild;
}

void LootService::SaveRules(uint32 owner, GuildLoot const& guild) const
{
    // Every change rewrites the guild's rule set in one transaction (at most MaxRules rules of eight conditions).
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    trans->Append("DELETE c FROM ga_loot_condition AS c INNER JOIN ga_loot_rule AS r ON r.rule_id = c.rule_id "
        "WHERE r.owner_account = {}", owner);
    trans->Append("DELETE FROM ga_loot_rule WHERE owner_account = {}", owner);
    for (LR::Rule const& rule : guild.rules)
    {
        trans->Append("INSERT INTO ga_loot_rule (rule_id, owner_account, position, action, enabled, logic) VALUES "
            "({}, {}, {}, {}, {}, {})", rule.id, owner, rule.position, rule.action, rule.enabled ? 1 : 0, rule.logic);
        for (std::size_t i = 0; i < rule.conditions.size(); ++i)
        {
            LR::Condition const& c = rule.conditions[i];
            std::string text = c.text;
            CharacterDatabase.EscapeString(text);
            trans->Append("INSERT INTO ga_loot_condition (rule_id, slot, grp, type, op, value, value2, text) VALUES "
                "({}, {}, {}, {}, {}, {}, {}, '{}')", rule.id, i, c.group, c.type, c.op, c.value, c.value2, text);
        }
    }
    CharacterDatabase.CommitTransaction(trans);
}

void LootService::SaveSettings(uint32 owner, GuildLoot const& guild) const
{
    LootSettings const& s = guild.settings;
    CharacterDatabase.Execute("INSERT INTO ga_loot_settings (owner_account, filter_enabled, default_ai, default_player, auto_loot) "
        "VALUES ({}, {}, {}, {}, {}) AS n ON DUPLICATE KEY UPDATE filter_enabled = n.filter_enabled, default_ai = n.default_ai, "
        "default_player = n.default_player, auto_loot = n.auto_loot", owner, s.filterEnabled ? 1 : 0, s.defaultAi,
        s.defaultPlayer, s.autoLoot ? 1 : 0);
}

void LootService::FlushTotals()
{
    for (auto& [owner, guild] : _guilds)
    {
        if (!guild.pendingSold && !guild.pendingDisenchanted && !guild.pendingDeleted && !guild.pendingStored)
            continue;
        LootSettings const& s = guild.settings;
        CharacterDatabase.Execute("INSERT INTO ga_loot_settings (owner_account, filter_enabled, default_ai, default_player, "
            "auto_loot, total_sold, total_disenchanted, total_deleted, total_stored) VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}) "
            "AS n ON DUPLICATE KEY UPDATE total_sold = ga_loot_settings.total_sold + n.total_sold, "
            "total_disenchanted = ga_loot_settings.total_disenchanted + n.total_disenchanted, "
            "total_deleted = ga_loot_settings.total_deleted + n.total_deleted, "
            "total_stored = ga_loot_settings.total_stored + n.total_stored",
            owner, s.filterEnabled ? 1 : 0, s.defaultAi, s.defaultPlayer, s.autoLoot ? 1 : 0, guild.pendingSold,
            guild.pendingDisenchanted, guild.pendingDeleted, guild.pendingStored);
        guild.pendingSold = 0;
        guild.pendingDisenchanted = 0;
        guild.pendingDeleted = 0;
        guild.pendingStored = 0;
    }
}

void LootService::FlushNow()
{
    FlushTotals();
}

// ---------------------------------------------------------------------------------------------------------------
// Rules and settings
// ---------------------------------------------------------------------------------------------------------------

uint8 LootService::AllowedMask()
{
    Settings const& s = GetSettings();
    return LR::AllowedMask(s.lootAllowSell, s.lootAllowDisenchant, s.lootAllowDelete);
}

std::vector<LR::Rule> const& LootService::GetRules(uint32 owner)
{
    return Guild(owner).rules;
}

LootSettings LootService::GetSettingsOf(uint32 owner) const
{
    auto itr = _guilds.find(owner);
    return itr == _guilds.end() ? LootSettings() : itr->second.settings;
}

bool LootService::SetRule(uint32 owner, LR::Rule rule, std::string& error)
{
    if (!LR::ValidRule(rule))
    {
        error = "INVALID";
        return false;
    }
    if (!(AllowedMask() & (1u << rule.action)))
    {
        error = "ACTION";
        return false;
    }

    GuildLoot& guild = Guild(owner);
    uint32 const pos = rule.position;
    if (rule.id == 0)
    {
        if (guild.rules.size() >= GetSettings().lootMaxRules)
        {
            error = "LIMIT";
            return false;
        }
        rule.id = _nextRuleId++;
        LR::Insert(guild.rules, std::move(rule), pos);
    }
    else
    {
        auto itr = std::find_if(guild.rules.begin(), guild.rules.end(), [&](LR::Rule const& r) { return r.id == rule.id; });
        if (itr == guild.rules.end())
        {
            error = "NOT_FOUND";
            return false;
        }
        itr->action = rule.action;
        itr->enabled = rule.enabled;
        itr->logic = rule.logic;
        itr->conditions = std::move(rule.conditions);
        if (pos && pos != itr->position)
            LR::Move(guild.rules, itr->id, pos);
    }
    SaveRules(owner, guild);
    return true;
}

bool LootService::DeleteRule(uint32 owner, uint32 ruleId)
{
    GuildLoot& guild = Guild(owner);
    if (!LR::Remove(guild.rules, ruleId))
        return false;
    SaveRules(owner, guild);
    return true;
}

bool LootService::MoveRule(uint32 owner, uint32 ruleId, uint32 position)
{
    GuildLoot& guild = Guild(owner);
    if (!position || !LR::Move(guild.rules, ruleId, position))
        return false;
    SaveRules(owner, guild);
    return true;
}

bool LootService::EnableRule(uint32 owner, uint32 ruleId, bool enabled)
{
    GuildLoot& guild = Guild(owner);
    auto itr = std::find_if(guild.rules.begin(), guild.rules.end(), [&](LR::Rule const& r) { return r.id == ruleId; });
    if (itr == guild.rules.end())
        return false;
    itr->enabled = enabled;
    SaveRules(owner, guild);
    return true;
}

void LootService::ClearRules(uint32 owner)
{
    GuildLoot& guild = Guild(owner);
    guild.rules.clear();
    SaveRules(owner, guild);
}

void LootService::SetOptions(uint32 owner, bool filterEnabled, uint8 defaultAi, uint8 defaultPlayer, bool autoLoot)
{
    GuildLoot& guild = Guild(owner);
    LootSettings& s = guild.settings;
    s.filterEnabled = filterEnabled;
    s.defaultAi = std::min<uint8>(defaultAi, LR::ActionCount - 1);
    s.defaultPlayer = std::min<uint8>(defaultPlayer, LR::ActionCount - 1);
    s.autoLoot = autoLoot;
    SaveSettings(owner, guild);

    std::lock_guard<std::mutex> lock(_autoLootLock);
    _autoLoot[owner] = autoLoot;
}

std::deque<LootLogEntry> const& LootService::GetLog(uint32 owner)
{
    return Guild(owner).log;
}

bool LootService::IsAutoLootOn(uint32 owner) const
{
    std::lock_guard<std::mutex> lock(_autoLootLock);
    auto itr = _autoLoot.find(owner);
    return itr == _autoLoot.end() || itr->second;
}

std::vector<uint32> LootService::GetOwnersWithNewLog() const
{
    std::vector<uint32> owners;
    for (auto const& [owner, guild] : _guilds)
        if (!guild.newLog.empty())
            owners.push_back(owner);
    return owners;
}

std::vector<LootLogEntry> LootService::TakeNewLog(uint32 owner)
{
    std::vector<LootLogEntry> entries;
    auto itr = _guilds.find(owner);
    if (itr != _guilds.end())
        entries.swap(itr->second.newLog);
    return entries;
}

// ---------------------------------------------------------------------------------------------------------------
// Loot routing
// ---------------------------------------------------------------------------------------------------------------

void LootService::OnLooted(Player* player, Item* item)
{
    if (!player || !item || !IsModuleEnabled() || !GetSettings().lootFilterEnable)
        return;
    // Only guild characters (a member of an adventure guild's WoW guild); the owner check is thread-safe.
    if (!sStorageService.GetOwnerOf(player))
        return;
    std::lock_guard<std::mutex> lock(_queueLock);
    _queue.push_back({ player->GetGUID(), item->GetGUID() });
}

bool LootService::IsAIPlayed(Player* player)
{
    ObjectGuid const guid = player->GetGUID();
    return sGuildBotHolder.IsAIDriven(guid) || sGuildBotHolder.IsWatched(guid);
}

LR::ItemFacts LootService::Facts(Player* player, Item* item) const
{
    ItemTemplate const* proto = item->GetTemplate();
    LR::ItemFacts f;
    f.entry = proto->ItemId;
    f.quality = proto->Quality;
    f.itemLevel = proto->ItemLevel;
    f.sellPrice = proto->SellPrice;
    f.itemClass = proto->Class;
    f.subClass = proto->SubClass;
    f.questItem = proto->Class == ITEM_CLASS_QUEST || proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_QUEST_ITEM1
        || proto->StartQuest != 0;
    f.name = proto->Name1;
    uint64 const money = uint64(proto->SellPrice) * item->GetCount();
    f.sellable = money && uint64(player->GetMoney()) + money <= MAX_MONEY_AMOUNT;
    f.disenchantable = proto->DisenchantID != 0;
    f.storable = StorageService::RefuseReason(item) == nullptr;
    return f;
}

LR::Verdict LootService::Test(Player* player, Item* item)
{
    LR::Verdict verdict;
    uint32 const owner = sStorageService.GetOwnerOf(player);
    if (!owner || !item)
        return verdict;
    GuildLoot& guild = Guild(owner);
    verdict = LR::Evaluate(guild.rules, Facts(player, item), AllowedMask());
    if (verdict.result == LR::ResultNoRule)
    {
        verdict.result = IsJunk(owner, item->GetTemplate(), item->GetItemRandomPropertyId(), JunkQuality(player),
            KeepWornGear(player))
            ? uint8(LR::ActionSell) : IsAIPlayed(player) ? guild.settings.defaultAi : guild.settings.defaultPlayer;
        verdict.position = 0;
    }
    return verdict;
}

void LootService::Update(uint32 diff)
{
    std::vector<Looted> queue;
    {
        std::lock_guard<std::mutex> lock(_queueLock);
        queue.swap(_queue);
    }

    std::unordered_map<ObjectGuid, ChatBatch> chat;
    for (Looted const& looted : queue)
        Process(looted, chat);

    // One line per tick for the character the owner plays (the bots' loot shows in the addon's storage window).
    for (auto const& [guid, b] : chat)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!player || !player->GetSession() || player->GetSession()->IsHeadless())
            continue;
        std::vector<std::string> parts;
        if (b.stored)
            parts.push_back(Acore::StringFormat("stored {}", b.stored));
        if (b.sold)
            parts.push_back(Acore::StringFormat("sold {} for {}", b.sold, FormatMoney(b.money)));
        if (b.disenchanted)
            parts.push_back(Acore::StringFormat("disenchanted {}", b.disenchanted));
        if (b.deleted)
            parts.push_back(Acore::StringFormat("deleted {}", b.deleted));
        if (parts.empty())
            continue;
        std::string line;
        for (std::size_t i = 0; i < parts.size(); ++i)
            line += (i ? ", " : "") + parts[i];
        line[0] = char(std::toupper(static_cast<unsigned char>(line[0])));
        ChatHandler(player->GetSession()).SendSysMessage(std::string(ChatTag) + line + ".");
    }

    _totalsTimer += diff;
    if (_totalsTimer >= TotalsIntervalMs)
    {
        _totalsTimer = 0;
        FlushTotals();
    }

    _sweepTimer += diff;
    if (_sweepTimer >= SweepCheckMs)
    {
        _sweepTimer = 0;
        SweepFullBags();
    }

    _openTimer += diff;
    if (_openTimer >= OpenCheckMs)
    {
        _openTimer = 0;
        OpenContainersStep();
    }
}

void LootService::SweepFullBags()
{
    if (!GetSettings().lootFilterEnable || !GetSettings().storageEnable)
        return;
    uint32 const now = getMSTime();
    for (auto& [owner, guild] : sGuildAdventureMgr.GetGuilds())
    {
        auto itr = _guilds.find(owner);
        if (itr != _guilds.end() && !itr->second.settings.filterEnabled)
            continue;
        std::vector<ObjectGuid> characters;
        characters.push_back(guild.leader);
        for (auto const& [guid, bot] : guild.bots)
            characters.push_back(guid);
        for (ObjectGuid const& guid : characters)
        {
            Player* player = sGuildBotHolder.GetPlannedPlayer(guid);
            if (!player)
                player = sGuildBotHolder.GetAIDrivenPlayer(guid); // the leader while the owner plays a bot
            if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld() || !IsAIPlayed(player))
                continue;
            // The Darkmoon Faire cards it carries, whatever room it has (they were kept as quest items before).
            auto cardLook = _lastCardLook.find(guid);
            if (cardLook == _lastCardLook.end() || getMSTimeDiff(cardLook->second, now) >= SweepIntervalMs)
            {
                _lastCardLook[guid] = now;
                StoreDarkmoonCards(player);
                StoreReputationTurnIns(player);
            }
            bool const tight = player->GetFreeInventorySpace() < SweepBelowFreeSlots;
            auto last = _lastSweep.find(guid);
            if (!tight && last == _lastSweep.end())
            {
                // (The first one with room after half an hour: not every character at once after a start.)
                _lastSweep[guid] = now;
                continue;
            }
            uint32 const since = last == _lastSweep.end() ? RoomySweepIntervalMs : getMSTimeDiff(last->second, now);
            if (since < SweepIntervalMs || (!tight && since < RoomySweepIntervalMs && !CarriesOffHandUnderTwoHand(player)))
                continue;
            _lastSweep[guid] = now;
            // Gear it has worn and will not wear again: an enchanter disenchants it where it stands (WG1; the sale
            // waits for a vendor visit). Not part of the automatic storing, so without the perk too.
            HandleWornGear(player, nullptr);
            // The sweep is part of the automatic storing: the guild perk Material Haulers.
            if (sPerkService.Has(guid, PerkKind::MaterialsStorage))
                SweepBags(player);
        }
    }
}

uint32 LootService::StoreDarkmoonCards(Player* player)
{
    if (!sBotSettings.IsOn(player, Setting::StorageDarkmoonCards))
        return 0;
    return StoreCarried(player, &StorageService::IsDarkmoonCard, "the Darkmoon cards");
}

uint32 LootService::StoreReputationTurnIns(Player* player)
{
    if (!sBotSettings.IsOn(player, Setting::StorageReputationTurnIns))
        return 0;
    return StoreCarried(player, &StorageService::IsReputationTurnIn, "the reputation turn-ins");
}

uint32 LootService::StoreCarried(Player* player, bool (*kind)(uint32), char const* what)
{
    std::vector<Looted> found;
    auto add = [&](Item* item)
    {
        if (item && kind(item->GetEntry()))
            found.push_back({ player->GetGUID(), item->GetGUID() });
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        add(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = player->GetBagByPos(bagSlot))
            for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                add(bag->GetItemByPos(uint8(i)));
    if (found.empty())
        return 0;

    uint32 const owner = sStorageService.GetOwnerOf(player);
    std::size_t const logged = owner ? Guild(owner).newLog.size() : 0;
    std::unordered_map<ObjectGuid, ChatBatch> chat;
    for (Looted const& looted : found)
        Process(looted, chat, true);

    // One line for what went (each stack is a line of Recent loot too).
    uint32 stacks = 0;
    std::string names;
    if (owner)
    {
        GuildLoot const& guild = Guild(owner);
        for (std::size_t i = std::min(logged, guild.newLog.size()); i < guild.newLog.size(); ++i)
        {
            LootLogEntry const& line = guild.newLog[i];
            if (line.character != player->GetGUID() || line.outcome != LR::ActionStore)
                continue;
            ++stacks;
            if (stacks <= 10)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(line.entry);
                names += Acore::StringFormat("{}{} x {}", names.empty() ? "" : ", ", line.count, proto ? proto->Name1 : "?");
            }
        }
    }
    if (stacks)
        LOG_INFO("module.ga.activity", "activity: {} put {} it carried into the guild storage: {}{}", player->GetName(), what,
            names, stacks > 10 ? Acore::StringFormat(" and {} more stacks", stacks - 10) : std::string());
    return stacks;
}

uint32 LootService::SweepBags(Player* player)
{
    // Not while an enchantment is under way: its scroll or vellum lies in the bags for the second before the cast, and
    // the sweep put it back into the guild storage (round 233: Magefrost took a Scroll of Enchant Bracer - Greater
    // Intellect, the half-hourly sweep stored it in the same second, and the cast found no scroll).
    if (player && sProfessions.IsEnchanting(player->GetGUID()))
        return 0;
    std::vector<Looted> items;
    auto add = [&](Item* item)
    {
        if (item)
            items.push_back({ player->GetGUID(), item->GetGUID() });
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        add(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = player->GetBagByPos(bagSlot))
            for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                add(bag->GetItemByPos(uint8(i)));

    // What the sweep moves is written down, one line for the sweep (a hunter that changed from a gun to a bow had 4365
    // bullets go into the storage without a line anywhere; the storage's count was the only trace).
    uint32 const owner = sStorageService.GetOwnerOf(player);
    std::size_t const logged = owner ? Guild(owner).newLog.size() : 0;

    std::unordered_map<ObjectGuid, ChatBatch> chat;
    for (Looted const& looted : items)
        Process(looted, chat, true);
    if (GetSettings().debug)
        LOG_INFO("module", "mod-guild-adventure: swept {} stacks in the bags of {}", items.size(), player->GetName());

    if (owner)
    {
        GuildLoot const& guild = Guild(owner);
        // kind of item -> count, per outcome, in the order they went
        struct Moved
        {
            uint32 entry;
            uint32 count;
        };
        std::vector<Moved> stored, sold, other;
        uint64 money = 0;
        for (std::size_t i = std::min(logged, guild.newLog.size()); i < guild.newLog.size(); ++i)
        {
            LootLogEntry const& line = guild.newLog[i];
            if (line.character != player->GetGUID() || line.outcome == LR::ActionKeep)
                continue;
            std::vector<Moved>& list = line.outcome == LR::ActionStore ? stored : (line.outcome == LR::ActionSell ? sold : other);
            auto same = std::find_if(list.begin(), list.end(), [&](Moved const& moved) { return moved.entry == line.entry; });
            if (same == list.end())
                list.push_back({ line.entry, line.count });
            else
                same->count += line.count;
            money += line.money;
        }
        auto text = [](std::vector<Moved> const& list)
        {
            std::string names;
            for (std::size_t i = 0; i < list.size() && i < 8; ++i)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(list[i].entry);
                names += Acore::StringFormat("{}{} x {}", names.empty() ? "" : ", ", list[i].count, proto ? proto->Name1 : "?");
            }
            if (list.size() > 8)
                names += Acore::StringFormat(", {} more kinds", list.size() - 8);
            return names;
        };
        if (!stored.empty() || !sold.empty() || !other.empty())
            LOG_INFO("module.ga.activity", "activity: {} bags through the loot filter:{}{}{}", player->GetName(),
                stored.empty() ? std::string() : " into the guild storage " + text(stored) + ";",
                sold.empty() ? std::string() : Acore::StringFormat(" sold {} ({} copper);", text(sold), money),
                other.empty() ? std::string() : " disenchanted or deleted " + text(other) + ";");
    }
    return uint32(items.size());
}

namespace
{
    // Whether a quest in the character's log asks for this item (open, or complete and not handed in yet: the items
    // are handed in with it). No counting: looted items join the stack in the bags, and the filter sees the stack.
    // (skipLeftAlone: not a quest the planner leaves alone, for a character the AI plays: nothing works on it, and the
    // Arcane Crystals a hunter gathered stayed in its bags for the attunement to Naxxramas, round 179.)
    bool NeededForQuest(Player* player, Item* item, bool skipLeftAlone = false)
    {
        uint32 const entry = item->GetEntry();
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = player->GetQuestSlotQuestId(slot);
            if (!questId)
                continue;
            QuestStatus const status = player->GetQuestStatus(questId);
            if (status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE)
                continue;
            if (skipLeftAlone && sQuestSupport.IsLeftAlone(player, questId))
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i] == entry && quest->RequiredItemCount[i])
                    return true;
        }
        return false;
    }
}

void LootService::Process(Looted const& looted, std::unordered_map<ObjectGuid, ChatBatch>& chat, bool fromBags)
{
    Player* player = ObjectAccessor::FindPlayer(looted.character);
    if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld())
        return;
    uint32 const owner = sStorageService.GetOwnerOf(player);
    if (!owner)
        return;
    // Gone, or already equipped (playerbots equips an upgrade as soon as it arrives).
    Item* item = player->GetItemByGuid(looted.item);
    if (!item || item->IsEquipped() || item->GetOwnerGUID() != player->GetGUID())
        return;

    GuildLoot& guild = Guild(owner);
    if (!guild.settings.filterEnabled)
        return;

    LR::ItemFacts const facts = Facts(player, item);
    LR::Verdict const verdict = LR::Evaluate(guild.rules, facts, AllowedMask());
    bool const ai = IsAIPlayed(player);
    // The Darkmoon Faire cards and decks go into the guild storage (the owner, 2026-11-25: [quote left out]). Quest items to the game, so no rule of the loot filter touches them
    // (not sold, disenchanted or deleted) and they are never junk; neither Material Haulers nor what a bot would do with
    // them holds them back. The bags of the character the owner plays are his: he puts them in by hand.
    bool const darkmoon = ai && StorageService::IsDarkmoonCard(facts.entry) && sBotSettings.IsOn(player, Setting::StorageDarkmoonCards);
    // The reputation turn-ins likewise (the owner, 2026-11-25, the design log section 7 RT1): into the guild storage, not sold.
    bool const turnIn = ai && !darkmoon && StorageService::IsReputationTurnIn(facts.entry) &&
        sBotSettings.IsOn(player, Setting::StorageReputationTurnIns);
    if (verdict.result == LR::ResultProtected && !darkmoon && !turnIn)
        return;

    if (fromBags && !ai)
        return; // the bags of the character the owner plays are his
    // Ammunition in the bags is Ammo::Trim's (Ammunition.h): what goes back into the storage, what is sold.
    if (fromBags && item->GetTemplate()->Class == ITEM_CLASS_PROJECTILE)
        return;
    // A container a bot loots (a clam, a bag of loot, a crate, a lockbox it can pick) is opened at once (the owner,
    // 2026-11-13: [quote left out]): what comes out is loot like any other and comes back through the filter. One it cannot open right now (a
    // fight, full bags) waits in its bags for the next look (OpenContainersStep). A lockbox it cannot pick, and a
    // container a rule of the guild sends elsewhere (keep, sell, delete), go the usual way.
    if (ai && ContainerKindOf(item->GetTemplate()) != ContainerKind::None
        && (verdict.result == LR::ResultNoRule || verdict.result == LR::ActionStore)
        && sBotSettings.IsOn(player, Setting::LootOpenContainers))
    {
        char const* reason = CannotOpen(player, item);
        if (!reason && OpenContainer(player, item, fromBags ? "from its bags" : "looted"))
            return;
        if (!reason || !IsLastingReason(reason))
            return; // stays in the bags until it can
    }
    uint8 action = verdict.result < LR::ActionCount ? verdict.result : (ai ? guild.settings.defaultAi : guild.settings.defaultPlayer);
    // Loot goes into the guild storage by itself only once the guild bought the perk Material Haulers (the owner, 2026-11-12:
    // [quote left out]): until then loot no rule decides stays in the
    // bags. With it the defaults act (the bots' loot into the storage), and the crafting materials the owner's own
    // character loots go there too. A rule's own decision ("to the storage" included) works either way.
    // Junk is sold at once (the owner, 2026-11-12: [quote left out]): before the guild's default, after its own rules.
    // What the character's farm order is for (the order FARM, Farming.h) goes into the guild storage: the leader sent
    // it out for exactly this item. It is never junk, and neither Material Haulers nor the bot's own use of it holds
    // it back; a rule of the guild that names it still decides.
    bool const farmed = ai && verdict.result == LR::ResultNoRule && Farming::IsOrderItem(player->GetGUID(), facts.entry);
    if (farmed && !fromBags)
        sOrderService.OnFarmLoot(player->GetGUID(), item->GetCount());
    bool const junk = !farmed && verdict.result == LR::ResultNoRule
        && IsJunk(owner, item->GetTemplate(), item->GetItemRandomPropertyId(), JunkQuality(player), KeepWornGear(player));
    if (farmed)
        action = LR::ActionStore;
    else if (junk)
        action = LR::ActionSell;
    else if (verdict.result == LR::ResultNoRule)
    {
        bool const haulers = sPerkService.Has(player->GetGUID(), PerkKind::MaterialsStorage);
        if (!haulers && action == LR::ActionStore)
            action = LR::ActionKeep;
        else if (haulers && !ai && !fromBags && PerkService::IsMaterial(item->GetTemplate()))
            action = LR::ActionStore;
    }
    // A sweep only moves what would leave the bags; what stays is no news.
    std::string why;
    // What an open quest of the character still asks for stays with it, whatever the item is: many quests collect
    // ordinary meat, hides and trinkets ("Redridge Goulash" wants Great Goretusk Snouts, trade goods to the game: a
    // bot killed twelve boars for them and every snout went into the storage). For a character the AI plays always;
    // for the owner's own character unless a rule of his decided.
    // (A deck an open quest of the bot hands in stays with it: its quest comes first, as for any item.)
    if (darkmoon && !NeededForQuest(player, item, ai))
    {
        action = LR::ActionStore;
        why = item->GetTemplate()->StartQuest ? "a Darkmoon deck" : "a Darkmoon card";
    }
    else if (turnIn && !NeededForQuest(player, item, ai))
    {
        action = LR::ActionStore;
        why = "a reputation turn-in";
    }
    else if (action != LR::ActionKeep && (ai || verdict.result == LR::ResultNoRule) && NeededForQuest(player, item, ai))
    {
        action = LR::ActionKeep;
        why = "for its quest";
    }
    // What the AI will use itself stays with it, whatever the rules say (an explicit "keep" keeps it anyway).
    else if (ai && action != LR::ActionKeep && IsToolOf(player, item->GetTemplate()) &&
        (item->GetTemplate()->TotemCategory || !_spellTools.count(facts.entry) || player->GetItemCount(facts.entry) <= 1))
    {
        // A profession tool (mining pick, skinning knife, blacksmith hammer, rod): its spells need it in the bags. A
        // fishing pole likewise (a white weapon to the game: it was sold as junk). And the tool a recipe names by
        // its item (the Jeweler's Kit, which has no totem category: a sweep stored it every minute and the
        // profession plan handed out the next, 71 -> 114 kits in one guild's storage); one of those stays.
        action = LR::ActionKeep;
        why = "a tool";
    }
    else if (ai && action != LR::ActionKeep && !farmed && BotDriver::WantsItem(player, item))
    {
        action = LR::ActionKeep;
        why = "the bot uses it";
    }
    // A piece for the dungeon set of a tank or healer (DungeonRoles.h): it stays, and goes into its role store. Junk
    // by the junk rule (white weapons and armour) too: a role piece is never junk.
    else if (ai && action != LR::ActionKeep && sRoleService.Wants(player, item))
    {
        action = LR::ActionKeep;
        why = "for its dungeon role";
        sRoleService.LookSoon(player->GetGUID());
    }
    // Junk gear the owner's own character would put on stays with it (a bot's upgrades: WantsItem above).
    else if (!ai && junk && WouldWear(player, item->GetTemplate()))
    {
        action = guild.settings.defaultPlayer == LR::ActionSell ? uint8(LR::ActionKeep) : guild.settings.defaultPlayer;
        why = "an upgrade for it";
    }

    // Ammunition never goes into the storage from loot or the bags (only what a guild character made does): what an
    // AI-played character does not shoot is sold at vendor price, the owner's own character keeps it.
    if (action == LR::ActionStore && item->GetTemplate()->Class == ITEM_CLASS_PROJECTILE)
    {
        // (Most ammunition below level 40 has no vendor price: that is deleted.)
        action = !ai ? LR::ActionKeep : item->GetTemplate()->SellPrice ? LR::ActionSell : LR::ActionDelete;
        why = ai ? "ammunition it does not shoot" : "ammunition is not stored";
    }

    if (fromBags && action == LR::ActionKeep)
        return;
    // soulbound and the like stay; no log line every sweep
    if (fromBags && action == LR::ActionStore && StorageService::RefuseReason(item))
        return;

    if (why.empty() && action == LR::ActionStore && item->IsSoulBound() && !StorageService::RefuseReason(item))
        why = "never worn, bound on pickup";
    if (why.empty() && farmed && action == LR::ActionStore)
        why = "its farm order";

    ChatBatch* batch = ai ? nullptr : &chat[player->GetGUID()];
    LootLogEntry entry;
    entry.time = Now();
    entry.character = player->GetGUID();
    entry.entry = facts.entry;
    entry.randomProperty = item->GetItemRandomPropertyId();
    entry.count = item->GetCount();
    if (why.empty() && junk)
        why = "junk";
    entry.why = fromBags && why.empty() ? std::string("from the bags") : why;

    // Act and record (Act fills outcome, money and why of the entry through the guild's last log line).
    guild.log.push_back(entry);
    Act(player, owner, guild, item, action, batch);
    while (guild.log.size() > LootLogSize)
        guild.log.pop_front();
    guild.newLog.push_back(guild.log.back());
}

void LootService::Act(Player* player, uint32 owner, GuildLoot& guild, Item* item, uint8 action, ChatBatch* chat)
{
    LootLogEntry& entry = guild.log.back();
    ItemTemplate const* proto = item->GetTemplate();
    uint32 const count = item->GetCount();
    uint8 outcome = action;

    // The fallback of mod-loot-filter: what the storage takes goes there, anything else stays in the bags.
    auto storeOrKeep = [&](char const* keptWhy)
    {
        std::string error;
        if (sStorageService.DepositItem(player, item, error))
        {
            outcome = LR::ActionStore;
            return;
        }
        outcome = LR::ActionKeep;
        if (entry.why.empty())
            entry.why = keptWhy ? keptWhy : error;
    };

    switch (action)
    {
        case LR::ActionStore:
            storeOrKeep(nullptr);
            break;
        case LR::ActionSell:
        {
            uint64 const money = uint64(proto->SellPrice) * count;
            if (!money)
                storeOrKeep("no vendor price");
            else if (uint64(player->GetMoney()) + money > MAX_MONEY_AMOUNT)
                storeOrKeep("gold limit");
            else
            {
                {
                    GuildBank::IncomeSource source("junk the loot filter sold");
                    player->ModifyMoney(int32(money));
                }
                player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
                sStorageService.Touch(player);
                entry.money = money;
            }
            break;
        }
        case LR::ActionDisenchant:
        {
            if (!proto->DisenchantID)
            {
                storeOrKeep("cannot be disenchanted");
                break;
            }
            // The materials of a disenchant (no Enchanting skill needed, as in mod-loot-filter): to the guild storage,
            // or the bags, or the mail.
            Loot loot;
            loot.FillLoot(proto->DisenchantID, LootTemplates_Disenchant, player, true);
            player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
            for (LootItem const& lootItem : loot.items)
            {
                ItemTemplate const* matProto = sObjectMgr->GetItemTemplate(lootItem.itemid);
                if (!matProto)
                    continue;
                if (sStorageService.Add(owner, matProto, 0, lootItem.count))
                    continue;
                ItemPosCountVec dest;
                if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, lootItem.itemid, lootItem.count) == EQUIP_ERR_OK)
                {
                    if (Item* newItem = player->StoreNewItem(dest, lootItem.itemid, true))
                        player->SendNewItem(newItem, lootItem.count, true, false);
                }
                else
                    player->SendItemRetrievalMail(lootItem.itemid, lootItem.count);
            }
            break;
        }
        case LR::ActionDelete:
            player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
            break;
        default:
            outcome = LR::ActionKeep;
            break;
    }

    entry.outcome = outcome;
    switch (outcome)
    {
        case LR::ActionStore:
            guild.settings.totalStored += count;
            guild.pendingStored += count;
            if (chat)
                chat->stored += count;
            break;
        case LR::ActionSell:
            guild.settings.totalSold += entry.money;
            guild.pendingSold += entry.money;
            if (chat)
            {
                chat->sold += count;
                chat->money += entry.money;
            }
            break;
        case LR::ActionDisenchant:
            ++guild.settings.totalDisenchanted;
            ++guild.pendingDisenchanted;
            if (chat)
                ++chat->disenchanted;
            break;
        case LR::ActionDelete:
            guild.settings.totalDeleted += count;
            guild.pendingDeleted += count;
            if (chat)
                chat->deleted += count;
            break;
        default:
            if (chat)
                chat->kept += count;
            break;
    }

    if (GetSettings().debug)
        LOG_INFO("module", "mod-guild-adventure: loot of {}: {} x {} -> action {}, outcome {}{}", player->GetName(), count,
            proto->ItemId, action, outcome, entry.why.empty() ? std::string() : " (" + entry.why + ")");
}

// ---------------------------------------------------------------------------------------------------------------
// Gear it has worn (the design log section 7 WG1)
// ---------------------------------------------------------------------------------------------------------------
// A bind-on-equip piece is soulbound once it was put on, and the guild storage takes no soulbound item (DL1 lets in only
// never-worn bind-on-pickup pieces). So the green pieces a bot wore and replaced stayed in its bags for good: the junk
// rule sells grey and white only, and mod-playerbots' own sale at the vendor skips them for an enchanter (it rates a
// soulbound piece it could disenchant "disenchant" and never does it). On 2026-11-14 four bots of one guild stood at
// 0 free bag slots with 15 to 34 such pieces each, and a quest object could not be looted.

char const* LootService::WornGearKeeps(Player* player, Item* item) const
{
    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
    if (!player || !proto || item->IsEquipped() || item->GetOwnerGUID() != player->GetGUID())
        return "not in its bags";
    if (!item->IsSoulBound() || item->IsBoundAccountWide())
        return "not soulbound";
    if ((proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR) || proto->InventoryType == INVTYPE_NON_EQUIP)
        return "no weapon or armour";
    if (proto->Quality != ITEM_QUALITY_UNCOMMON && proto->Quality != ITEM_QUALITY_RARE)
        return "not green or blue";
    if (StorageService::RefuseReason(proto) || NeededForQuest(player, item))
        return "a quest item";
    // Never worn: the guild storage takes it (a sweep puts it there).
    if (sStorageService.IsMarkedUnworn(item))
        return "never worn";
    // A unique-equipped piece (no limit of how many it may carry) goes like any other worn piece, also one that came
    // out of the guild storage (the owner, 2026-11-26, WG1: the worn copies of an Aquamarine Signet stayed in the bags for
    // good and kept mod-playerbots from putting the better copy on). Soulbound and not marked never-worn, it was worn.
    bool const uniqueEquipped = !proto->MaxCount && proto->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE);
    if (!uniqueEquipped && sStorageService.CameFromStorage(item))
        return "it came out of the guild storage";
    if (proto->MaxCount)
        return "unique";
    uint32 const owner = sStorageService.GetOwnerOf(player);
    int32 const randomProperty = item->GetItemRandomPropertyId();
    if (sStorageService.GetReserve(owner, proto->ItemId, randomProperty) ||
        (randomProperty && sStorageService.GetReserve(owner, proto->ItemId, 0)))
        return "the guild keeps a reserve of it";
    if (IsToolOf(player, proto) || (proto->Class == ITEM_CLASS_WEAPON &&
        (proto->SubClass == ITEM_SUBCLASS_WEAPON_MISC || proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE)))
        return "a tool";
    if (BotDriver::WantsItem(player, item))
        return "it would wear it";
    if (sRoleService.Wants(player, item) || sRoleService.IsRoleUpgrade(player, proto->ItemId, randomProperty))
        return "for its dungeon role";
    // A blue bind-on-pickup piece of a class that can tank or heal, while the guild has given it no dungeon role yet:
    // whether it would serve a role set nobody knows, so it stays.
    if (proto->Quality >= ITEM_QUALITY_RARE && proto->Bonding == BIND_WHEN_PICKED_UP &&
        sRoleService.GetRole(player->GetGUID()) == DungeonRole::None && !sRoleService.RolesOf(owner, player->getClass()).empty())
        return "a blue piece for a dungeon role its class can fill";
    return nullptr;
}

uint32 LootService::HandleWornGear(Player* player, Creature* vendor)
{
    if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld() || !IsAIPlayed(player) || player->IsInCombat() ||
        !GetSettings().lootFilterEnable || !GetSettings().storageEnable)
        return 0;
    uint32 const owner = sStorageService.GetOwnerOf(player);
    if (!owner)
        return 0;
    // Quest items no quest needs any more go first (QI1).
    uint32 done = HandleSpareQuestItems(player, vendor);
    uint32 const mode = uint32(sBotSettings.Get(player, Setting::LootWornGear));
    if (!mode)
        return done;
    bool const sell = mode == 1 && vendor && vendor->IsVendor();
    uint32 const skill = player->HasSkill(SKILL_ENCHANTING) ? player->GetSkillValue(SKILL_ENCHANTING) : 0;
    auto canDisenchant = [&](ItemTemplate const* proto)
    {
        return skill && CraftOrders::CanDisenchant(proto->ItemId) && uint32(proto->RequiredDisenchantSkill) <= skill;
    };

    std::vector<Item*> pieces;
    auto look = [&](Item* item)
    {
        if (!item || WornGearKeeps(player, item))
            return;
        ItemTemplate const* proto = item->GetTemplate();
        if (canDisenchant(proto) || (sell && proto->SellPrice))
            pieces.push_back(item);
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        look(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = player->GetBagByPos(bagSlot))
            for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                look(bag->GetItemByPos(uint8(i)));
    if (pieces.empty())
        return done;

    GuildLoot& guild = Guild(owner);
    for (Item* item : pieces)
    {
        ItemTemplate const* proto = item->GetTemplate();
        uint32 const entry = proto->ItemId;
        int32 const randomProperty = item->GetItemRandomPropertyId();
        uint32 const count = item->GetCount();
        std::string const name = proto->Name1;
        if (canDisenchant(proto))
        {
            // The same table a disenchant in the game rolls on (as the guild's disenchants of storage items do); the
            // materials go into the guild storage, what it does not take into the bags (or the mail).
            Loot loot;
            loot.FillLoot(proto->DisenchantID, LootTemplates_Disenchant, player, true);
            player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
            std::string got;
            for (LootItem const& lootItem : loot.items)
            {
                ItemTemplate const* material = sObjectMgr->GetItemTemplate(lootItem.itemid);
                if (!material)
                    continue;
                got += Acore::StringFormat("{}{} x {}", got.empty() ? "" : ", ", uint32(lootItem.count), material->Name1);
                if (sStorageService.Add(owner, material, 0, lootItem.count))
                    continue;
                ItemPosCountVec dest;
                if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, lootItem.itemid, lootItem.count) == EQUIP_ERR_OK)
                {
                    if (Item* newItem = player->StoreNewItem(dest, lootItem.itemid, true))
                        player->SendNewItem(newItem, lootItem.count, true, false);
                }
                else
                    player->SendItemRetrievalMail(lootItem.itemid, lootItem.count);
            }
            sStorageService.Touch(player);
            ++guild.settings.totalDisenchanted;
            ++guild.pendingDisenchanted;
            AddStorageLog(owner, player->GetGUID(), entry, randomProperty, count, LR::ActionDisenchant, 0,
                "worn, it will not wear it again: " + (got.empty() ? std::string("nothing") : got));
            LOG_INFO("module.ga.activity", "activity: {} disenchanted its worn {} (it will not wear it again): {} into the guild storage",
                player->GetName(), name, got.empty() ? "nothing" : got);
        }
        else
        {
            uint64 const money = uint64(proto->SellPrice) * count;
            if (!money || uint64(player->GetMoney()) + money > MAX_MONEY_AMOUNT)
                continue;
            {
                // The guild tax's line names the sale.
                std::string const what = Acore::StringFormat("selling its worn {} to a vendor", name);
                GuildBank::IncomeSource source(what.c_str());
                player->ModifyMoney(int32(money));
            }
            player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
            sStorageService.Touch(player);
            guild.settings.totalSold += money;
            guild.pendingSold += money;
            AddStorageLog(owner, player->GetGUID(), entry, randomProperty, count, LR::ActionSell, money,
                Acore::StringFormat("worn, it will not wear it again: sold at {}", vendor->GetName()));
            LOG_INFO("module.ga.activity", "activity: {} sold its worn {} at {} for {}c (it will not wear it again)", player->GetName(),
                name, vendor->GetName(), money);
        }
        ++done;
    }
    return done;
}

// ---------------------------------------------------------------------------------------------------------------
// Quest items no quest needs any more (the design log section 7 QI1)
// ---------------------------------------------------------------------------------------------------------------
// the owner, 2026-11-18: [quote left out].
// The guild storage takes no quest item and nothing sold them, so they filled the bags once the worn gear was gone
// (Wartank 25 stacks: scourgestones, power crystals, a photograph of a quest long done).

namespace
{
    // The quests that ask for an item (RequiredItemId), built once.
    std::unordered_map<uint32, std::vector<Quest const*>> const& QuestsAskingFor()
    {
        static std::unordered_map<uint32, std::vector<Quest const*>> index;
        static bool built = false;
        if (!built)
        {
            built = true;
            for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
                for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    if (quest->RequiredItemId[i])
                        index[quest->RequiredItemId[i]].push_back(quest);
        }
        return index;
    }

    // Whether the character can still do the quest for experience: below the level cap, not done (a repeatable one
    // may be done again), for its race and class, and not grey for it (the bots leave grey quests alone, GQ1).
    bool StillGivesXp(Player* player, Quest const* quest)
    {
        if (!quest || player->GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
            return false;
        if (!quest->IsRepeatable() && player->GetQuestRewardStatus(quest->GetQuestId()))
            return false;
        if (!player->SatisfyQuestClass(quest, false) || !player->SatisfyQuestRace(quest, false))
            return false;
        int32 const level = quest->GetQuestLevel() > 0 ? quest->GetQuestLevel() : int32(player->GetLevel());
        return level > int32(Acore::XP::GetGrayLevel(player->GetLevel())) && quest->XPValue(player->GetLevel()) > 0;
    }

    // Whether a quest in the character's log uses the item: asks for it or hands it out (its source item, a drop).
    bool UsedByLog(Player* player, uint32 entry)
    {
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(player->GetQuestSlotQuestId(slot));
            if (!quest)
                continue;
            if (quest->GetSrcItemId() == entry)
                return true;
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i] == entry)
                    return true;
            for (uint8 i = 0; i < QUEST_SOURCE_ITEM_IDS_COUNT; ++i)
                if (quest->ItemDrop[i] == entry)
                    return true;
        }
        return false;
    }
}

char const* LootService::QuestItemKeeps(Player* player, Item* item) const
{
    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
    if (!player || !proto || item->IsEquipped() || item->GetOwnerGUID() != player->GetGUID())
        return "not in its bags";
    bool const questKind = proto->Class == ITEM_CLASS_QUEST || proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_QUEST_ITEM1;
    if (!questKind && !proto->StartQuest)
        return "no quest item";
    // Green or better (Darkmoon cards are blue quest items worth a deck): not by this rule.
    if (proto->Quality > ITEM_QUALITY_NORMAL)
        return "green or better";
    if (UsedByLog(player, proto->ItemId))
        return "a quest in its log uses it";
    if (proto->HasFlag(ITEM_FLAG_HAS_LOOT))
        return "it holds something";
    // A quest starter only goes once its quest gives no experience any more (the owner).
    if (proto->StartQuest)
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(proto->StartQuest))
            if (player->GetQuestStatus(quest->GetQuestId()) == QUEST_STATUS_NONE && StillGivesXp(player, quest))
                return "it starts a quest that still gives experience";
    auto asking = QuestsAskingFor().find(proto->ItemId);
    if (asking != QuestsAskingFor().end())
        for (Quest const* quest : asking->second)
        {
            if (StillGivesXp(player, quest))
                return "a quest it can still do for experience asks for it";
            // A turn-in of a repeatable quest that gives reputation stays until the owner decides (the Argent Dawn's and
            // the Cenarion Circle's: Core of Elements, Bone Fragments, Evil Bat Eyes, Scourgestones; about 10,000 lay in
            // his guild's bags on 2026-11-25, and the bag sweep every half hour would have sold or deleted them at once).
            if (quest->IsRepeatable() || quest->IsDailyOrWeekly())
                for (uint8 i = 0; i < QUEST_REPUTATIONS_COUNT; ++i)
                    if (quest->RewardFactionId[i] && (quest->RewardFactionValueId[i] || quest->RewardFactionValueIdOverride[i]))
                        return "a repeatable quest that gives reputation asks for it";
        }
    return nullptr;
}

uint32 LootService::HandleSpareQuestItems(Player* player, Creature* vendor)
{
    if (!sBotSettings.IsOn(player, Setting::LootSpareQuestItems))
        return 0;
    uint32 const owner = sStorageService.GetOwnerOf(player);
    if (!owner)
        return 0;
    bool const atVendor = vendor && vendor->IsVendor();
    std::vector<Item*> spare;
    auto look = [&](Item* item)
    {
        // What sells waits for a vendor; what does not is deleted where the character stands (the owner's order).
        if (item && !QuestItemKeeps(player, item) && (atVendor || !item->GetTemplate()->SellPrice))
            spare.push_back(item);
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        look(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = player->GetBagByPos(bagSlot))
            for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                look(bag->GetItemByPos(uint8(i)));

    GuildLoot& guild = Guild(owner);
    uint32 done = 0;
    for (Item* item : spare)
    {
        ItemTemplate const* proto = item->GetTemplate();
        uint32 const entry = proto->ItemId;
        uint32 const count = item->GetCount();
        int32 const randomProperty = item->GetItemRandomPropertyId();
        std::string const name = proto->Name1;
        uint64 const money = uint64(proto->SellPrice) * count;
        if (money)
        {
            if (uint64(player->GetMoney()) + money > MAX_MONEY_AMOUNT)
                continue;
            {
                std::string const what = Acore::StringFormat("selling {} to a vendor (no quest needs it)", name);
                GuildBank::IncomeSource source(what.c_str());
                player->ModifyMoney(int32(money));
            }
            guild.settings.totalSold += money;
            guild.pendingSold += money;
        }
        else
        {
            guild.settings.totalDeleted += count;
            guild.pendingDeleted += count;
        }
        player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
        sStorageService.Touch(player);
        AddStorageLog(owner, player->GetGUID(), entry, randomProperty, count,
            money ? uint8(LR::ActionSell) : uint8(LR::ActionDelete), money,
            money ? Acore::StringFormat("no quest needs it any more: sold at {}", vendor->GetName()) : std::string("no quest needs it any more"));
        if (money)
            LOG_INFO("module.ga.activity", "activity: {} sold {} x {} at {} for {}c: no quest needs it any more", player->GetName(), count,
                name, vendor->GetName(), money);
        else
            LOG_INFO("module.ga.activity", "activity: {} deleted {} x {}: no quest needs it any more and no vendor buys it",
                player->GetName(), count, name);
        ++done;
    }
    return done;
}

// ---------------------------------------------------------------------------------------------------------------
// Junk
// ---------------------------------------------------------------------------------------------------------------

void LootService::LoadNeededItems()
{
    uint32 const start = getMSTime();
    _neededItems.clear();
    _wornCache.clear();

    // Reagents of profession recipes (first aid, cooking and fishing included): crafting materials, whatever their
    // quality or class (Ruined Leather Scraps are grey, a few cooking ingredients are misc items).
    for (uint32 spellId = 1; spellId < sSpellMgr->GetSpellInfoStoreSize(); ++spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            continue;
        bool hasReagent = false;
        for (int32 reagent : info->Reagent)
            hasReagent = hasReagent || reagent > 0;
        if (!hasReagent)
            continue;
        bool profession = false;
        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto itr = bounds.first; itr != bounds.second && !profession; ++itr)
            profession = IsProfessionSkill(itr->second->SkillLine);
        if (!profession)
            continue;
        for (int32 reagent : info->Reagent)
            if (reagent > 0)
                _neededItems.insert(uint32(reagent));
    }
    std::size_t const reagents = _neededItems.size();

    // The tools profession recipes name by their item (never junk either).
    _spellTools.clear();
    for (uint32 spellId = 1; spellId < sSpellMgr->GetSpellInfoStoreSize(); ++spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || (!info->Totem[0] && !info->Totem[1]))
            continue;
        bool profession = false;
        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto itr = bounds.first; itr != bounds.second && !profession; ++itr)
            profession = IsProfessionSkill(itr->second->SkillLine);
        if (!profession)
            continue;
        for (uint32 tool : info->Totem)
            if (tool)
            {
                _spellTools.insert(tool);
                _neededItems.insert(tool);
            }
    }

    // What quests ask for or hand out (a white "Gnoll Paw" a quest wants is no junk).
    for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
    {
        for (uint32 item : quest->RequiredItemId)
            if (item)
                _neededItems.insert(item);
        for (uint32 item : quest->ItemDrop)
            if (item)
                _neededItems.insert(item);
        if (uint32 const item = quest->GetSrcItemId())
            _neededItems.insert(item);
    }

    LOG_INFO("module", "mod-guild-adventure: junk: {} items are profession reagents, {} more are asked for by quests ({} ms)",
        reagents, _neededItems.size() - reagents, GetMSTimeDiffToNow(start));
}

bool LootService::IsToolOf(Player* player, ItemTemplate const* proto) const
{
    if (!player || !proto)
        return false;
    if (proto->TotemCategory || (proto->Class == ITEM_CLASS_WEAPON && proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE))
        return true;
    // The module's own list of profession tools (what the profession plan gives a bot).
    if (Professions::IsTool(proto->ItemId))
        return true;
    if (!_spellTools.count(proto->ItemId))
        return false;
    for (auto const& [spellId, spell] : player->GetSpellMap())
    {
        if (!spell || spell->State == PLAYERSPELL_REMOVED)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (info && (info->Totem[0] == proto->ItemId || info->Totem[1] == proto->ItemId))
            return true;
    }
    return false;
}

int32 LootService::JunkQuality(Player const* looter)
{
    if (!GetSettings().lootFilterEnable || !(AllowedMask() & (1u << LR::ActionSell)))
        return -1;
    if (!sBotSettings.IsOn(looter, Setting::LootSellJunk))
        return -1;
    return std::min<int32>(sBotSettings.Get(looter, Setting::LootSellJunkQuality), ITEM_QUALITY_NORMAL);
}

int32 LootService::JunkQualityOfGuild(uint32 owner)
{
    if (!GetSettings().lootFilterEnable || !(AllowedMask() & (1u << LR::ActionSell)))
        return -1;
    if (!sBotSettings.GetForGuild(owner, Setting::LootSellJunk))
        return -1;
    return std::min<int32>(sBotSettings.GetForGuild(owner, Setting::LootSellJunkQuality), ITEM_QUALITY_NORMAL);
}

bool LootService::IsJunkKind(ItemTemplate const* proto, int32 maxQuality) const
{
    if (!proto || maxQuality < 0 || int32(proto->Quality) > maxQuality || !proto->SellPrice)
        return false;
    // Never: quest items, keys, tools (pick, knife, rod), crafting materials and what a quest asks for.
    if (proto->Class == ITEM_CLASS_QUEST || proto->Class == ITEM_CLASS_KEY || proto->Bonding == BIND_QUEST_ITEM
        || proto->Bonding == BIND_QUEST_ITEM1 || proto->StartQuest || proto->TotemCategory || Professions::IsTool(proto->ItemId) || _neededItems.count(proto->ItemId))
        return false;
    // Grey: everything else only a vendor pays for.
    if (proto->Quality == ITEM_QUALITY_POOR)
        return true;
    // White: weapons (not a fishing pole) and armour, and misc junk that does nothing when used. Trade goods,
    // consumables, bags, ammunition, recipes, gems and the like stay what they are.
    switch (proto->Class)
    {
        case ITEM_CLASS_WEAPON:
            return proto->SubClass != ITEM_SUBCLASS_WEAPON_FISHING_POLE;
        case ITEM_CLASS_ARMOR:
            return true;
        case ITEM_CLASS_MISC:
            if (proto->SubClass != ITEM_SUBCLASS_JUNK)
                return false;
            for (_Spell const& spell : proto->Spells)
                if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                    return false;
            return true;
        default:
            return false;
    }
}

bool LootService::WornByGuild(uint32 owner, ItemTemplate const* proto, int32 randomProperty)
{
    uint32 const now = getMSTime();
    uint64 const key = (uint64(owner) << 32) | proto->ItemId;
    auto cached = _wornCache.find(key);
    if (cached != _wornCache.end() && getMSTimeDiff(cached->second.first, now) < WornCacheMs)
        return cached->second.second;

    bool worn = false;
    if (AdventureGuild* guild = sGuildAdventureMgr.FindGuild(owner))
    {
        std::vector<ObjectGuid> characters;
        characters.push_back(guild->leader);
        for (auto const& [guid, bot] : guild->bots)
            characters.push_back(guid);
        for (ObjectGuid const& guid : characters)
        {
            Player* player = ObjectAccessor::FindPlayer(guid);
            if (!player || !player->IsInWorld())
                continue;
            // (A bot: for levelling, or for the dungeon set of a tank or healer.)
            worn = BotDriver::GetAI(player) ? BotDriver::IsUpgrade(player, proto->ItemId, randomProperty) ||
                sRoleService.IsRoleUpgrade(player, proto->ItemId, randomProperty) : WouldWear(player, proto);
            if (worn)
                break;
        }
    }
    if (_wornCache.size() > 4096)
        _wornCache.clear();
    _wornCache[key] = { now, worn };
    return worn;
}

bool LootService::KeepWornGear(Player const* looter)
{
    return sBotSettings.IsOn(looter, Setting::LootKeepWornGear);
}

bool LootService::KeepWornGearOfGuild(uint32 owner)
{
    return sBotSettings.GetForGuild(owner, Setting::LootKeepWornGear) != 0;
}

bool LootService::WouldWear(Player* player, ItemTemplate const* proto)
{
    if (!player || !proto || player->CanUseItem(proto) != EQUIP_ERR_OK)
        return false;
    uint8 const slot = player->FindEquipSlot(proto, NULL_SLOT, true);
    if (slot == NULL_SLOT)
        return false;
    Item const* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    return !current || current->GetTemplate()->ItemLevel < proto->ItemLevel;
}

bool LootService::IsJunk(uint32 owner, ItemTemplate const* proto, int32 randomProperty, int32 maxQuality, bool keepWorn)
{
    if (!IsJunkKind(proto, maxQuality))
        return false;
    // the owner, 2026-11-13: [quote left out]. Only with LootFilter.KeepWornGear (or for
    // the storage's Sell junk) does gear a guild character in the world would wear go the default way instead.
    if (keepWorn && (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR) && owner &&
        WornByGuild(owner, proto, randomProperty))
        return false;
    return true;
}

JunkSale LootService::SellStoredJunk(uint32 owner, Player* player, bool sell)
{
    JunkSale sale;
    int32 const maxQuality = JunkQualityOfGuild(owner);
    if (!owner || maxQuality < 0 || (sell && !player))
        return sale;
    for (StorageItem const& item : sStorageService.List(owner))
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.entry);
        // What a guild character in the world would put on stays: the storage pass hands it out (StorageSupply).
        if (!proto || !item.amount || !IsJunk(owner, proto, item.randomProperty, maxQuality, true))
            continue;
        uint32 amount = item.amount;
        if (sell)
        {
            // The storage's items, the guild's gold: into the guild bank (2026-11-13; it went to the played character).
            amount = sStorageService.Remove(owner, item.entry, item.randomProperty, amount);
            if (!amount)
                continue;
            if (!GuildBank::Deposit(owner, uint64(proto->SellPrice) * amount, Acore::StringFormat("sold {} x {} from the guild storage "
                "as junk", amount, proto->Name1)))
            {
                sStorageService.Add(owner, proto, item.randomProperty, amount); // no WoW guild: nothing is sold
                sale.capped = true;
                continue;
            }
            AddStorageLog(owner, player->GetGUID(), item.entry, item.randomProperty, amount, LR::ActionSell,
                uint64(proto->SellPrice) * amount, "sold from the guild storage as junk, into the guild bank");
        }
        ++sale.kinds;
        sale.items += amount;
        sale.money += uint64(proto->SellPrice) * amount;
    }
    if (sell && sale.money)
    {
        GuildLoot& guild = Guild(owner);
        guild.settings.totalSold += sale.money;
        guild.pendingSold += sale.money;
        LOG_INFO("module", "mod-guild-adventure: {} sold {} junk items ({} kinds) from the guild storage for {} into the guild bank{}",
            player->GetName(), sale.items, sale.kinds, FormatMoney(sale.money), sale.capped ? " (not all: no guild bank)" : "");
    }
    return sale;
}

// ---------------------------------------------------------------------------------------------------------------
// The owner's own orders on the guild storage: sell, disenchant (docs/storage.md "Selling and disenchanting")
// ---------------------------------------------------------------------------------------------------------------

void LootService::AddStorageLog(uint32 owner, ObjectGuid character, uint32 entry, int32 randomProperty, uint32 count,
    uint8 outcome, uint64 money, std::string why)
{
    GuildLoot& guild = Guild(owner);
    LootLogEntry line;
    line.time = Now();
    line.character = character;
    line.entry = entry;
    line.randomProperty = randomProperty;
    line.count = count;
    line.outcome = outcome;
    line.money = money;
    line.why = std::move(why);
    guild.log.push_back(line);
    while (guild.log.size() > LootLogSize)
        guild.log.pop_front();
    guild.newLog.push_back(line);
}

void LootService::LogStorageMove(uint32 owner, ObjectGuid character, std::string const& name, uint32 entry,
    int32 randomProperty, uint32 count, bool deposited, std::string const& how)
{
    if (!owner || !count)
        return;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    AddStorageLog(owner, character, entry, randomProperty, count, deposited ? LogDeposited : LogTaken, 0, how);
    LOG_INFO("module.ga.activity", "activity: {} {} {} x {} {} the guild storage of owner {} ({})", name.empty() ? "the guild" : name,
        deposited ? "put" : "took", count, proto ? proto->Name1 : std::to_string(entry), deposited ? "into" : "out of", owner, how);
}

uint32 LootService::SellStored(uint32 owner, Player* player, uint32 entry, int32 randomProperty, uint32 count,
    uint64& money, std::string& error)
{
    money = 0;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    uint32 const have = owner ? sStorageService.GetAmount(owner, entry, randomProperty) : 0;
    if (!player || !proto || !have)
    {
        error = "nothing";
        return 0;
    }
    if (!proto->SellPrice)
    {
        error = "no_price";
        return 0;
    }
    if (!GuildBank::WowGuildOf(owner))
    {
        error = "gold_limit"; // no guild bank to take the gold
        return 0;
    }
    uint32 amount = count ? std::min(count, have) : have;
    amount = sStorageService.Remove(owner, entry, randomProperty, amount);
    if (!amount)
    {
        error = "nothing";
        return 0;
    }
    money = uint64(proto->SellPrice) * amount;
    // The storage's items, the guild's gold: into the guild bank (2026-11-13; it went to the played character).
    if (!GuildBank::Deposit(owner, money, Acore::StringFormat("{} sold {} x {} from the guild storage", player->GetName(), amount,
        proto->Name1)))
    {
        sStorageService.Add(owner, proto, randomProperty, amount);
        money = 0;
        error = "gold_limit";
        return 0;
    }

    GuildLoot& guild = Guild(owner);
    guild.settings.totalSold += money;
    guild.pendingSold += money;
    AddStorageLog(owner, player->GetGUID(), entry, randomProperty, amount, LR::ActionSell, money,
        "sold from the guild storage, into the guild bank");
    LOG_INFO("module", "mod-guild-adventure: {} sold {} x {} from the guild storage for {} into the guild bank", player->GetName(),
        amount, proto->Name1, FormatMoney(money));
    return amount;
}

uint32 LootService::DeleteStored(uint32 owner, Player* player, uint32 entry, int32 randomProperty, uint32 count,
    std::string& error)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    uint32 const have = owner ? sStorageService.GetAmount(owner, entry, randomProperty) : 0;
    if (!player || !proto || !have)
    {
        error = "nothing";
        return 0;
    }
    if (proto->SellPrice)
    {
        error = "has_price"; // what a vendor buys is sold, not destroyed
        return 0;
    }
    uint32 const amount = sStorageService.Remove(owner, entry, randomProperty, count ? std::min(count, have) : have);
    if (!amount)
    {
        error = "nothing";
        return 0;
    }
    GuildLoot& guild = Guild(owner);
    guild.settings.totalDeleted += amount;
    guild.pendingDeleted += amount;
    AddStorageLog(owner, player->GetGUID(), entry, randomProperty, amount, LR::ActionDelete, 0,
        "deleted from the guild storage (no vendor buys it)");
    LOG_INFO("module.ga.activity", "activity: {} deleted {} x {} from the guild storage (no vendor price)", player->GetName(), amount,
        proto->Name1);
    return amount;
}

int32 LootService::StorageDisenchantSkill(ItemTemplate const* proto)
{
    // the owner, 2026-11-13: [quote left out].
    if (!proto || (proto->Quality != ITEM_QUALITY_UNCOMMON && proto->Quality != ITEM_QUALITY_RARE) ||
        !CraftOrders::CanDisenchant(proto->ItemId))
        return -1;
    return int32(CraftOrders::DisenchantSkill(proto->ItemId));
}

Player* LootService::BestEnchanter(uint32 owner, uint32& skill)
{
    skill = 0;
    AdventureGuild const* guild = sGuildAdventureMgr.FindGuild(owner);
    return guild ? CraftOrders::BestEnchanter(*guild, &skill) : nullptr;
}

StorageDisenchant LootService::DisenchantFromStorage(uint32 owner, uint32 entry, int32 randomProperty, std::string const& forWhom)
{
    StorageDisenchant out;
    AdventureGuild const* guild = sGuildAdventureMgr.FindGuild(owner);
    int32 const needs = StorageDisenchantSkill(sObjectMgr->GetItemTemplate(entry));
    if (!guild || needs < 0)
    {
        out.error = "not_disenchantable";
        out.text = "Only green and blue weapons and armour can be disenchanted from the guild storage.";
        return out;
    }
    uint32 skill = 0;
    Player* enchanter = CraftOrders::BestEnchanter(*guild, &skill);
    if (!enchanter || skill < uint32(needs))
    {
        out.error = enchanter ? "skill" : "no_enchanter";
        out.text = enchanter ? Acore::StringFormat("It needs Enchanting {}; {} has {}.", needs, enchanter->GetName(), skill)
            : std::string("No character of the guild in the world knows Enchanting.");
        return out;
    }
    // The gaps thread's routine (CraftOrders::DisenchantStored): the materials go into the storage, the activity
    // line names the purpose; the storage's own log is ours.
    std::string const purpose = "for " + forWhom;
    DisenchantResult const result = sCraftOrders.DisenchantStored(*guild, entry, randomProperty, 1, purpose.c_str(), enchanter);
    out.text = result.text;
    if (!result.done)
    {
        out.error = "nothing";
        return out;
    }
    out.done = result.done;
    out.enchanter = result.enchanterName;
    for (auto const& [material, amount] : result.materials)
    {
        ItemTemplate const* materialProto = sObjectMgr->GetItemTemplate(material);
        out.materials += Acore::StringFormat("{}{} x {}", out.materials.empty() ? "" : ", ", amount,
            materialProto ? materialProto->Name1 : std::to_string(material));
    }
    if (out.materials.empty())
        out.materials = "nothing";

    GuildLoot& loot = Guild(owner);
    loot.settings.totalDisenchanted += result.done;
    loot.pendingDisenchanted += result.done;
    AddStorageLog(owner, result.enchanter, entry, randomProperty, result.done, LR::ActionDisenchant, 0,
        Acore::StringFormat("disenchanted from the guild storage for {}: {}", forWhom, out.materials));
    return out;
}
