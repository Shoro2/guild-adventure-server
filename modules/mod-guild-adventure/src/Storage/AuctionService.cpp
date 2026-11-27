/*
 * AuctionService implementation. See AuctionService.h and docs/storage.md "Auction house".
 */

#include "AuctionService.h"
#include "AuctionHouseMgr.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "QueryResult.h"
#include "GadvProtocol.h"
#include "GameTime.h"
#include "GuildBank.h"
#include "LootService.h"
#include "GuildAdventureConfig.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildAdventureUtil.h"
#include "Item.h"
#include "BotSettings.h"
#include "Professions.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "StorageService.h"
#include "StringFormat.h"
#include <algorithm>
#include <set>

using namespace GuildAdventure;

namespace
{
    constexpr uint32 MaxStack = 1000;
    constexpr uint32 MaxKeep = 1000000;
    constexpr uint32 MaxBuyout = 0x7FFFFFFF;

    // The AH bot's default quality multipliers (mod-ah-bot-plus, AuctionHouseBot.PriceMultiplier.Quality.*).
    float QualityMultiplier(uint32 quality)
    {
        switch (quality)
        {
            case ITEM_QUALITY_UNCOMMON:  return 1.8f;
            case ITEM_QUALITY_RARE:      return 1.9f;
            case ITEM_QUALITY_EPIC:      return 2.1f;
            case ITEM_QUALITY_LEGENDARY:
            case ITEM_QUALITY_ARTIFACT:  return 3.0f;
            default:                     return 1.0f;
        }
    }
}

AuctionService& AuctionService::Instance()
{
    static AuctionService instance;
    return instance;
}

void AuctionService::Load()
{
    _rules.clear();
    _categoryRules.clear();
    _listings.clear();

    uint32 rules = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT owner_account, item_entry, random_property, keep_amount, stack_size, "
        "price, enabled FROM ga_auction_rule"))
    {
        do
        {
            Field* f = result->Fetch();
            AuctionRule rule;
            uint32 const owner = f[0].Get<uint32>();
            rule.entry = f[1].Get<uint32>();
            rule.randomProperty = f[2].Get<int32>();
            rule.keep = f[3].Get<uint32>();
            rule.stack = f[4].Get<uint32>();
            rule.price = f[5].Get<uint32>();
            rule.enabled = f[6].Get<uint8>() != 0;
            _rules[owner][{ rule.entry, rule.randomProperty }] = rule;
            ++rules;
        } while (result->NextRow());
    }

    if (QueryResult result = CharacterDatabase.Query("SELECT owner_account, category, keep_amount, stack_size, enabled FROM "
        "ga_auction_category_rule"))
    {
        do
        {
            Field* f = result->Fetch();
            AuctionCategoryRule rule;
            uint32 const owner = f[0].Get<uint32>();
            rule.category = f[1].Get<std::string>();
            rule.keep = f[2].Get<uint32>();
            rule.stack = f[3].Get<uint32>();
            rule.enabled = f[4].Get<uint8>() != 0;
            _categoryRules[owner][rule.category] = rule;
            ++rules;
        } while (result->NextRow());
    }

    // Listings whose auction is gone (sold, expired or cancelled while the server was down) are forgotten.
    uint32 kept = 0;
    uint32 dropped = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT auction_id, owner_account, item_entry, random_property, item_count, "
        "created_at FROM ga_auction_listing"))
    {
        do
        {
            Field* f = result->Fetch();
            AuctionListing listing;
            listing.auctionId = f[0].Get<uint32>();
            listing.owner = f[1].Get<uint32>();
            listing.entry = f[2].Get<uint32>();
            listing.randomProperty = f[3].Get<int32>();
            listing.count = f[4].Get<uint32>();
            listing.createdAt = f[5].Get<uint32>();
            bool exists = false;
            for (AuctionHouseId house : { AuctionHouseId::Alliance, AuctionHouseId::Horde, AuctionHouseId::Neutral })
                if (AuctionHouseObject* object = sAuctionMgr->GetAuctionsMapByHouseId(house))
                    if (object->GetAuction(listing.auctionId))
                        exists = true;
            if (exists)
            {
                _listings[listing.auctionId] = listing;
                ++kept;
            }
            else
            {
                CharacterDatabase.Execute("DELETE FROM ga_auction_listing WHERE auction_id = {}", listing.auctionId);
                ++dropped;
            }
        } while (result->NextRow());
    }

    LOG_INFO("module", "mod-guild-adventure: loaded {} auction rules, {} guild auctions in the auction house ({} gone)", rules, kept,
        dropped);
}

