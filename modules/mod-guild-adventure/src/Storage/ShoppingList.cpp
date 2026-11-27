/*
 * ShoppingList: see ShoppingList.h.
 */

#include "ShoppingList.h"
#include "BotSettings.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GadvProtocol.h"
#include "GameTime.h"
#include "GuildAdventureUtil.h"
#include "GuildAdventureMgr.h"
#include "GuildBank.h"
#include "GuildBudget.h"
#include "Item.h"
#include "Log.h"
#include "LootService.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "StorageService.h"
#include "StringFormat.h"
#include "WorldIndex.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>

using namespace GuildAdventure;

namespace
{
    thread_local bool t_buying = false;

    struct BuyingScope
    {
        BuyingScope() { t_buying = true; }
        ~BuyingScope() { t_buying = false; }
    };

    char const* ActivityLog = "module.ga.activity";
    constexpr uint32 NoteGapSeconds = 30 * MINUTE;
    // Faction templates of a player of each side (human, orc): a vendor whose faction is hostile to it does not trade.
    constexpr uint32 AllianceFactionTemplate = 1;
    constexpr uint32 HordeFactionTemplate = 2;

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    }

    std::string Money(uint64 copper)
    {
        return GuildBudget::MoneyText(uint32(std::min<uint64>(copper, std::numeric_limits<uint32>::max())));
    }

    // The vendor slot that sells `entry` for gold (nullptr: this vendor does not).
    VendorItem const* SlotOf(Creature* vendor, uint32 entry, uint32& slot)
    {
        VendorItemData const* items = vendor ? vendor->GetVendorItems() : nullptr;
        if (!items)
            return nullptr;
        for (uint32 i = 0; i < items->GetItemCount(); ++i)
            if (VendorItem const* item = items->GetItem(i))
                if (item->item == entry && !item->ExtendedCost)
                {
                    slot = i;
                    return item;
                }
        return nullptr;
    }
}

bool ShoppingList::IsBuying()
{
    return t_buying;
}

ShoppingList& ShoppingList::Instance()
{
    static ShoppingList instance;
    return instance;
}

void ShoppingList::Load()
{
    _lists.clear();
    _days.clear();
    _vendorItems.clear();
    _notes.clear();

    uint32 count = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT owner_account, item_entry, min_stock, enabled FROM ga_shopping_list"))
        do
        {
            Field const* f = result->Fetch();
            ShoppingEntry entry;
            entry.entry = f[1].Get<uint32>();
            entry.min = f[2].Get<uint32>();
            entry.enabled = f[3].Get<uint8>() != 0;
            _lists[f[0].Get<uint32>()][entry.entry] = entry;
            ++count;
        } while (result->NextRow());
    if (QueryResult result = CharacterDatabase.Query("SELECT owner_account, day, spent FROM ga_shopping_spent"))
        do
        {
            Field const* f = result->Fetch();
            Day& day = _days[f[0].Get<uint32>()];
            day.day = f[1].Get<uint32>();
            day.spent = f[2].Get<uint64>();
        } while (result->NextRow());

    // What vendors sell for gold, and to which side (a vendor hostile to a side's players does not trade with it).
    FactionTemplateEntry const* alliance = sFactionTemplateStore.LookupEntry(AllianceFactionTemplate);
    FactionTemplateEntry const* horde = sFactionTemplateStore.LookupEntry(HordeFactionTemplate);
    for (auto const& [creatureEntry, creature] : *sObjectMgr->GetCreatureTemplates())
    {
        if (!(creature.npcflag & UNIT_NPC_FLAG_VENDOR))
            continue;
        VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(creatureEntry);
        if (!items || items->Empty())
            continue;
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(creature.faction);
        bool const forAlliance = faction && alliance && !faction->IsHostileTo(*alliance);
        bool const forHorde = faction && horde && !faction->IsHostileTo(*horde);
        if (!forAlliance && !forHorde)
            continue;
        for (uint32 i = 0; i < items->GetItemCount(); ++i)
        {
            VendorItem const* vendorItem = items->GetItem(i);
            if (!vendorItem || vendorItem->ExtendedCost)
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vendorItem->item);
            if (!proto)
                continue;
            ShoppingItem& item = _vendorItems[proto->ItemId];
            if (!item.entry)
            {
                item.entry = proto->ItemId;
                item.name = proto->Name1;
                item.quality = proto->Quality;
                item.requiredLevel = proto->RequiredLevel;
                item.price = uint32(std::max<int32>(proto->BuyPrice, 0));
                item.bundle = std::max<uint32>(1, proto->BuyCount);
            }
            item.alliance = item.alliance || forAlliance;
            item.horde = item.horde || forHorde;
        }
    }
    LOG_INFO("module", "mod-guild-adventure: shopping lists: {} entries; {} kinds of item are sold by vendors for gold", count,
        _vendorItems.size());
}

