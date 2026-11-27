/*
 * mod-guild-adventure: the ammunition of the guild's characters (see Ammunition.h).
 */

#include "Ammunition.h"
#include "Bag.h"
#include "BotDriver.h"
#include "BotSettings.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Creature.h"
#include "GuildAdventureConfig.h"
#include "GuildAdventureSettings.h"
#include "GuildBank.h"
#include "GuildBudget.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Professions.h"
#include "ScriptMgr.h"
#include "ShoppingList.h"
#include "StorageService.h"
#include "StorageSupply.h"
#include "StringFormat.h"
#include "WorldIndex.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";
    // The levels below which a hunter stocks less (the owner: [quote left out]).
    constexpr uint8 StarterLevel = 5;
    constexpr uint8 LowLevel = 20;
    constexpr uint32 DefaultStackSize = 1000;

    template <typename Visit>
    void ForEachInBags(Player const* player, Visit&& visit)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                visit(item);
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bag->GetItemByPos(uint8(slot)))
                        visit(item);
    }

    uint32 Damage(ItemTemplate const* proto)
    {
        return uint32((proto->Damage[0].DamageMin + proto->Damage[0].DamageMax) * 100.0f);
    }

    // How much of a kind of ammunition a character took out of the guild storage and still may give back (world
    // thread; forgotten with a restart, after which what it carries counts as its own).
    std::unordered_map<ObjectGuid, std::unordered_map<uint32, uint32>> takenFromStorage;
}

void Ammo::NoteTaken(Player const* player, uint32 entry, uint32 count)
{
    if (player && entry && count)
        takenFromStorage[player->GetGUID()][entry] += count;
}

uint32 Ammo::KindOf(ItemTemplate const* weapon)
{
    if (!weapon || weapon->Class != ITEM_CLASS_WEAPON)
        return 0;
    switch (weapon->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_BOW:
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            return ITEM_SUBCLASS_ARROW;
        case ITEM_SUBCLASS_WEAPON_GUN:
            return ITEM_SUBCLASS_BULLET;
        default:
            return 0;
    }
}

uint32 Ammo::KindOf(Player const* player)
{
    Item const* ranged = player ? player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED) : nullptr;
    return KindOf(ranged ? ranged->GetTemplate() : nullptr);
}

bool Ammo::Better(ItemTemplate const* a, ItemTemplate const* b)
{
    if (!a || !b)
        return a != nullptr;
    if (Damage(a) != Damage(b))
        return Damage(a) > Damage(b);
    if (a->RequiredLevel != b->RequiredLevel)
        return a->RequiredLevel > b->RequiredLevel;
    return a->ItemLevel > b->ItemLevel;
}

bool Ammo::Fits(Player const* player, ItemTemplate const* proto)
{
    if (!player || !proto || proto->Class != ITEM_CLASS_PROJECTILE)
        return false;
    uint32 const kind = KindOf(player);
    return kind && proto->SubClass == kind && player->CanUseItem(proto) == EQUIP_ERR_OK;
}

uint32 Ammo::Stock(Player const* player, ItemTemplate const* proto)
{
    if (!player)
        return 0;
    Settings const settings = sBotSettings.For(player);
    if (player->getClass() == CLASS_HUNTER)
        return player->GetLevel() < StarterLevel ? settings.ammoHunterStockBelow5 :
            player->GetLevel() < LowLevel ? settings.ammoHunterStockBelow20 : settings.ammoHunterStock;
    return settings.ammoOthersStacks * (proto ? std::max<uint32>(1, proto->GetMaxStackSize()) : DefaultStackSize);
}

uint32 Ammo::Count(Player const* player, uint32* best)
{
    if (best)
        *best = 0;
    if (!player || !KindOf(player))
        return 0;
    uint32 count = 0;
    ItemTemplate const* bestProto = nullptr;
    ForEachInBags(player, [&](Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (!Fits(player, proto))
            return;
        count += item->GetCount();
        if (!bestProto || Better(proto, bestProto))
            bestProto = proto;
    });
    if (best && bestProto)
        *best = bestProto->ItemId;
    return count;
}

uint32 Ammo::CountAtLeast(Player const* player, ItemTemplate const* proto)
{
    if (!player || !proto || !KindOf(player))
        return 0;
    uint32 count = 0;
    ForEachInBags(player, [&](Item* item)
    {
        ItemTemplate const* carried = item->GetTemplate();
        if (Fits(player, carried) && !Better(proto, carried))
            count += item->GetCount();
    });
    return count;
}