void AuctionService::Update(uint32 diff)
{
    for (Item* item : _returned)
        delete item;
    _returned.clear();

    Settings const& settings = GetSettings();
    if (!settings.auctionEnable || !settings.storageEnable)
        return;
    _timer += diff;
    if (_timer < settings.auctionIntervalSeconds * IN_MILLISECONDS)
        return;
    _timer = 0;
    std::set<uint32> owners;
    for (auto const& [owner, rules] : _rules)
        if (!rules.empty())
            owners.insert(owner);
    for (auto const& [owner, rules] : _categoryRules)
        if (!rules.empty())
            owners.insert(owner);
    for (uint32 owner : owners)
        PostSurplus(owner);
}

// ---------------------------------------------------------------------------------------------------------------
// Rules
// ---------------------------------------------------------------------------------------------------------------

char const* AuctionService::RefuseReason(ItemTemplate const* proto)
{
    if (char const* reason = StorageService::RefuseReason(proto))
        return reason;
    if (proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_QUEST_ITEM1)
        return "binds on pickup";
    return nullptr;
}

uint32 AuctionService::DefaultPrice(ItemTemplate const* proto)
{
    Settings const& s = GetSettings();
    uint32 minimum = s.auctionMinPrice;
    if (proto->Class == ITEM_CLASS_TRADE_GOODS)
        minimum = s.auctionMinPriceTradeGoods;
    else if (proto->Class == ITEM_CLASS_PROJECTILE)
        minimum = 5;
    double const base = std::max<double>(proto->SellPrice, minimum);
    double const price = base * QualityMultiplier(proto->Quality) * s.auctionPriceFactor;
    return uint32(std::clamp<double>(price, 1.0, MaxBuyout));
}

std::vector<AuctionRule> AuctionService::GetRules(uint32 owner) const
{
    std::vector<AuctionRule> rules;
    auto itr = _rules.find(owner);
    if (itr != _rules.end())
        for (auto const& [key, rule] : itr->second)
            rules.push_back(rule);
    return rules;
}

void AuctionService::SaveRule(uint32 owner, AuctionRule const& rule) const
{
    CharacterDatabase.Execute("REPLACE INTO ga_auction_rule (owner_account, item_entry, random_property, keep_amount, stack_size, price, "
        "enabled) VALUES ({}, {}, {}, {}, {}, {}, {})", owner, rule.entry, rule.randomProperty, rule.keep, rule.stack, rule.price,
        rule.enabled ? 1 : 0);
}

bool AuctionService::SetRule(uint32 owner, AuctionRule rule, std::string& error)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(rule.entry);
    if (!proto || RefuseReason(proto))
    {
        error = "NOT_AUCTIONABLE";
        return false;
    }
    if (rule.stack > MaxStack || rule.keep > MaxKeep)
    {
        error = "INVALID";
        return false;
    }
    auto& rules = _rules[owner];
    Key const key{ rule.entry, rule.randomProperty };
    if (!rules.count(key) && rules.size() >= GetSettings().auctionMaxRules)
    {
        error = "LIMIT";
        return false;
    }
    if (!rule.stack)
        rule.stack = std::max<uint32>(1, std::min<uint32>(proto->GetMaxStackSize(), 20));
    rule.stack = std::min<uint32>(rule.stack, std::max<uint32>(1, proto->GetMaxStackSize()));
    rules[key] = rule;
    SaveRule(owner, rule);
    return true;
}