std::vector<ShoppingEntry> ShoppingList::Get(uint32 owner) const
{
    std::vector<ShoppingEntry> list;
    auto itr = _lists.find(owner);
    if (itr != _lists.end())
        for (auto const& [entry, item] : itr->second)
            list.push_back(item);
    return list;
}

char const* ShoppingList::RefuseReason(ItemTemplate const* proto, uint32 team) const
{
    if (!proto)
        return "unknown item";
    if (proto->Class == ITEM_CLASS_PROJECTILE)
        return "ammunition goes into the guild storage only when a guild character made it";
    if (proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_QUEST_ITEM1)
        return "it binds when bought: the guild storage does not take it";
    if (char const* reason = StorageService::RefuseReason(proto))
        return reason;
    ShoppingItem const* item = FindItem(proto->ItemId);
    if (!item || !item->price)
        return "no vendor sells it for gold";
    if ((team == TEAM_HORDE && !item->horde) || (team != TEAM_HORDE && !item->alliance))
        return "no vendor of your side sells it";
    return nullptr;
}

ShoppingItem const* ShoppingList::FindItem(uint32 entry) const
{
    auto itr = _vendorItems.find(entry);
    return itr == _vendorItems.end() ? nullptr : &itr->second;
}

bool ShoppingList::Set(uint32 owner, uint32 team, ShoppingEntry entry, std::string& error)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry.entry);
    if (char const* reason = RefuseReason(proto, team))
    {
        ShoppingItem const* item = proto ? FindItem(proto->ItemId) : nullptr;
        error = !proto || !item || !item->price || std::string(reason).find("side") != std::string::npos ? "NOT_SOLD" : "NOT_STORED";
        error += std::string(": ") + reason;
        return false;
    }
    if (entry.min > MaxMinimum)
    {
        error = Acore::StringFormat("INVALID: at most {} as the minimum", MaxMinimum);
        return false;
    }
    std::map<uint32, ShoppingEntry>& list = _lists[owner];
    if (!list.count(entry.entry) && list.size() >= MaxEntries)
    {
        error = Acore::StringFormat("LIMIT: at most {} items on the shopping list", MaxEntries);
        return false;
    }
    list[entry.entry] = entry;
    Save(owner, entry);
    LOG_INFO(ActivityLog, "activity: the shopping list of owner {}: {} at least {}{}", owner, proto->Name1, entry.min,
        entry.enabled ? "" : " (paused)");
    return true;
}

bool ShoppingList::Delete(uint32 owner, uint32 entry)
{
    auto itr = _lists.find(owner);
    if (itr == _lists.end() || !itr->second.erase(entry))
        return false;
    CharacterDatabase.Execute("DELETE FROM ga_shopping_list WHERE owner_account = {} AND item_entry = {}", owner, entry);
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    LOG_INFO(ActivityLog, "activity: the shopping list of owner {}: {} taken off", owner, proto ? proto->Name1 : std::to_string(entry));
    return true;
}

void ShoppingList::Save(uint32 owner, ShoppingEntry const& entry) const
{
    CharacterDatabase.Execute("REPLACE INTO ga_shopping_list (owner_account, item_entry, min_stock, enabled, updated_at) VALUES "
        "({}, {}, {}, {}, {})", owner, entry.entry, entry.min, entry.enabled ? 1 : 0, Now());
}

std::vector<ShoppingItem> ShoppingList::Search(std::string const& text, uint32 team, std::size_t max) const
{
    std::vector<ShoppingItem> found;
    std::string const needle = Lower(text);
    bool const number = !needle.empty() && std::all_of(needle.begin(), needle.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    uint32 const id = number ? uint32(std::strtoul(needle.c_str(), nullptr, 10)) : 0;
    std::vector<std::pair<int, ShoppingItem const*>> ranked;
    for (auto const& [entry, item] : _vendorItems)
    {
        if (!item.price || (team == TEAM_HORDE ? !item.horde : !item.alliance))
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (RefuseReason(proto, team))
            continue;
        int rank = -1;
        if (number && entry == id)
            rank = 0;
        else if (!number)
        {
            std::string const name = Lower(item.name);
            std::size_t const at = name.find(needle);
            if (at == 0)
                rank = 1;
            else if (at != std::string::npos)
                rank = 2;
        }
        if (rank >= 0)
            ranked.emplace_back(rank, &item);
    }
    std::sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b)
    {
        if (a.first != b.first)
            return a.first < b.first;
        if (a.second->requiredLevel != b.second->requiredLevel)
            return a.second->requiredLevel < b.second->requiredLevel;
        return a.second->name < b.second->name;
    });
    for (auto const& [rank, item] : ranked)
    {
        if (found.size() >= max)
            break;
        found.push_back(*item);
    }
    return found;
}

