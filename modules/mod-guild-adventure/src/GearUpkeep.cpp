/*
 * GearUpkeep implementation. See GearUpkeep.h.
 */

#include "GearUpkeep.h"
#include "BotSettings.h"
#include "Bag.h"
#include "BotDriver.h"
#include "Creature.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildBudget.h"
#include "GuildEvents.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "StringFormat.h"
#include "WorldIndex.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <vector>

using namespace GuildAdventure;

namespace
{
    // At most this many pieces per vendor visit (weapons first).
    constexpr uint32 MaxBuysPerVisit = 3;

    struct Offer
    {
        uint32 slot = 0;
        ItemTemplate const* proto = nullptr;
        uint32 price = 0;
        uint32 group = 0;
        float score = 0.0f;
    };

    // The weapons and armour pieces of a vendor list the bot can wear, pay (budget) and mod-playerbots rates an
    // upgrade. `vendor` (optional): the creature itself, for its prices and its stock.
    std::vector<Offer> Offers(Player* bot, VendorItemData const* items, Creature* vendor, uint32 budget)
    {
        std::vector<Offer> offers;
        if (!items || !budget)
            return offers;
        float const discount = vendor ? bot->GetReputationPriceDiscount(vendor) : 1.0f;
        for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
        {
            VendorItem const* item = items->GetItem(slot);
            ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
            if (!proto || item->ExtendedCost || !proto->BuyPrice ||
                (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR))
                continue;
            uint32 const group = Gear::SlotGroup(proto);
            if (!group || proto->RequiredLevel > bot->GetLevel())
                continue;
            uint32 const price = uint32(std::floor(float(proto->BuyPrice) * discount));
            if (price > budget || bot->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;
            if (vendor && item->maxcount && !vendor->GetVendorItemCurrentCount(item))
                continue;
            if (!BotDriver::IsUpgrade(bot, proto->ItemId, 0))
                continue;
            float const score = BotDriver::ItemScore(bot, proto->ItemId, 0);
            // (Jewellery that only fills a bare place is taken from loot and the storage, not bought.)
            if (BotDriver::IsJewellery(proto) && score <= 0.0f)
                continue;
            offers.push_back({ slot, proto, price, group, score });
        }
        return offers;
    }
}

uint32 Gear::SlotGroup(ItemTemplate const* proto)
{
    switch (proto->InventoryType)
    {
        case INVTYPE_WEAPON:
        case INVTYPE_2HWEAPON:
        case INVTYPE_WEAPONMAINHAND:
        case INVTYPE_WEAPONOFFHAND:
        case INVTYPE_SHIELD:
        case INVTYPE_HOLDABLE:
            return 100;
        case INVTYPE_RANGED:
        case INVTYPE_RANGEDRIGHT:
        case INVTYPE_THROWN:
        case INVTYPE_RELIC:
            return 101;
        case INVTYPE_ROBE:
            return INVTYPE_CHEST;
        case INVTYPE_NON_EQUIP:
        case INVTYPE_BODY:
        case INVTYPE_TABARD:
        case INVTYPE_BAG:
        case INVTYPE_QUIVER:
        case INVTYPE_AMMO:
            return 0;
        default:
            return proto->InventoryType;
    }
}

bool Gear::SellsUpgrade(Player* bot, uint32 vendorEntry)
{
    if (!sBotSettings.IsOn(bot, Setting::GearBuyEnable))
        return false;
    VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(vendorEntry);
    return items && !Offers(bot, items, nullptr, GuildBudget::Available(bot)).empty();
}

std::string Gear::BuyAt(Player* bot, Creature* vendor)
{
    VendorItemData const* items = vendor ? vendor->GetVendorItems() : nullptr;
    if (!sBotSettings.IsOn(bot, Setting::GearBuyEnable) || !items || !bot->IsAlive() || bot->IsInCombat())
        return "";

    // The best upgrade per place on the body; weapons first.
    std::map<uint32, Offer, std::greater<uint32>> best;
    for (Offer const& offer : Offers(bot, items, vendor, GuildBudget::Available(bot)))
    {
        auto [it, isNew] = best.try_emplace(offer.group, offer);
        if (!isNew && offer.score > it->second.score)
            it->second = offer;
    }

    std::string bought;
    uint32 count = 0;
    for (auto const& [group, offer] : best)
    {
        if (count >= MaxBuysPerVisit)
            break;
        GuildBudget::Grant const grant = GuildBudget::Request(bot, offer.price);
        if (!grant.ok)
            continue;
        // (The core's return value only says whether the vendor's list changed: the bags say whether it was bought.)
        uint32 const before = bot->GetItemCount(offer.proto->ItemId, true);
        bot->BuyItemFromVendorSlot(vendor->GetGUID(), offer.slot, offer.proto->ItemId, 1, NULL_BAG, NULL_SLOT);
        if (bot->GetItemCount(offer.proto->ItemId, true) <= before)
        {
            GuildBudget::Cancel(bot, offer.price, grant);
            continue;
        }
        ++count;
        bought += Acore::StringFormat("{}{} ({}{})", bought.empty() ? "" : ", ", offer.proto->Name1, GuildBudget::MoneyText(offer.price),
            grant.fromGuild ? ", the guild bank paid " + GuildBudget::MoneyText(grant.fromGuild) : std::string());
        if (grant.fromGuild)
            if (AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(bot->GetGUID()))
                RecordEvent(*guild, "GEAR", bot->GetGUID(), Acore::StringFormat("{} bought {} from {} for {}, the guild bank paid {}.",
                    bot->GetName(), offer.proto->Name1, vendor->GetName(), GuildBudget::MoneyText(offer.price),
                    GuildBudget::MoneyText(grant.fromGuild)), bot->GetLevel(), bot->GetZoneId());
    }
    if (count)
        BotDriver::EquipUpgrades(bot);
    return bought;
}

std::vector<std::string> Gear::Describe(Player* bot, float yards)
{
    std::vector<std::string> lines;
    uint32 const budget = GuildBudget::Available(bot);
    lines.push_back(Acore::StringFormat("{} (level {}) may spend {} on one piece now: the guild bank pays at most {} of it ({}% of "
        "its {}, {} of the bot's allowance left, {} paid so far), own money {}.", bot->GetName(), uint32(bot->GetLevel()),
        GuildBudget::MoneyText(budget), GuildBudget::MoneyText(GuildBudget::GuildPartAvailable(bot)),
        uint32(sBotSettings.Get(bot, Setting::BudgetItemPercent)),
        GuildBudget::MoneyText(uint32(std::min<uint64>(GuildBudget::BankMoney(bot), 0xFFFFFFFFu))),
        GuildBudget::MoneyText(GuildBudget::AllowanceLeft(bot)), GuildBudget::MoneyText(GuildBudget::SpentBy(bot->GetGUID())),
        GuildBudget::MoneyText(bot->GetMoney())));
    uint32 vendors = 0;
    for (ServicePoint const& service : sWorldIndex.GetServices(ServiceType::Vendor))
    {
        if (service.spawn.mapId != bot->GetMapId() || !WorldIndex::IsFriendlyFaction(service.factionTemplate, bot))
            continue;
        float const distance = bot->GetDistance2d(service.spawn.x, service.spawn.y);
        if (distance > yards)
            continue;
        ++vendors;
        std::string offers;
        for (Offer const& offer : Offers(bot, sObjectMgr->GetNpcVendorItemList(service.spawn.entry), nullptr, budget))
            offers += Acore::StringFormat("{}{} ({})", offers.empty() ? "" : ", ", offer.proto->Name1, GuildBudget::MoneyText(offer.price));
        if (!offers.empty())
            lines.push_back(Acore::StringFormat("{} ({:.0f} yd) sells: {}", service.name, distance, offers));
    }
    lines.push_back(Acore::StringFormat("{} vendor{} within {:.0f} yd, {} of them with an upgrade it would buy.", vendors,
        vendors == 1 ? "" : "s", yards, lines.size() - 1));
    return lines;
}

std::string Gear::BuyHere(Player* bot)
{
    for (ServicePoint const& service : sWorldIndex.GetServices(ServiceType::Vendor))
    {
        if (service.spawn.mapId != bot->GetMapId() || bot->GetDistance2d(service.spawn.x, service.spawn.y) > 12.0f)
            continue;
        Creature* vendor = bot->FindNearestCreature(service.spawn.entry, INTERACTION_DISTANCE);
        if (!vendor || !SellsUpgrade(bot, service.spawn.entry))
            continue;
        std::string const bought = BuyAt(bot, vendor);
        return bought.empty() ? Acore::StringFormat("nothing bought at {} (the budget, or the purchase failed)", vendor->GetName()) :
            Acore::StringFormat("bought at {}: {}", vendor->GetName(), bought);
    }
    return "no vendor with an upgrade within reach of its hands";
}

std::string Gear::Warning(Player const* bot)
{
    uint32 const gap = uint32(sBotSettings.Get(bot, Setting::GearWarnGap));
    uint32 const level = bot->GetLevel();
    // The first levels are played in what a character starts with.
    if (!gap || level < 10)
        return "";

    std::string text;
    auto add = [&text](std::string const& what) { text += (text.empty() ? "" : ", ") + what; };
    auto weapon = [&](uint8 slot, char const* name)
    {
        Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            add(Acore::StringFormat("no {}", name));
        else if (item->GetTemplate()->ItemLevel + gap <= level)
            add(Acore::StringFormat("{} {} (item level {})", name, item->GetTemplate()->Name1, item->GetTemplate()->ItemLevel));
    };
    // The weapon its fights depend on: the main hand of those who strike with it, the bow or gun of a hunter, the wand
    // of a caster (a caster's staff and a hunter's axe only carry stats; a druid fights in its forms).
    switch (bot->getClass())
    {
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
        case CLASS_ROGUE:
        case CLASS_SHAMAN:
        case CLASS_DEATH_KNIGHT:
            weapon(EQUIPMENT_SLOT_MAINHAND, "main hand");
            break;
        case CLASS_HUNTER:
            weapon(EQUIPMENT_SLOT_RANGED, "ranged weapon");
            break;
        case CLASS_MAGE:
        case CLASS_PRIEST:
        case CLASS_WARLOCK:
            weapon(EQUIPMENT_SLOT_RANGED, "wand");
            break;
        default:
            break;
    }

    // The armour on its body (a bare place counts as item level 0).
    static constexpr uint8 bodySlots[] = { EQUIPMENT_SLOT_CHEST, EQUIPMENT_SLOT_LEGS, EQUIPMENT_SLOT_FEET, EQUIPMENT_SLOT_HANDS,
        EQUIPMENT_SLOT_WAIST, EQUIPMENT_SLOT_WRISTS };
    uint32 sum = 0;
    for (uint8 slot : bodySlots)
        if (Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            sum += item->GetTemplate()->ItemLevel;
    uint32 const average = sum / uint32(std::size(bodySlots));
    if (average + gap <= level)
        add(Acore::StringFormat("body armour item level {} on average", average));
    return text;
}

std::string Gear::FillEmptySlots(Player* bot)
{
    if (!bot->IsAlive() || bot->IsInCombat())
        return "";
    // Armour of another kind than the class's only with the setting; jewellery always (mod-playerbots puts neither
    // into a bare place).
    bool const armourToo = sBotSettings.IsOn(bot, Setting::GearFillEmptySlots);

    struct Piece
    {
        Item* item;
        float score;
    };
    std::vector<Piece> pieces;
    auto consider = [&](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto || proto->Class != ITEM_CLASS_ARMOR)
            return;
        if (BotDriver::IsJewellery(proto))
        {
            if (BotDriver::FillsEmptyPlace(bot, proto))
                pieces.push_back({ item, BotDriver::ItemScore(bot, proto->ItemId, item->GetItemRandomPropertyId()) });
            return;
        }
        if (!armourToo)
            return;
        switch (proto->InventoryType)
        {
            case INVTYPE_HEAD:
            case INVTYPE_SHOULDERS:
            case INVTYPE_CHEST:
            case INVTYPE_ROBE:
            case INVTYPE_WAIST:
            case INVTYPE_LEGS:
            case INVTYPE_FEET:
            case INVTYPE_WRISTS:
            case INVTYPE_HANDS:
            case INVTYPE_CLOAK:
                pieces.push_back({ item, 0.0f });
                break;
            default:
                break;
        }
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = bot->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                consider(bag->GetItemByPos(uint8(slot)));
    // The best piece first where two fit the same place: jewellery by what it is worth to the bot, then the item level.
    std::sort(pieces.begin(), pieces.end(), [](Piece const& a, Piece const& b)
    {
        if (a.score != b.score)
            return a.score > b.score;
        return a.item->GetTemplate()->ItemLevel > b.item->GetTemplate()->ItemLevel;
    });

    std::string worn;
    for (Piece const& piece : pieces)
    {
        Item* const item = piece.item;
        // Without a swap: only a place where the bot wears nothing takes it.
        uint16 dest = 0;
        if (bot->CanUseItem(item) != EQUIP_ERR_OK || bot->CanEquipItem(NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
            continue;
        std::string const name = item->GetTemplate()->Name1;
        bot->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
        bot->EquipItem(dest, item, true);
        worn += (worn.empty() ? "" : ", ") + name;
    }
    return worn;
}