ItemTemplate const* Ammo::VendorBest(Player const* player)
{
    ItemTemplate const* best = nullptr;
    for (uint32 entry : sProfessions.VendorAmmo())
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry); proto && Fits(player, proto) && (!best || Better(proto, best)))
            best = proto;
    return best;
}

uint32 Ammo::BestInStorage(Player const* player, uint32* amount)
{
    if (amount)
        *amount = 0;
    if (!player || player->getClass() != CLASS_HUNTER)
        return 0;
    uint32 const kind = KindOf(player);
    Settings const settings = sBotSettings.For(player);
    if (!kind || !settings.storageEnable || !settings.storageSupplyProvisions)
        return 0;
    uint32 const owner = sStorageService.GetOwnerOf(player);
    if (!owner)
        return 0;
    // Not worse than what it carries: a level-30 hunter with Sharp Arrows filled its quiver with the storage's Rough
    // Arrows, and the vendor hook then refused the better ones a vendor sold.
    uint32 carried = 0;
    Count(player, &carried);
    ItemTemplate const* atLeast = sObjectMgr->GetItemTemplate(carried);
    // Nor worse than what a vendor sells it: Hunterbm (60) shot the storage's Thorium Shells (17.5 damage) and bought
    // none while the storage held them, where every general goods vendor sells Impact Shot (22) from level 55 (round
    // 171). It buys that at its next vendor visit, and what it took out of the storage goes back there (Trim).
    ItemTemplate const* bought = VendorBest(player);
    if (!atLeast || (bought && Better(bought, atLeast)))
        atLeast = bought;
    ItemTemplate const* best = nullptr;
    for (StorageItem const& item : sStorageService.List(owner))
    {
        if (item.itemClass != ITEM_CLASS_PROJECTILE || item.subClass != kind || !item.amount)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.entry);
        if (!Fits(player, proto) || (atLeast && Better(atLeast, proto)))
            continue;
        if (!best || Better(proto, best))
        {
            best = proto;
            if (amount)
                *amount = item.amount;
        }
    }
    return best ? best->ItemId : 0;
}

bool Ammo::InStorage(Player const* player)
{
    return BestInStorage(player) != 0;
}

bool Ammo::MayBuy(Player const* player, ItemTemplate const* proto, uint32 rounds)
{
    if (!Fits(player, proto))
        return false;
    uint32 const have = Count(player);
    // (Ammunition worse than what it buys does not count against its stock: it is parted with once the better one is
    // in its bags, the worst kind first.)
    if (CountAtLeast(player, proto) + rounds > Stock(player, proto))
        return false;
    // The guild storage first: a hunter buys nothing while it holds ammunition for it (it takes that within a
    // minute), unless it is nearly out where it stands.
    if (have >= sBotSettings.For(player).ammoRestockBelow && InStorage(player))
        return false;
    return true;
}

uint32 Ammo::VendorSells(uint32 vendorEntry, Player const* player)
{
    VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(vendorEntry);
    if (!items || !player)
        return 0;
    ItemTemplate const* best = nullptr;
    for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
    {
        VendorItem const* item = items->GetItem(slot);
        ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
        if (!proto || item->ExtendedCost || !Fits(player, proto))
            continue;
        if (!best || Better(proto, best))
            best = proto;
    }
    return best ? best->ItemId : 0;
}

void Ammo::UseBest(Player* player)
{
    if (!player)
        return;
    uint32 best = 0;
    if (!Count(player, &best) || !best || player->GetUInt32Value(PLAYER_AMMO_ID) == best)
        return;
    // (What is in the slot stays when it is as good: two kinds of the same damage.)
    ItemTemplate const* inSlot = sObjectMgr->GetItemTemplate(player->GetUInt32Value(PLAYER_AMMO_ID));
    if (inSlot && Fits(player, inSlot) && player->HasItemCount(inSlot->ItemId, 1) && !Better(sObjectMgr->GetItemTemplate(best), inSlot))
        return;
    if (player->CanUseAmmo(best) == EQUIP_ERR_OK)
        player->SetAmmo(best);
}