ShoppingList::Day& ShoppingList::Today(uint32 owner)
{
    Day& day = _days[owner];
    uint32 const today = Now() / DAY;
    if (day.day != today)
    {
        day.day = today;
        day.spent = 0;
    }
    return day;
}

uint64 ShoppingList::SpentToday(uint32 owner)
{
    return Today(owner).spent;
}

bool ShoppingList::MayNote(uint32 owner, uint32 entry, char const* what)
{
    uint32& last = _notes[{ owner, Acore::StringFormat("{}:{}", entry, what) }];
    uint32 const now = Now();
    if (last && now - last < NoteGapSeconds)
        return false;
    last = now;
    return true;
}

std::string ShoppingList::BuyHere(Player* player)
{
    for (ServicePoint const& service : sWorldIndex.GetServices(ServiceType::Vendor))
    {
        if (service.spawn.mapId != player->GetMapId() || player->GetDistance2d(service.spawn.x, service.spawn.y) > 12.0f)
            continue;
        Creature* vendor = player->FindNearestCreature(service.spawn.entry, INTERACTION_DISTANCE);
        if (!vendor)
            continue;
        std::string skipped;
        std::string const bought = BuyAt(player, vendor, &skipped);
        if (!bought.empty())
            return Acore::StringFormat("bought at {}: {}{}", vendor->GetName(), bought, skipped.empty() ? "" : "; not bought: " + skipped);
        return Acore::StringFormat("nothing bought at {}{}", vendor->GetName(), skipped.empty() ?
            std::string(" (nothing on the list it sells is below its minimum)") : ": " + skipped);
    }
    return "no vendor within reach of its hands";
}

