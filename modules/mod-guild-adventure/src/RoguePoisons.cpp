/*
 * mod-guild-adventure: the poisons of the guild's rogues. See RoguePoisons.h.
 */

#include "RoguePoisons.h"
#include "BotSettings.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GuildBudget.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StorageService.h"
#include "StringFormat.h"
#include "WorldIndex.h"
#include <algorithm>
#include <cmath>
#include <vector>

using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";

    constexpr uint8 PoisonsFromLevel = 20; // the first poison a vendor sells (Instant Poison, Crippling Poison)

    enum class Kind : uint8
    {
        Instant,
        Deadly,
    };

    // The ranks of each kind, lowest first (item entries of the 3.3.5a poison vendors).
    constexpr uint32 InstantRanks[] = { 6947, 6949, 6950, 8926, 8927, 8928, 21927, 43230, 43231 };
    constexpr uint32 DeadlyRanks[] = { 2892, 2893, 8984, 8985, 20844, 22053, 22054, 43232, 43233 };

    std::vector<uint32> RanksOf(Kind kind)
    {
        if (kind == Kind::Deadly)
            return { std::begin(DeadlyRanks), std::end(DeadlyRanks) };
        return { std::begin(InstantRanks), std::end(InstantRanks) };
    }

    char const* KindName(Kind kind)
    {
        return kind == Kind::Deadly ? "Deadly Poison" : "Instant Poison";
    }

    // A rank the character can use now (its level).
    bool Usable(Player const* player, uint32 entry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        return proto && player->GetLevel() >= proto->RequiredLevel;
    }

    Item* WeaponIn(Player const* player, uint8 slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        return item && item->GetTemplate()->Class == ITEM_CLASS_WEAPON ? item : nullptr;
    }

    // What a kind is wanted for: how many hands carry it.
    struct Need
    {
        Kind kind;
        uint8 hands;
    };

    // The kinds the rogue uses and on how many hands, by its pair setting and its level (Deadly Poison only from the
    // level of its first rank; mod-playerbots puts Instant on the main hand and Deadly on the off hand when it carries
    // both, the one it carries on both hands when it carries one: the pairs below are the ones it plays).
    std::vector<Need> Needs(Player const* player)
    {
        std::vector<Need> needs;
        bool const main = WeaponIn(player, EQUIPMENT_SLOT_MAINHAND) != nullptr;
        bool const off = WeaponIn(player, EQUIPMENT_SLOT_OFFHAND) != nullptr;
        if (!main && !off)
            return needs;
        bool const deadly = Usable(player, DeadlyRanks[0]);
        uint32 const pair = uint32(sBotSettings.Get(player, Setting::PoisonsPair));
        Kind mainKind = pair == 2 && deadly ? Kind::Deadly : Kind::Instant;
        Kind offKind = (pair == 0 || pair == 2) && deadly ? Kind::Deadly : Kind::Instant;
        auto add = [&needs](Kind kind)
        {
            for (Need& need : needs)
                if (need.kind == kind)
                {
                    ++need.hands;
                    return;
                }
            needs.push_back({ kind, 1 });
        };
        if (main)
            add(mainKind);
        if (off)
            add(offKind);
        return needs;
    }

    uint32 StockOf(Player const* player, Need const& need)
    {
        return uint32(std::max<int32>(0, sBotSettings.Get(player, Setting::PoisonsStock))) * need.hands;
    }

    // The doses of a kind it carries, every rank it can use counted.
    uint32 Doses(Player const* player, Kind kind)
    {
        uint32 doses = 0;
        for (uint32 entry : RanksOf(kind))
            if (Usable(player, entry))
                doses += player->GetItemCount(entry);
        return doses;
    }

    // What the guild storage holds of a kind for it (ranks it can use).
    uint32 InStorage(Player const* player, Kind kind)
    {
        uint32 const owner = sStorageService.GetOwnerOf(player);
        if (!owner)
            return 0;
        uint32 amount = 0;
        for (uint32 entry : RanksOf(kind))
            if (Usable(player, entry))
                amount += sStorageService.GetAmount(owner, entry, 0);
        return amount;
    }

    // The temporary enchantment on a weapon, as text for the log ("" = none).
    std::string CoatOn(Player const* player, uint8 slot)
    {
        Item* weapon = WeaponIn(player, slot);
        if (!weapon)
            return "";
        uint32 const id = weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);
        if (!id)
            return Acore::StringFormat("{} without a poison", weapon->GetTemplate()->Name1);
        SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(id);
        return Acore::StringFormat("{} with {} ({} min left)", weapon->GetTemplate()->Name1,
            enchant ? enchant->description[0] : "an enchantment", weapon->GetEnchantmentDuration(TEMP_ENCHANTMENT_SLOT) / MINUTE / IN_MILLISECONDS);
    }
}