bool AuctionService::DeleteRule(uint32 owner, uint32 entry, int32 randomProperty)
{
    auto itr = _rules.find(owner);
    if (itr == _rules.end() || !itr->second.erase({ entry, randomProperty }))
        return false;
    CharacterDatabase.Execute("DELETE FROM ga_auction_rule WHERE owner_account = {} AND item_entry = {} AND random_property = {}", owner,
        entry, randomProperty);
    return true;
}

namespace
{
    // The categories of the Storage tab (GuildAdventure::StorageCategory).
    bool IsStorageCategory(std::string const& category)
    {
        static char const* const known[] = { "cloth", "leather", "metal", "herb", "meat", "elemental", "enchanting", "gems",
            "trade", "recipes", "consumables", "weapons", "armor", "bags", "darkmoon", "other" };
        for (char const* name : known)
            if (category == name)
                return true;
        return false;
    }
}

std::vector<AuctionCategoryRule> AuctionService::GetCategoryRules(uint32 owner) const
{
    std::vector<AuctionCategoryRule> list;
    auto itr = _categoryRules.find(owner);
    if (itr != _categoryRules.end())
        for (auto const& [category, rule] : itr->second)
            list.push_back(rule);
    return list;
}

bool AuctionService::SetCategoryRule(uint32 owner, AuctionCategoryRule rule, std::string& error)
{
    if (!IsStorageCategory(rule.category))
    {
        error = "CATEGORY";
        return false;
    }
    if (rule.stack > MaxStack || rule.keep > MaxKeep)
    {
        error = "INVALID";
        return false;
    }
    auto& rules = _categoryRules[owner];
    if (!rules.count(rule.category) && rules.size() >= 32)
    {
        error = "LIMIT";
        return false;
    }
    rules[rule.category] = rule;
    CharacterDatabase.Execute("REPLACE INTO ga_auction_category_rule (owner_account, category, keep_amount, stack_size, enabled) "
        "VALUES ({}, '{}', {}, {}, {})", owner, rule.category, rule.keep, rule.stack, rule.enabled ? 1 : 0);
    LOG_INFO("module.ga.activity", "activity: auction rule of owner {} for the category {}: keep {} of each kind, the rest in "
        "stacks of {}{}", owner, rule.category, rule.keep, rule.stack ? rule.stack : 20, rule.enabled ? "" : " (off)");
    return true;
}

bool AuctionService::DeleteCategoryRule(uint32 owner, std::string const& category)
{
    auto itr = _categoryRules.find(owner);
    if (itr == _categoryRules.end() || !itr->second.erase(category))
        return false;
    CharacterDatabase.Execute("DELETE FROM ga_auction_category_rule WHERE owner_account = {} AND category = '{}'", owner, category);
    LOG_INFO("module.ga.activity", "activity: auction rule of owner {} for the category {} deleted", owner, category);
    return true;
}

std::optional<AuctionRule> AuctionService::EffectiveRule(uint32 owner, uint32 entry, int32 randomProperty) const
{
    auto own = _rules.find(owner);
    if (own != _rules.end())
    {
        auto itr = own->second.find({ entry, randomProperty });
        if (itr != own->second.end())
            return itr->second;
    }
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    auto cats = _categoryRules.find(owner);
    if (!proto || cats == _categoryRules.end())
        return std::nullopt;
    auto itr = cats->second.find(StorageCategory(uint8(proto->Class), uint8(proto->SubClass), entry));
    if (itr == cats->second.end())
        return std::nullopt;
    AuctionRule rule;
    rule.entry = entry;
    rule.randomProperty = randomProperty;
    rule.keep = itr->second.keep;
    rule.stack = itr->second.stack ? itr->second.stack : std::max<uint32>(1, std::min<uint32>(proto->GetMaxStackSize(), 20));
    rule.stack = std::min<uint32>(rule.stack, std::max<uint32>(1, proto->GetMaxStackSize()));
    rule.price = 0;
    rule.enabled = itr->second.enabled;
    return rule;
}

uint32 AuctionService::GetListedCount(uint32 owner, uint32 entry, int32 randomProperty) const
{
    uint32 count = 0;
    for (auto const& [id, listing] : _listings)
        if (listing.owner == owner && listing.entry == entry && listing.randomProperty == randomProperty)
            count += listing.count;
    return count;
}