std::string ShoppingList::BuyAt(Player* player, Creature* vendor, std::string* skipped)
{
    std::string bought;
    if (!player || !vendor || !vendor->IsVendor() || !sBotSettings.IsOn(player, Setting::ShoppingEnable))
        return bought;
    uint32 const owner = sStorageService.GetOwnerOf(player);
    auto listItr = owner ? _lists.find(owner) : _lists.end();
    if (listItr == _lists.end() || listItr->second.empty())
        return bought;

    uint64 const visitCap = uint64(std::max(sBotSettings.GetForGuild(owner, Setting::ShoppingMaxPerVisit), 0));
    uint64 const dayCap = uint64(std::max(sBotSettings.GetForGuild(owner, Setting::ShoppingMaxPerDay), 0));
    bool const fromPurse = sBotSettings.GetForGuild(owner, Setting::ShoppingFromPurse) != 0;
    std::string const vendorName = vendor->GetName();
    uint64 visitSpent = 0;
    auto note = [&](uint32 entry, char const* what, std::string const& text)
    {
        if (skipped)
            *skipped += (skipped->empty() ? "" : "; ") + text;
        if (MayNote(owner, entry, what))
            LOG_INFO(ActivityLog, "activity: {} does not buy for the guild's shopping list at {}: {}", player->GetName(), vendorName, text);
    };

    for (auto const& [entry, wanted] : listItr->second)
    {
        if (!wanted.enabled || !wanted.min)
            continue;
        uint32 slot = 0;
        VendorItem const* vendorItem = SlotOf(vendor, entry, slot);
        if (!vendorItem)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto || RefuseReason(proto, player->GetTeamId()) || proto->BuyPrice <= 0)
            continue;
        uint32 const have = sStorageService.GetStoredOfEntry(owner, entry);
        if (have >= wanted.min)
            continue;

        uint32 const bundle = std::max<uint32>(1, proto->BuyCount);
        uint32 bundles = (wanted.min - have + bundle - 1) / bundle;
        if (vendorItem->maxcount)
            bundles = std::min(bundles, vendor->GetVendorItemCurrentCount(vendorItem) / bundle);
        if (!bundles)
            continue; // the vendor's limited stock is sold out for now
        float const discount = player->GetReputationPriceDiscount(vendor);
        // (As the core prices one purchase, Player::BuyItemFromVendorSlot: the copper price times the discount in float;
        // the same in double comes out a copper lower now and then, which the bot would have paid itself.)
        auto priceOf = [&](uint32 n) { return uint64(std::floor(float(uint32(proto->BuyPrice) * n) * discount)); };
        uint64 const one = std::max<uint64>(1, priceOf(1));
        // One stack per purchase; the core rounds the discounted price of each purchase down on its own, so what n
        // bundles cost is the sum over those purchases.
        uint32 const perCall = std::max<uint32>(1, std::min<uint32>(255, std::max<uint32>(1, proto->GetMaxStackSize()) / bundle));
        auto planCost = [&](uint32 n)
        {
            uint64 total = 0;
            for (uint32 rest = n; rest;)
            {
                uint32 const chunk = std::min(rest, perCall);
                total += priceOf(chunk);
                rest -= chunk;
            }
            return total;
        };

        // The guild's budgets: per visit and per day.
        Day& day = Today(owner);
        uint64 const left = std::min(visitCap > visitSpent ? visitCap - visitSpent : 0, dayCap > day.spent ? dayCap - day.spent : 0);
        bundles = uint32(std::min<uint64>(bundles, left / one));
        while (bundles && planCost(bundles) > left)
            --bundles;
        if (!bundles)
        {
            note(entry, "budget", Acore::StringFormat("{} would cost {} for one, above what is left of the guild's budget ({} per visit, "
                "{} per day; {} spent today)", proto->Name1, Money(one), Money(visitCap), Money(dayCap), Money(day.spent)));
            continue;
        }
        // The guild bank pays; what it lacks only from the bot's own gold when Shopping.FromPurse says so.
        uint64 const bank = GuildBank::Money(owner);
        uint64 const payable = fromPurse ? bank + player->GetMoney() : bank;
        bundles = uint32(std::min<uint64>(bundles, payable / one));
        while (bundles && planCost(bundles) > payable)
            --bundles;
        if (!bundles)
        {
            note(entry, "bank", Acore::StringFormat("{} costs {} and the guild bank has {}", proto->Name1, Money(one), Money(bank)));
            continue;
        }
        uint64 const cost = planCost(bundles);
        uint64 const fromBank = std::min(cost, bank);
        if (fromBank && !GuildBank::Withdraw(owner, fromBank, Acore::StringFormat("for the shopping list: {} buys {} x {} at {}",
            player->GetName(), bundles * bundle, proto->Name1, vendorName)))
        {
            note(entry, "bank", Acore::StringFormat("the guild bank could not pay {} for {}", Money(fromBank), proto->Name1));
            continue;
        }
        if (fromBank)
        {
            GuildBank::NoTax noTax; // the guild's own gold, handed over for the purchase: no tax on it
            player->ModifyMoney(int32(fromBank));
        }

        // One stack per purchase, straight into the guild storage, so the bags need room for one stack only.
        uint32 const moneyBefore = player->GetMoney();
        uint32 got = 0;
        BuyingScope const buying;
        for (uint32 remaining = bundles; remaining;)
        {
            uint32 const now = std::min(remaining, perCall);
            uint32 const before = player->GetItemCount(entry);
            // (The core's return value only says whether the vendor's list changed, a limited item: a purchase is
            // counted by the bags.)
            player->BuyItemFromVendorSlot(vendor->GetGUID(), slot, entry, uint8(now), NULL_BAG, NULL_SLOT);
            uint32 const after = player->GetItemCount(entry);
            uint32 const gained = after > before ? after - before : 0;
            if (!gained)
                break;
            player->DestroyItemCount(entry, gained, true);
            sStorageService.Add(owner, proto, 0, gained);
            got += gained;
            remaining -= now;
        }
        uint64 const spent = moneyBefore > player->GetMoney() ? moneyBefore - player->GetMoney() : 0;
        if (fromBank > spent)
        {
            // What the guild bank handed over and was not spent goes back.
            uint64 const back = fromBank - spent;
            player->ModifyMoney(-int32(back));
            GuildBank::Deposit(owner, back, Acore::StringFormat("back from {}: the shopping list's purchase cost less", player->GetName()));
        }
        if (!got)
        {
            note(entry, "bags", Acore::StringFormat("{} could not buy {} (no room in its bags, or the vendor refused)", player->GetName(),
                proto->Name1));
            continue;
        }
        sStorageService.Touch(player);
        day.spent += spent;
        visitSpent += spent;
        CharacterDatabase.Execute("REPLACE INTO ga_shopping_spent (owner_account, day, spent) VALUES ({}, {}, {})", owner, day.day, day.spent);
        uint64 const ownPart = spent > fromBank ? spent - fromBank : 0;
        std::string const payer = ownPart ? Acore::StringFormat("{} from the guild bank, {} from its own gold", Money(spent - ownPart),
            Money(ownPart)) : Acore::StringFormat("{} from the guild bank", Money(spent));
        LOG_INFO(ActivityLog, "activity: {} bought {} x {} at {} for the guild's shopping list ({}): the guild storage has {} of {}",
            player->GetName(), got, proto->Name1, vendorName, payer, sStorageService.GetStoredOfEntry(owner, entry), wanted.min);
        sLootService.AddStorageLog(owner, player->GetGUID(), entry, 0, got, LootService::LogBought, spent,
            Acore::StringFormat("bought at {} for the shopping list, {}", vendorName, payer));
        bought += Acore::StringFormat("{}{} x {} ({})", bought.empty() ? "" : ", ", got, proto->Name1, Money(spent));
    }
    if (!bought.empty())
        sGadvProtocol.PushShopping(owner); // today's spending; the storage's counts come with its own deltas
    return bought;
}