std::string Ammo::BuyAt(Player* player, Creature* vendor)
{
    if (!player || !vendor || player->getClass() != CLASS_HUNTER || !KindOf(player))
        return "";
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(VendorSells(vendor->GetEntry(), player));
    VendorItemData const* items = vendor->GetVendorItems();
    if (!proto || !items)
    {
        UseBest(player);
        return "";
    }
    Settings const settings = sBotSettings.For(player);
    uint32 const have = Count(player);
    uint32 const stock = Stock(player, proto);
    uint32 const bundle = std::max<uint32>(1, proto->BuyCount);
    // What it carries that is worse than this vendor's best does not count: it fills its stock with the better kind,
    // and Trim parts with the worse (what it took out of the guild storage goes back there).
    uint32 const good = CountAtLeast(player, proto);
    uint32 const wanted = good < stock ? (stock - good) / bundle : 0;
    // The guild storage first (StorageSupply brings what it holds for this hunter within a minute).
    if (!wanted || (have >= settings.ammoRestockBelow && InStorage(player)))
    {
        UseBest(player);
        return "";
    }

    uint32 const unit = std::max<uint32>(1, uint32(std::floor(proto->BuyPrice * player->GetReputationPriceDiscount(vendor))));
    uint32 bundles = std::min<uint32>(wanted, player->GetMoney() / unit);
    // Nearly out and without the money for one bundle: the guild's budget pays one, like a quest's item (a recruit
    // owns nothing; a level-2 hunter stood for half an hour with nothing to shoot).
    GuildBudget::Grant grant;
    bool booked = false;
    if (!bundles && have < settings.ammoRestockBelow)
    {
        grant = GuildBudget::Request(player, unit);
        booked = grant.ok;
        bundles = grant.ok ? 1 : 0;
    }
    if (!bundles)
    {
        UseBest(player);
        if (have < settings.ammoRestockBelow)
            LOG_INFO(ActivityLog, "activity: {} gets no ammunition from {}: it cannot pay {} for {} x {} and the guild's budget does not allow it",
                player->GetName(), vendor->GetName(), GuildBudget::MoneyText(unit), bundle, proto->Name1);
        return "";
    }

    uint32 const before = player->GetItemCount(proto->ItemId);
    uint32 const moneyBefore = player->GetMoney();
    for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
    {
        VendorItem const* item = items->GetItem(slot);
        if (!item || item->item != proto->ItemId || item->ExtendedCost)
            continue;
        auto buy = [&](uint32 count)
        {
            uint32 const had = player->GetItemCount(proto->ItemId);
            player->BuyItemFromVendorSlot(vendor->GetGUID(), slot, proto->ItemId, uint8(count), NULL_BAG, NULL_SLOT);
            return player->GetItemCount(proto->ItemId) > had;
        };
        // All at once; when that does not go (bag space), bundle by bundle.
        if (!buy(std::min<uint32>(bundles, 255)))
            for (uint32 i = 0; i < bundles && buy(1); ++i)
            { }
        break;
    }
    uint32 const got = player->GetItemCount(proto->ItemId) - before;
    if (booked && !got)
        GuildBudget::Cancel(player, unit, grant);
    UseBest(player);
    if (!got)
        return "";
    uint32 const paid = moneyBefore > player->GetMoney() ? moneyBefore - player->GetMoney() : 0;
    return Acore::StringFormat("{} x {} for its weapon ({}{}; it carries {} of the {} it stocks)", got, proto->Name1,
        GuildBudget::MoneyText(paid), booked && grant.fromGuild ? ", the guild bank paid " + GuildBudget::MoneyText(grant.fromGuild) : std::string(),
        Count(player), stock);
}