std::vector<AuctionListing> AuctionService::GetListings(uint32 owner) const
{
    std::vector<AuctionListing> listings;
    for (auto const& [id, listing] : _listings)
        if (listing.owner == owner)
            listings.push_back(listing);
    return listings;
}

// ---------------------------------------------------------------------------------------------------------------
// Posting
// ---------------------------------------------------------------------------------------------------------------

uint32 AuctionService::PostKind(uint32 owner, AdventureGuild const& guild, AuctionRule const& rule, CharacterDatabaseTransaction trans,
    std::string& summary)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(rule.entry);
    if (!rule.enabled || !proto || RefuseReason(proto))
        return 0;
    AuctionHouseId const houseId = guild.team == TEAM_HORDE ? AuctionHouseId::Horde : AuctionHouseId::Alliance;
    AuctionHouseEntry const* houseEntry = AuctionHouseMgr::GetAuctionHouseEntryFromHouse(houseId);
    AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
    if (!houseEntry || !house)
        return 0;

    Settings const& settings = GetSettings();
    uint32 const stack = std::clamp<uint32>(rule.stack, 1, std::max<uint32>(1, proto->GetMaxStackSize()));
    uint32 const price = rule.price ? rule.price : DefaultPrice(proto);
    uint32 stacks = 0;
    for (auto const& [id, listing] : _listings)
        if (listing.owner == owner && listing.entry == rule.entry && listing.randomProperty == rule.randomProperty)
            ++stacks;

    uint32 posted = 0;
    // What stays: the rule's keep, at least the kind's reserve (a reserved stock is not auctioned away).
    uint32 stored = sStorageService.GetStored(owner, rule.entry, rule.randomProperty);
    uint32 const keep = std::max(rule.keep, sStorageService.GetReserve(owner, rule.entry, rule.randomProperty));
    while (stored >= uint64(keep) + stack && stacks < settings.auctionMaxStacksPerItem)
    {
        if (sStorageService.Remove(owner, rule.entry, rule.randomProperty, stack) < stack)
            break; // changed under us; the next pass tries again
        Item* item = Item::CreateItem(rule.entry, stack);
        if (!item)
        {
            sStorageService.Add(owner, proto, rule.randomProperty, stack);
            break;
        }
        if (rule.randomProperty)
            item->SetItemRandomProperties(rule.randomProperty);
        item->SetOwnerGUID(guild.leader);

        AuctionEntry* auction = new AuctionEntry();
        auction->Id = sObjectMgr->GenerateAuctionID();
        auction->houseId = houseId;
        auction->item_guid = item->GetGUID();
        auction->item_template = item->GetEntry();
        auction->itemCount = item->GetCount();
        auction->owner = guild.leader;
        auction->buyout = uint32(std::min<uint64>(uint64(price) * stack, MaxBuyout));
        auction->startbid = std::max<uint32>(1, uint32(uint64(auction->buyout) * 9 / 10));
        auction->bid = 0;
        auction->bidder = ObjectGuid::Empty;
        auction->deposit = 0;
        auction->expire_time = GameTime::GetGameTime().count() + time_t(settings.auctionHours) * HOUR;
        auction->auctionHouseEntry = houseEntry;

        item->SaveToDB(trans);
        sAuctionMgr->AddAItem(item);
        house->AddAuction(auction);
        auction->SaveToDB(trans);

        AuctionListing listing;
        listing.auctionId = auction->Id;
        listing.owner = owner;
        listing.entry = rule.entry;
        listing.randomProperty = rule.randomProperty;
        listing.count = stack;
        listing.createdAt = Now();
        _listings[listing.auctionId] = listing;
        trans->Append("INSERT INTO ga_auction_listing (auction_id, owner_account, item_entry, random_property, item_count, created_at) "
            "VALUES ({}, {}, {}, {}, {}, {})", listing.auctionId, owner, rule.entry, rule.randomProperty, stack, listing.createdAt);

        stored -= stack;
        ++stacks;
        ++posted;
    }
    if (posted)
    {
        summary += Acore::StringFormat("{}{} x {} {}", summary.empty() ? "" : ", ", posted, stack, proto->Name1);
        sLootService.LogStorageMove(owner, guild.leader, "", rule.entry, rule.randomProperty, posted * stack, false,
            Acore::StringFormat("listed in the auction house, {} x {}", posted, stack));
    }
    return posted;
}