bool Poisons::Uses(Player const* player)
{
    if (!player || player->getClass() != CLASS_ROGUE || player->GetLevel() < PoisonsFromLevel ||
        !sBotSettings.IsOn(player, Setting::PoisonsEnable))
        return false;
    return WeaponIn(player, EQUIPMENT_SLOT_MAINHAND) || WeaponIn(player, EQUIPMENT_SLOT_OFFHAND);
}

bool Poisons::WantsTopUp(Player const* player)
{
    if (!Uses(player))
        return false;
    for (Need const& need : Needs(player))
        if (Doses(player, need.kind) * 2 < StockOf(player, need) && !InStorage(player, need.kind))
            return true;
    return false;
}

bool Poisons::Bare(Player const* player)
{
    if (!Uses(player))
        return false;
    for (Need const& need : Needs(player))
        if (Doses(player, need.kind) || InStorage(player, need.kind))
            return false;
    for (uint8 slot : { uint8(EQUIPMENT_SLOT_MAINHAND), uint8(EQUIPMENT_SLOT_OFFHAND) })
        if (Item* weapon = WeaponIn(player, slot); weapon && !weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
            return true;
    return false;
}

bool Poisons::VendorSells(uint32 vendorEntry, Player const* player)
{
    if (!Uses(player))
        return false;
    VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(vendorEntry);
    if (!items)
        return false;
    std::vector<Need> const needs = Needs(player);
    for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
    {
        VendorItem const* item = items->GetItem(slot);
        if (!item || item->ExtendedCost || !Usable(player, item->item))
            continue;
        for (Need const& need : needs)
        {
            std::vector<uint32> const ranks = RanksOf(need.kind);
            if (std::find(ranks.begin(), ranks.end(), item->item) != ranks.end())
                return true;
        }
    }
    return false;
}

uint32 Poisons::TakeFromStorage(Player* player)
{
    if (!Uses(player))
        return 0;
    uint32 const owner = sStorageService.GetOwnerOf(player);
    if (!owner)
        return 0;
    uint32 taken = 0;
    for (Need const& need : Needs(player))
    {
        uint32 have = Doses(player, need.kind);
        uint32 const stock = StockOf(player, need);
        std::vector<uint32> ranks = RanksOf(need.kind);
        // The best rank first.
        for (auto itr = ranks.rbegin(); itr != ranks.rend() && have < stock; ++itr)
        {
            if (!Usable(player, *itr))
                continue;
            uint32 const amount = sStorageService.GetAmount(owner, *itr, 0);
            if (!amount)
                continue;
            std::string error;
            uint32 const got = sStorageService.Withdraw(player, *itr, 0, std::min(stock - have, amount), error);
            if (!got)
            {
                LOG_DEBUG(ActivityLog, "activity: {} could not take its poisons out of the guild storage: {}", player->GetName(), error);
                break; // (bags full: the next look)
            }
            have += got;
            taken += got;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(*itr);
            LOG_INFO(ActivityLog, "activity: {} (level {}) took {} x {} out of the guild storage for its weapons ({} doses of {} "
                "now, it keeps {}; {} left there)", player->GetName(), uint32(player->GetLevel()), got, proto ? proto->Name1 : "?", have,
                KindName(need.kind), stock, sStorageService.GetAmount(owner, *itr, 0));
        }
    }
    return taken;
}

std::string Poisons::BuyAt(Player* player, Creature* vendor)
{
    if (!Uses(player) || !vendor)
        return "";
    VendorItemData const* items = vendor->GetVendorItems();
    if (!items)
        return "";
    std::string bought;
    for (Need const& need : Needs(player))
    {
        uint32 const have = Doses(player, need.kind);
        uint32 const stock = StockOf(player, need);
        // The guild storage first: what it holds comes with the next look into it.
        if (have >= stock || InStorage(player, need.kind))
            continue;

        // The best rank the vendor sells that its level allows.
        std::vector<uint32> const ranks = RanksOf(need.kind);
        ItemTemplate const* proto = nullptr;
        uint32 vendorSlot = 0;
        for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
        {
            VendorItem const* item = items->GetItem(slot);
            if (!item || item->ExtendedCost || !Usable(player, item->item))
                continue;
            auto const rank = std::find(ranks.begin(), ranks.end(), item->item);
            if (rank == ranks.end())
                continue;
            ItemTemplate const* candidate = sObjectMgr->GetItemTemplate(item->item);
            if (!proto || candidate->RequiredLevel > proto->RequiredLevel)
            {
                proto = candidate;
                vendorSlot = slot;
            }
        }
        if (!proto)
            continue;

        uint32 const bundle = std::max<uint32>(1, proto->BuyCount);
        uint32 const bundles = (stock - have + bundle - 1) / bundle;
        uint32 const unit = std::max<uint32>(1, uint32(std::floor(proto->BuyPrice * player->GetReputationPriceDiscount(vendor))));
        uint32 const price = unit * bundles;
        // Its own money first; what it cannot pay the guild's budget may (GuildBudget), else what it can pay.
        uint32 count = std::min<uint32>(bundles, player->GetMoney() / unit);
        GuildBudget::Grant grant;
        bool booked = false;
        if (count < bundles)
        {
            grant = GuildBudget::Request(player, price);
            booked = grant.ok;
            if (grant.ok)
                count = bundles;
        }
        if (!count)
        {
            LOG_INFO(ActivityLog, "activity: {} buys no {} at {}: it cannot pay {} and the guild's budget does not allow it",
                player->GetName(), proto->Name1, vendor->GetName(), GuildBudget::MoneyText(price));
            continue;
        }

        uint32 const before = player->GetItemCount(proto->ItemId);
        uint32 const moneyBefore = player->GetMoney();
        auto buy = [&](uint32 n)
        {
            uint32 const had = player->GetItemCount(proto->ItemId);
            player->BuyItemFromVendorSlot(vendor->GetGUID(), vendorSlot, proto->ItemId, uint8(n), NULL_BAG, NULL_SLOT);
            return player->GetItemCount(proto->ItemId) > had;
        };
        // A stack at a time; when that does not go (bag space), bundle by bundle.
        uint32 const maxStack = std::max<uint32>(1, proto->GetMaxStackSize());
        for (uint32 left = count; left > 0;)
        {
            uint32 const n = std::min<uint32>({ left, maxStack, 255u });
            if (buy(n))
            {
                left -= n;
                continue;
            }
            while (left > 0 && buy(1))
                --left;
            break;
        }
        uint32 const got = player->GetItemCount(proto->ItemId) - before;
        if (booked && !got)
            GuildBudget::Cancel(player, price, grant);
        if (!got)
            continue;
        uint32 const paid = moneyBefore > player->GetMoney() ? moneyBefore - player->GetMoney() : 0;
        bought += (bought.empty() ? "" : ", ") + Acore::StringFormat("{} x {} ({}{}; {} doses of {} now, it keeps {})", got,
            proto->Name1, GuildBudget::MoneyText(paid), booked && grant.fromGuild ? ", the guild bank paid " +
            GuildBudget::MoneyText(grant.fromGuild) : std::string(), Doses(player, need.kind), KindName(need.kind), stock);
    }
    return bought;
}

std::string Poisons::Describe(Player const* player)
{
    if (!player || player->getClass() != CLASS_ROGUE)
        return "not a rogue";
    if (player->GetLevel() < PoisonsFromLevel)
        return Acore::StringFormat("level {}: poisons from level {}", uint32(player->GetLevel()), uint32(PoisonsFromLevel));
    if (!sBotSettings.IsOn(player, Setting::PoisonsEnable))
        return "poisons are off for it (Poisons.Enable)";
    std::string text;
    for (Need const& need : Needs(player))
        text += (text.empty() ? "" : "; ") + Acore::StringFormat("{} on {} hand{}: carries {} of {}, the guild storage holds {}",
            KindName(need.kind), uint32(need.hands), need.hands == 1 ? "" : "s", Doses(player, need.kind), StockOf(player, need),
            InStorage(player, need.kind));
    std::string const main = CoatOn(player, EQUIPMENT_SLOT_MAINHAND);
    std::string const off = CoatOn(player, EQUIPMENT_SLOT_OFFHAND);
    return (text.empty() ? std::string("no weapon in its hands") : text) + (main.empty() ? "" : "; main hand " + main) +
        (off.empty() ? "" : "; off hand " + off);
}

// ---------------------------------------------------------------------------------------------------------------
// .ga admin poisons <character> [take | buy]   (administrator, console, SOAP)
//   how the poison rules judge a rogue in the world; take (test aid): a look into the guild storage now; buy (test
//   aid): a purchase at the poison vendor it stands at.
// ---------------------------------------------------------------------------------------------------------------

namespace
{
    using namespace Acore::ChatCommands;

    class GuildAdventurePoisonsCommandScript : public CommandScript
    {
    public:
        GuildAdventurePoisonsCommandScript() : CommandScript("GuildAdventurePoisonsCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable adminTable =
            {
                { "poisons", HandlePoisons, SEC_ADMINISTRATOR, Console::Yes }
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

        static bool HandlePoisons(ChatHandler* handler, PlayerIdentifier target, Optional<Variant<EXACT_SEQUENCE("take"), EXACT_SEQUENCE("buy")>> what)
        {
            Player* player = target.GetConnectedPlayer();
            if (!player || !player->IsInWorld())
            {
                handler->SendErrorMessage(Acore::StringFormat("{} is not in the world.", target.GetName()), false);
                return false;
            }
            if (what && what->holds_alternative<EXACT_SEQUENCE("take")>())
                handler->SendSysMessage(Acore::StringFormat("took {} out of the guild storage", Poisons::TakeFromStorage(player)));
            else if (what)
            {
                std::string result = "no poison vendor within reach of its hands";
                for (ServicePoint const& service : sWorldIndex.GetServices(ServiceType::Vendor))
                {
                    if (service.spawn.mapId != player->GetMapId() || player->GetDistance2d(service.spawn.x, service.spawn.y) > 12.0f ||
                        !Poisons::VendorSells(service.spawn.entry, player))
                        continue;
                    if (Creature* vendor = player->FindNearestCreature(service.spawn.entry, INTERACTION_DISTANCE))
                    {
                        std::string const bought = Poisons::BuyAt(player, vendor);
                        result = Acore::StringFormat("at {}: {}", vendor->GetName(), bought.empty() ? "nothing bought" : "bought " + bought);
                        break;
                    }
                }
                handler->SendSysMessage(result);
            }
            handler->SendSysMessage(Poisons::Describe(player));
            return true;
        }
    };
}

void AddGuildAdventurePoisonsScripts()
{
    new GuildAdventurePoisonsCommandScript();
}