std::string Ammo::Trim(Player* player)
{
    if (!player || !player->IsAlive() || player->IsInCombat())
        return "";
    uint32 const kind = KindOf(player);
    // A hunter without a ranged weapon in its hands is between two weapons: nothing is judged.
    if (!kind && player->getClass() == CLASS_HUNTER)
        return "";

    struct Stack
    {
        Item* item;
        ItemTemplate const* proto;
    };
    std::vector<Stack> fitting, unfit;
    uint32 total = 0;
    ForEachInBags(player, [&](Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->Class != ITEM_CLASS_PROJECTILE)
            return;
        if (Fits(player, proto))
        {
            fitting.push_back({ item, proto });
            total += item->GetCount();
        }
        else
            unfit.push_back({ item, proto });
    });
    if (fitting.empty() && unfit.empty())
        return "";

    struct Sold
    {
        ItemTemplate const* proto;
        uint32 count;
        uint64 money;
        char const* why;
    };
    std::vector<Sold> sold;
    // Back into the guild storage instead (the owner: [quote left out]; the
    // sale is for what was bought): ammunition of a kind a recipe makes, and as much of any kind as this character
    // took out of the storage. A hunter took 1603 Sharp Arrow out of the storage and a gun in the same look, and the
    // arrows were deleted in the same second. Returns how many of the stack went back.
    uint32 const owner = sStorageService.GetOwnerOf(player);
    std::string returned;
    auto giveBack = [&](Stack const& stack, uint32 count) -> uint32
    {
        if (!owner || !count)
            return 0;
        bool const made = !sProfessions.RecipesFor(stack.proto->ItemId).empty();
        uint32 give = count;
        uint32* taken = nullptr;
        if (!made)
        {
            auto byPlayer = takenFromStorage.find(player->GetGUID());
            if (byPlayer == takenFromStorage.end())
                return 0;
            auto byEntry = byPlayer->second.find(stack.proto->ItemId);
            if (byEntry == byPlayer->second.end() || !byEntry->second)
                return 0;
            taken = &byEntry->second;
            give = std::min(count, *taken);
        }
        if (!sStorageService.Add(owner, stack.proto, 0, give))
            return 0;
        if (taken)
            *taken -= give;
        returned += Acore::StringFormat("{}{} x {} ({})", returned.empty() ? "" : ", ", give, stack.proto->Name1,
            made ? "a guild character made it" : "it had taken it out");
        uint32 take = give;
        player->DestroyItemCount(stack.item, take, true);
        return give;
    };
    auto sell = [&](Stack const& stack, uint32 count, char const* why)
    {
        uint64 money = uint64(stack.proto->SellPrice) * count;
        if (uint64(player->GetMoney()) + money > MAX_MONEY_AMOUNT)
            money = 0;
        if (money)
        {
            GuildBank::IncomeSource source("surplus ammunition sold at vendor price");
            player->ModifyMoney(int32(money));
        }
        auto same = std::find_if(sold.begin(), sold.end(), [&](Sold const& entry) { return entry.proto == stack.proto; });
        if (same == sold.end())
            sold.push_back({ stack.proto, count, money, why });
        else
        {
            same->count += count;
            same->money += money;
        }
        uint32 take = count;
        player->DestroyItemCount(stack.item, take, true);
    };

    // What its weapon does not shoot, or it cannot use.
    for (Stack const& stack : unfit)
    {
        uint32 const count = stack.item->GetCount();
        uint32 const back = giveBack(stack, count);
        if (back < count)
            sell(stack, count - back, !kind ? "it has nothing that shoots it" :
                stack.proto->SubClass != kind ? "its weapon does not shoot it" : "it cannot use it");
    }

    // What is over its stock: the worst kind first, of a kind the smallest stacks first.
    ItemTemplate const* bestProto = nullptr;
    for (Stack const& stack : fitting)
        if (!bestProto || Ammo::Better(stack.proto, bestProto))
            bestProto = stack.proto;
    uint32 const stock = Stock(player, bestProto);
    if (total > stock)
    {
        std::sort(fitting.begin(), fitting.end(), [](Stack const& a, Stack const& b)
        {
            if (a.proto != b.proto)
                return Ammo::Better(b.proto, a.proto) || (!Ammo::Better(a.proto, b.proto) && a.proto->ItemId < b.proto->ItemId);
            return a.item->GetCount() < b.item->GetCount();
        });
        uint32 excess = total - stock;
        for (Stack const& stack : fitting)
        {
            if (!excess)
                break;
            uint32 const count = std::min<uint32>(excess, stack.item->GetCount());
            uint32 const back = giveBack(stack, count);
            if (back < count)
                sell(stack, count - back, "over its stock");
            excess -= count;
        }
    }
    if (sold.empty() && returned.empty())
        return "";

    sStorageService.Touch(player);
    UseBest(player);
    if (!returned.empty())
        LOG_INFO(ActivityLog, "activity: {} put ammunition back into the guild storage: {}", player->GetName(), returned);
    if (sold.empty())
        return "";

    std::string text;
    uint64 money = 0;
    for (Sold const& entry : sold)
    {
        text += Acore::StringFormat("{}{} x {} ({}{})", text.empty() ? "" : ", ", entry.count, entry.proto->Name1, entry.why,
            entry.proto->SellPrice ? "" : ", no vendor price: deleted");
        money += entry.money;
    }
    return Acore::StringFormat("{}; {} (it stocks {}, carries {} now)", text,
        GuildBudget::MoneyText(uint32(std::min<uint64>(money, MAX_MONEY_AMOUNT))), stock, Count(player));
}