uint32 AuctionService::PostSurplus(uint32 owner)
{
    AdventureGuild const* guild = sGuildAdventureMgr.FindGuild(owner);
    if (!guild || !guild->leader)
        return 0;

    // Every kind that has a rule of its own, then every other kind of the storage whose category has one.
    std::vector<AuctionRule> plan;
    auto own = _rules.find(owner);
    if (own != _rules.end())
        for (auto const& [key, rule] : own->second)
            plan.push_back(rule);
    if (_categoryRules.count(owner))
        for (StorageItem const& item : sStorageService.List(owner))
        {
            if (own != _rules.end() && own->second.count({ item.entry, item.randomProperty }))
                continue;
            if (std::optional<AuctionRule> rule = EffectiveRule(owner, item.entry, item.randomProperty))
                plan.push_back(*rule);
        }
    if (plan.empty())
        return 0;

    uint32 created = 0;
    std::string summary;
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    for (AuctionRule const& rule : plan)
        created += PostKind(owner, *guild, rule, trans, summary);
    CharacterDatabase.CommitTransaction(trans);

    if (created)
    {
        LOG_INFO("module", "mod-guild-adventure: auction house: owner {} posted {} auctions ({})", owner, created, summary);
        LOG_INFO("module.ga.activity", "activity: the guild storage of owner {} posted {} auctions in the auction house: {}", owner,
            created, summary);
        sGadvProtocol.PushNotice(owner, Acore::StringFormat("Guild storage surplus in the auction house: {}.", summary), false);
        sGadvProtocol.PushAuctions(owner);
    }
    return created;
}

uint32 AuctionService::ExpireListings(uint32 owner)
{
    uint32 count = 0;
    time_t const now = GameTime::GetGameTime().count();
    for (auto const& [id, listing] : _listings)
    {
        if (listing.owner != owner)
            continue;
        for (AuctionHouseId house : { AuctionHouseId::Alliance, AuctionHouseId::Horde, AuctionHouseId::Neutral })
            if (AuctionHouseObject* object = sAuctionMgr->GetAuctionsMapByHouseId(house))
                if (AuctionEntry* auction = object->GetAuction(id))
                    if (!auction->bidder)
                    {
                        auction->expire_time = now;
                        ++count;
                    }
    }
    return count;
}

// ---------------------------------------------------------------------------------------------------------------
// Auction hooks
// ---------------------------------------------------------------------------------------------------------------

void AuctionService::OnAuctionRemove(AuctionEntry* entry)
{
    if (!entry)
        return;
    auto itr = _listings.find(entry->Id);
    if (itr == _listings.end())
        return;
    uint32 const owner = itr->second.owner;
    _listings.erase(itr);
    CharacterDatabase.Execute("DELETE FROM ga_auction_listing WHERE auction_id = {}", entry->Id);
    sGadvProtocol.PushAuctions(owner);
}

void AuctionService::OnBeforeExpiredMail(AuctionEntry* entry, bool& sendMail)
{
    if (!entry || !sendMail)
        return;
    auto itr = _listings.find(entry->Id);
    if (itr == _listings.end())
        return;
    Item* item = sAuctionMgr->GetAItem(entry->item_guid);
    ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
    if (!item || !proto)
        return; // the core mails it to the leader as usual
    uint32 const owner = itr->second.owner;
    uint32 const count = item->GetCount();
    int32 const property = item->GetItemRandomPropertyId();
    AdventureGuild const* guild = sGuildAdventureMgr.FindGuild(owner);
    ObjectGuid const leader = guild ? guild->leader : ObjectGuid::Empty;

    // What did not sell is sold at the vendor price, the copper into the guild bank (the owner, 2026-11-26: [quote left out]; 5299 returns had gone back into the
    // storage and up again since 2026-11-12). What the kind's auction rule keeps, at least its reserve, goes back into the
    // storage first: the bots may have taken some out since it was put up. Never a tool, a Darkmoon card or a reputation
    // turn-in (they are not sold anywhere else either), nor what no vendor buys: those go back whole.
    uint32 back = count;
    uint32 keep = 0;
    if (sBotSettings.GetForGuild(owner, Setting::AuctionSellUnsold) && proto->SellPrice && !Professions::IsTool(proto->ItemId) &&
        !StorageService::IsDarkmoonCard(proto->ItemId) && !StorageService::IsReputationTurnIn(proto->ItemId))
    {
        keep = sStorageService.GetReserve(owner, proto->ItemId, property);
        if (std::optional<AuctionRule> const rule = EffectiveRule(owner, proto->ItemId, property))
            keep = std::max(keep, rule->keep);
        uint32 const stored = sStorageService.GetStored(owner, proto->ItemId, property);
        back = stored < keep ? std::min(count, keep - stored) : 0;
    }
    if (back && !sStorageService.Add(owner, proto, property, back))
        return; // the core mails it to the leader as usual
    uint32 sold = count - back;
    uint64 const copper = uint64(proto->SellPrice) * sold;
    std::string const what = Acore::StringFormat("{} x {}", sold, proto->Name1);
    if (sold && !GuildBank::Deposit(owner, copper, Acore::StringFormat("sold {} at the vendor price: it did not sell in the auction house",
            what)))
    {
        // No WoW guild to take the copper: back into the storage as before. (Add refuses a kind or an owner alike, so it
        // fails here only when nothing went back above.)
        if (!sStorageService.Add(owner, proto, property, sold))
            return; // the core mails it to the leader as usual
        back += sold;
        sold = 0;
    }

    // No mail. The core forgets the item next (AuctionHouseMgr::RemoveAItem without deleting it), so its row goes here and
    // the object at the next world tick.
    sendMail = false;
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    item->DeleteFromDB(trans);
    CharacterDatabase.CommitTransaction(trans);
    _returned.push_back(item);
    if (sold)
    {
        LOG_INFO("module.ga.activity", "activity: auction house: {} of owner {} did not sell: sold at the vendor price for {}c into the "
            "guild bank{}", what, owner, copper, back ? Acore::StringFormat(" ({} back into the guild storage: it keeps {} of it)", back,
                keep) : std::string());
        sLootService.AddStorageLog(owner, leader, proto->ItemId, property, sold, uint8(LootRules::ActionSell), uint32(std::min<uint64>(copper,
            UINT32_MAX)), "did not sell in the auction house: sold at the vendor price, into the guild bank");
    }
    if (back)
    {
        LOG_INFO("module", "mod-guild-adventure: auction house: {} x {} did not sell; back in the guild storage of owner {}", back,
            proto->Name1, owner);
        sLootService.LogStorageMove(owner, leader, "", proto->ItemId, property, back, true, sold || keep ?
            "back from the auction house, it did not sell (the kind's rule or reserve keeps it)" :
            "back from the auction house, it did not sell");
    }
}

void AuctionService::OnBeforeSuccessfulMail(AuctionEntry* entry, uint32 profit, bool& sendMail)
{
    if (!entry || !sendMail || !profit)
        return;
    auto itr = _listings.find(entry->Id);
    if (itr == _listings.end())
        return;
    AuctionListing const listing = itr->second;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(listing.entry);
    std::string const what = Acore::StringFormat("{} x {}", listing.count, proto ? proto->Name1 : std::to_string(listing.entry));
    // The storage's items, the guild's gold: into the guild bank (until 2026-11-13 the leader got it by mail, S13).
    if (!GuildBank::Deposit(listing.owner, profit, Acore::StringFormat("sold {} in the auction house", what)))
        return; // no WoW guild: the leader's mail as before
    sendMail = false;
    AdventureGuild const* guild = sGuildAdventureMgr.FindGuild(listing.owner);
    sLootService.AddStorageLog(listing.owner, guild ? guild->leader : ObjectGuid::Empty, listing.entry, listing.randomProperty,
        listing.count, uint8(LootRules::ActionSell), profit, "sold in the auction house, into the guild bank");
}