std::vector<std::string> Ammo::Describe(Player* player)
{
    std::vector<std::string> lines;
    Item const* ranged = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    uint32 const kind = KindOf(player);
    uint32 best = 0;
    uint32 const have = Count(player, &best);
    ItemTemplate const* bestProto = sObjectMgr->GetItemTemplate(best);
    ItemTemplate const* inSlot = sObjectMgr->GetItemTemplate(player->GetUInt32Value(PLAYER_AMMO_ID));
    lines.push_back(Acore::StringFormat("{} (level {}): {} in its ranged slot, shoots {}; carries {} fitting rounds of the {} it stocks{}; ammo slot: {}",
        player->GetName(), uint32(player->GetLevel()), ranged ? ranged->GetTemplate()->Name1 : "nothing",
        kind == ITEM_SUBCLASS_ARROW ? "arrows" : kind == ITEM_SUBCLASS_BULLET ? "bullets" : "nothing", have, Stock(player, bestProto),
        bestProto ? Acore::StringFormat(" (best: {})", bestProto->Name1) : std::string(), inSlot ? inSlot->Name1 : "empty"));
    std::string other;
    ForEachInBags(player, [&](Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (proto && proto->Class == ITEM_CLASS_PROJECTILE && !Fits(player, proto))
            other += Acore::StringFormat("{}{} x {}", other.empty() ? "" : ", ", item->GetCount(), proto->Name1);
    });
    if (!other.empty())
        lines.push_back("ammunition it cannot shoot (sold at vendor price at its next look): " + other);
    uint32 amount = 0;
    if (ItemTemplate const* stored = sObjectMgr->GetItemTemplate(BestInStorage(player, &amount)))
        lines.push_back(Acore::StringFormat("the guild storage holds {} x {} for it: it takes that below half its stock and buys none", amount, stored->Name1));
    else if (player->getClass() == CLASS_HUNTER)
        lines.push_back("the guild storage holds no ammunition it would take");
    Settings const settings = sBotSettings.For(player);
    for (ServicePoint const& service : sWorldIndex.GetServices(ServiceType::Vendor))
    {
        if (service.spawn.mapId != player->GetMapId() || player->GetDistance2d(service.spawn.x, service.spawn.y) > settings.ammoTopUpYards)
            continue;
        if (ItemTemplate const* sold = sObjectMgr->GetItemTemplate(VendorSells(service.spawn.entry, player)))
            lines.push_back(Acore::StringFormat("{} ({:.0f} yd) sells {} ({} for {})", service.name,
                player->GetDistance2d(service.spawn.x, service.spawn.y), sold->Name1, GuildBudget::MoneyText(sold->BuyPrice),
                std::max<uint32>(1, sold->BuyCount)));
    }
    return lines;
}

std::string Ammo::BuyHere(Player* player)
{
    for (ServicePoint const& service : sWorldIndex.GetServices(ServiceType::Vendor))
    {
        if (service.spawn.mapId != player->GetMapId() || player->GetDistance2d(service.spawn.x, service.spawn.y) > 12.0f)
            continue;
        Creature* vendor = player->FindNearestCreature(service.spawn.entry, INTERACTION_DISTANCE);
        if (!vendor || !VendorSells(service.spawn.entry, player))
            continue;
        uint32 const before = Count(player);
        std::string const bought = BuyAt(player, vendor);
        uint32 const after = Count(player);
        // mod-playerbots' own purchase, as on a vendor visit: the hook holds it to the stock.
        BotDriver::BuyFromVendor(player);
        return Acore::StringFormat("at {}: {}; {} -> {} rounds, {} after mod-playerbots' own purchase (stock {})", vendor->GetName(),
            bought.empty() ? "nothing bought by the module" : "bought " + bought, before, after, Count(player), Stock(player));
    }
    return "no vendor that sells its ammunition within reach of its hands";
}

// ---------------------------------------------------------------------------------------------------------------
// The vendor hook: no purchase of ammunition beyond the rules for a character the AI plays
// ---------------------------------------------------------------------------------------------------------------

namespace
{
    using namespace Acore::ChatCommands;

    // .ga admin ammo <character> [buy | trim]   (administrator, console, SOAP)
    //   how the ammunition rules judge a character in the world; buy (test aid): a purchase at the vendor it stands
    //   at, the module's and then mod-playerbots' own; trim (test aid): the sale of what it must not carry, now.
    class GuildAdventureAmmoCommandScript : public CommandScript
    {
    public:
        GuildAdventureAmmoCommandScript() : CommandScript("GuildAdventureAmmoCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable adminTable =
            {
                { "ammo", HandleAmmo, SEC_ADMINISTRATOR, Console::Yes }
            };
            static ChatCommandTable gaTable =
            {
                { "admin", adminTable }
            };
            static ChatCommandTable commandTable =
            {
                { "ga", gaTable }
            };
            return commandTable;
        }

        static bool HandleAmmo(ChatHandler* handler, PlayerIdentifier target, Optional<Variant<EXACT_SEQUENCE("buy"), EXACT_SEQUENCE("trim")>> what)
        {
            Player* player = target.GetConnectedPlayer();
            if (!player || !player->IsInWorld())
            {
                handler->SendErrorMessage(Acore::StringFormat("{} is not in the world.", target.GetName()), false);
                return false;
            }
            if (what && what->holds_alternative<EXACT_SEQUENCE("buy")>())
                handler->SendSysMessage(Ammo::BuyHere(player));
            else if (what)
            {
                std::string const sold = Ammo::Trim(player);
                handler->SendSysMessage(sold.empty() ? "nothing to sell" : "sold at vendor price: " + sold);
            }
            for (std::string const& line : Ammo::Describe(player))
                handler->SendSysMessage(line);
            return true;
        }
    };

    class GuildAdventureAmmoPlayerScript : public PlayerScript
    {
    public:
        GuildAdventureAmmoPlayerScript() : PlayerScript("GuildAdventureAmmoPlayerScript", { PLAYERHOOK_ON_BEFORE_BUY_ITEM_FROM_VENDOR }) { }

        // mod-playerbots buys ammunition by its own count (eight stacks for a hunter, two for anyone else with a bow
        // or gun) at every vendor it visits. The core takes "item = 0" as "the script handled it": nothing is bought.
        // Only characters the AI plays (both checks are safe on a map thread): what the owner buys by hand is his.
        void OnPlayerBeforeBuyItemFromVendor(Player* player, ObjectGuid /*vendorguid*/, uint32 /*vendorslot*/, uint32& item,
            uint8 count, uint8 /*bag*/, uint8 /*slot*/) override
        {
            if (!IsModuleEnabled() || !player || !item)
                return;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            if (!proto)
                return;
            // Food and drink: mod-playerbots buys a stack of one of them at every vendor. Not what a mage conjures
            // itself, and not while the guild storage has that kind for the bot (StorageSupply.h).
            constexpr uint32 FoodSpellCategory = 11;
            constexpr uint32 DrinkSpellCategory = 59;
            if (proto->Class == ITEM_CLASS_CONSUMABLE &&
                (proto->Spells[0].SpellCategory == FoodSpellCategory || proto->Spells[0].SpellCategory == DrinkSpellCategory))
            {
                // (A purchase for the guild storage's shopping list buys for the storage, not for the bot.)
                if (!BotDriver::GetAI(player) || !sStorageService.GetOwnerOf(player) || ShoppingList::IsBuying())
                    return;
                StorageSupply::Kind const kind = proto->Spells[0].SpellCategory == DrinkSpellCategory ? StorageSupply::Kind::Drink :
                    StorageSupply::Kind::Food;
                bool const conjures = StorageSupply::Conjures(player, kind);
                if (conjures || StorageSupply::Has(player, kind))
                {
                    LOG_DEBUG(ActivityLog, "activity: {} does not buy {}: {}", player->GetName(), proto->Name1,
                        conjures ? "it conjures its own" : "the guild storage has some for it");
                    item = 0;
                }
                return;
            }
            if (proto->Class != ITEM_CLASS_PROJECTILE)
                return;
            if (!BotDriver::GetAI(player) || !sStorageService.GetOwnerOf(player))
                return;
            uint32 const rounds = std::max<uint32>(1, proto->BuyCount) * std::max<uint32>(1, count);
            if (!Ammo::MayBuy(player, proto, rounds))
            {
                LOG_DEBUG(ActivityLog, "activity: {} does not buy {} x {}: it carries {} of the {} it stocks", player->GetName(),
                    rounds, proto->Name1, Ammo::Count(player), Ammo::Stock(player, proto));
                item = 0;
            }
        }
    };
}

void AddGuildAdventureAmmoScripts()
{
    new GuildAdventureAmmoPlayerScript();
    new GuildAdventureAmmoCommandScript();
}
