/*
 * mod-guild-adventure: craft orders of a guild (CraftOrders.h, docs/orders.md "Craft orders").
 */

#include "CraftOrders.h"
#include "DungeonRoles.h"
#include "BotDriver.h"
#include "BotSettings.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CraftService.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GadvProtocol.h"
#include "GameTime.h"
#include "GuildAdventureConfig.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildBudget.h"
#include "GuildEvents.h"
#include "Bag.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "LootService.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Professions.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StorageService.h"
#include "StringFormat.h"
#include "Timer.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <set>

using namespace Acore::ChatCommands;
using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";
    // Orders a guild can have open, and the most one order asks for.
    constexpr uint32 MaxOrders = 30;
    constexpr uint32 MaxAmount = 200;
    // The most of one material bought at one vendor visit, and items disenchanted from the storage at one town trip.
    constexpr uint32 MaxBuyPerVisit = 20;
    constexpr uint32 MaxDisenchantPerTrip = 3;

    std::string ItemName(uint32 entry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        return proto ? proto->Name1 : Acore::StringFormat("item {}", entry);
    }

    // What one cast of the recipe makes: the item and how many.
    uint32 CreatedPerCast(SpellInfo const* spell, uint32 item)
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if ((spell->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM || spell->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM_2) &&
                spell->Effects[i].ItemType == item)
                return uint32(std::max<int32>(1, spell->Effects[i].CalcValue()));
        return 1;
    }

    uint32 CreatedItem(SpellInfo const* spell)
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if ((spell->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM || spell->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM_2) &&
                spell->Effects[i].ItemType)
                return spell->Effects[i].ItemType;
        return 0;
    }

    uint32 Have(Player* player, uint32 owner, uint32 entry)
    {
        uint32 have = player->GetItemCount(entry);
        if (owner && GetSettings().storageReagents)
            have += sStorageService.GetAmount(owner, entry, 0);
        return have;
    }

    // The profession a recipe belongs to, for a note ("Enchanting").
    std::string RecipeSkillName(uint32 spellId)
    {
        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
            if (SkillLineEntry const* line = sSkillLineStore.LookupEntry(itr->second->SkillLine))
                return line->name[0];
        return "a profession";
    }

    // Buys up to `count` of an item a vendor sells for copper. Returns how many the character got.
    uint32 BuyFrom(Player* player, Creature* vendor, uint32 itemId, uint32 count)
    {
        VendorItemData const* items = vendor ? vendor->GetVendorItems() : nullptr;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!items || !proto || !count)
            return 0;
        uint32 const before = player->GetItemCount(itemId);
        for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
        {
            VendorItem const* item = items->GetItem(slot);
            if (!item || item->item != itemId || item->ExtendedCost)
                continue;
            uint32 const bundle = std::max<uint32>(1, proto->BuyCount);
            // Counted by the bags: the core answers false for an item of unlimited stock although it sold it, so a loop
            // on its answer stopped after one bundle per visit.
            for (uint32 bundles = (count + bundle - 1) / bundle; bundles && player->GetItemCount(itemId) - before < count; --bundles)
            {
                uint32 const had = player->GetItemCount(itemId);
                player->BuyItemFromVendorSlot(vendor->GetGUID(), slot, itemId, 1, NULL_BAG, NULL_SLOT);
                if (player->GetItemCount(itemId) <= had)
                    break; // no money, no room, no stock
            }
            break;
        }
        return player->GetItemCount(itemId) - before;
    }

    bool VendorSells(Creature* vendor, uint32 itemId)
    {
        VendorItemData const* items = vendor ? vendor->GetVendorItems() : nullptr;
        if (!items)
            return false;
        for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
            if (VendorItem const* item = items->GetItem(slot))
                if (item->item == itemId && !item->ExtendedCost)
                    return true;
        return false;
    }
}

CraftOrders& CraftOrders::Instance()
{
    static CraftOrders instance;
    return instance;
}

void CraftOrders::Load()
{
    _orders.clear();
    _jobs.clear();
    _nextId = 1;
    uint32 count = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT id, owner_account, item_entry, amount, made, created_at FROM ga_craft_order ORDER BY id"))
        do
        {
            Field const* f = result->Fetch();
            CraftOrder order;
            order.id = f[0].Get<uint32>();
            order.item = f[2].Get<uint32>();
            order.amount = f[3].Get<uint32>();
            order.made = f[4].Get<uint32>();
            order.createdAt = f[5].Get<uint32>();
            _orders[f[1].Get<uint32>()].push_back(order);
            _nextId = std::max(_nextId, order.id + 1);
            ++count;
        } while (result->NextRow());
    // What each disenchant table can give, so that an order's missing essence is not looked for in items that only
    // give dust.
    _disenchantGives.clear();
    _disenchantProducts.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT Entry, Item, Chance, GroupId FROM disenchant_loot_template WHERE Reference = 0"))
    {
        // (A chance of 0 is a share of what the others of its group leave: 80 % dust and "0 %" essence is 20 % essence.)
        std::map<std::pair<uint32, uint32>, std::pair<float, std::vector<uint32>>> groups; // (table, group) -> explicit sum, the rest
        do
        {
            Field const* f = result->Fetch();
            uint32 const table = f[0].Get<uint32>();
            uint32 const material = f[1].Get<uint32>();
            float const chance = f[2].Get<float>();
            auto& group = groups[{ table, f[3].Get<uint32>() }];
            if (chance > 0.0f)
            {
                group.first += chance;
                _disenchantGives[table][material] = std::max(_disenchantGives[table][material], chance);
            }
            else
                group.second.push_back(material);
            _disenchantProducts.insert(material);
        } while (result->NextRow());
        for (auto const& [key, group] : groups)
            for (uint32 material : group.second)
            {
                float& chance = _disenchantGives[key.first][material];
                chance = std::max(chance, std::max(1.0f, (100.0f - group.first) / float(group.second.size())));
            }
    }
    LOG_INFO("module", "mod-guild-adventure: craft orders: {} open; {} disenchant tables", count, _disenchantGives.size());
}

bool CraftOrders::DisenchantGives(uint32 item, uint32 material) const
{
    return DisenchantChance(item, material) > 0.0f;
}

float CraftOrders::DisenchantChance(uint32 item, uint32 material) const
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    if (!proto || !proto->DisenchantID)
        return 0.0f;
    auto itr = _disenchantGives.find(proto->DisenchantID);
    if (itr == _disenchantGives.end())
        return 0.0f;
    auto chance = itr->second.find(material);
    return chance != itr->second.end() ? chance->second : 0.0f;
}

void CraftOrders::Save(uint32 owner, CraftOrder const& order) const
{
    CharacterDatabase.Execute("REPLACE INTO ga_craft_order (id, owner_account, item_entry, amount, made, created_at) VALUES ({}, {}, {}, {}, {}, {})",
        order.id, owner, order.item, order.amount, order.made, order.createdAt);
}

void CraftOrders::Erase(uint32 id) const
{
    CharacterDatabase.Execute("DELETE FROM ga_craft_order WHERE id = {}", id);
}

bool CraftOrders::InWork(uint32 orderId) const
{
    return std::any_of(_jobs.begin(), _jobs.end(), [orderId](auto const& job) { return job.second.orderId == orderId; });
}

std::vector<Player*> CraftOrders::Members(AdventureGuild const& guild)
{
    std::vector<Player*> members;
    auto add = [&](ObjectGuid guid)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (player && player->IsInWorld())
            members.push_back(player);
    };
    add(guild.leader);
    for (auto const& [guid, bot] : guild.bots)
        add(guid);
    return members;
}

SpellInfo const* CraftOrders::KnownRecipe(Player const* player, uint32 item)
{
    for (uint32 spellId : sProfessions.RecipesFor(item))
        if (player->HasSpell(spellId))
            if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId))
                if (CraftService::IsCraftRecipe(spell))
                    return spell;
    return nullptr;
}

std::string CraftOrders::MissingMaterials(Player* player, SpellInfo const* spell)
{
    uint32 const owner = sStorageService.GetOwnerOf(player);
    std::string missing;
    for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
    {
        if (spell->Reagent[i] <= 0 || !spell->ReagentCount[i])
            continue;
        uint32 const entry = uint32(spell->Reagent[i]);
        uint32 const have = Have(player, owner, entry);
        if (have < spell->ReagentCount[i])
            missing += Acore::StringFormat("{}{} x {}", missing.empty() ? "" : ", ", spell->ReagentCount[i] - have, ItemName(entry));
    }
    return missing;
}

std::string CraftOrders::MissingTool(Player* player, SpellInfo const* spell, SpellInfo const** toolRecipe)
{
    if (toolRecipe)
        *toolRecipe = nullptr;
    // A named tool.
    for (uint8 i = 0; i < 2; ++i)
        if (spell->Totem[i] && !player->HasItemCount(spell->Totem[i], 1))
        {
            if (toolRecipe)
                *toolRecipe = KnownRecipe(player, spell->Totem[i]);
            return ItemName(spell->Totem[i]);
        }
    // A kind of tool (an enchanter's rod): any item of that kind does.
    for (uint8 i = 0; i < 2; ++i)
    {
        uint32 const category = spell->TotemCategory[i];
        if (!category || player->HasItemTotemCategory(category))
            continue;
        std::string name = "a tool of the trade";
        // A recipe of its own that makes one.
        if (toolRecipe)
            for (auto const& [spellId, state] : player->GetSpellMap())
            {
                if (state->State == PLAYERSPELL_REMOVED)
                    continue;
                SpellInfo const* candidate = sSpellMgr->GetSpellInfo(spellId);
                if (!candidate || !CraftService::IsCraftRecipe(candidate))
                    continue;
                ItemTemplate const* made = sObjectMgr->GetItemTemplate(CreatedItem(candidate));
                if (made && made->TotemCategory && player->IsTotemCategoryCompatiableWith(made, category))
                {
                    *toolRecipe = candidate;
                    name = made->Name1;
                    break;
                }
            }
        return name;
    }
    return std::string();
}

bool CraftOrders::Add(AdventureGuild const& guild, uint32 item, uint32 count, std::string& code, std::string& text)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    if (!proto || sProfessions.RecipesFor(item).empty())
    {
        code = "CRAFT_ORDER_INVALID";
        text = proto ? Acore::StringFormat("No profession recipe makes {}.", proto->Name1) : std::string("Unknown item.");
        return false;
    }
    if (!count || count > MaxAmount)
    {
        code = "CRAFT_ORDER_INVALID";
        text = Acore::StringFormat("Order between 1 and {} at a time.", MaxAmount);
        return false;
    }
    std::vector<CraftOrder>& orders = _orders[guild.ownerAccount];
    // The same item again adds to its order.
    for (CraftOrder& order : orders)
        if (order.item == item)
        {
            order.amount = std::min<uint32>(order.amount + count, order.made + MaxAmount);
            Save(guild.ownerAccount, order);
            text = Acore::StringFormat("{} x {} ordered ({} made so far).", order.amount, proto->Name1, order.made);
            return true;
        }
    if (orders.size() >= MaxOrders)
    {
        code = "CRAFT_ORDER_LIMIT";
        text = Acore::StringFormat("A guild has at most {} craft orders at a time.", MaxOrders);
        return false;
    }
    CraftOrder order;
    order.id = _nextId++;
    order.item = item;
    order.amount = count;
    order.createdAt = uint32(GameTime::GetGameTime().count());
    orders.push_back(order);
    Save(guild.ownerAccount, order);
    text = Acore::StringFormat("{} x {} ordered.", count, proto->Name1);
    return true;
}

bool CraftOrders::HasOpen(uint32 owner, uint32 item) const
{
    auto itr = _orders.find(owner);
    if (itr != _orders.end())
        for (CraftOrder const& order : itr->second)
            if (order.item == item && order.made < order.amount)
                return true;
    return false;
}

bool CraftOrders::Remove(AdventureGuild const& guild, uint32 id, std::string& text)
{
    auto itr = _orders.find(guild.ownerAccount);
    if (itr != _orders.end())
        for (auto order = itr->second.begin(); order != itr->second.end(); ++order)
            if (order->id == id)
            {
                text = Acore::StringFormat("The order of {} x {} is taken back ({} were made).", order->amount, ItemName(order->item), order->made);
                // (A bot that is at it finishes its casts; what it makes stays in its bags like anything it crafts.)
                for (auto job = _jobs.begin(); job != _jobs.end();)
                    job = job->second.orderId == id ? _jobs.erase(job) : std::next(job);
                Erase(id);
                itr->second.erase(order);
                return true;
            }
    text = "No such order.";
    return false;
}

std::vector<CraftOrderView> CraftOrders::Describe(AdventureGuild const& guild) const
{
    std::vector<CraftOrderView> views;
    auto itr = _orders.find(guild.ownerAccount);
    if (itr == _orders.end())
        return views;
    std::vector<Player*> const members = Members(guild);
    for (CraftOrder const& order : itr->second)
    {
        CraftOrderView view;
        view.order = order;
        view.itemName = ItemName(order.item);
        if (InWork(order.id))
        {
            for (auto const& [guid, job] : _jobs)
                if (job.orderId == order.id)
                    if (Player* crafter = ObjectAccessor::FindPlayer(guid))
                        view.note = Acore::StringFormat("{} is making it", crafter->GetName());
        }
        else
        {
            // The first character that knows a recipe says what is missing; one that lacks nothing wins.
            std::string waiting;
            for (Player* member : members)
            {
                SpellInfo const* spell = KnownRecipe(member, order.item);
                if (!spell)
                    continue;
                SpellInfo const* toolRecipe = nullptr;
                std::string const tool = MissingTool(member, spell, &toolRecipe);
                std::string const materials = MissingMaterials(member, spell);
                if (tool.empty() && materials.empty())
                {
                    view.note = Acore::StringFormat("{} makes it on its next town trip", member->GetName());
                    waiting.clear();
                    break;
                }
                if (waiting.empty())
                    waiting = !materials.empty() ?
                        Acore::StringFormat("waits for materials: {} ({} would make it{})", materials, member->GetName(),
                            tool.empty() ? "" : ", and needs " + tool) :
                        Acore::StringFormat("{} needs {} first", member->GetName(), tool);
            }
            if (view.note.empty())
                view.note = !waiting.empty() ? waiting :
                    Acore::StringFormat("nobody of the guild who is in the world knows a recipe for it ({})",
                        RecipeSkillName(sProfessions.RecipesFor(order.item).front()));
        }
        views.push_back(std::move(view));
    }
    return views;
}

std::vector<Craftable> CraftOrders::Craftables(AdventureGuild const& guild) const
{
    std::map<uint32, Craftable> items;
    for (Player* member : Members(guild))
        for (auto const& [spellId, state] : member->GetSpellMap())
        {
            if (state->State == PLAYERSPELL_REMOVED)
                continue;
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
            if (!spell || !CraftService::IsCraftRecipe(spell))
                continue;
            uint32 const item = CreatedItem(spell);
            // Only what a profession makes (not a mage's conjured water).
            std::vector<uint32> const& recipes = sProfessions.RecipesFor(item);
            if (!item || std::find(recipes.begin(), recipes.end(), spellId) == recipes.end())
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            if (!proto)
                continue;
            Craftable& entry = items[item];
            if (!entry.item)
            {
                entry.item = item;
                entry.name = proto->Name1;
                entry.itemLevel = proto->ItemLevel;
                entry.itemClass = proto->Class;
                entry.itemSubClass = proto->SubClass;
            }
            if (entry.crafters.find(member->GetName()) == std::string::npos)
                entry.crafters += (entry.crafters.empty() ? "" : ", ") + member->GetName();
        }
    std::vector<Craftable> list;
    for (auto& [item, entry] : items)
        list.push_back(std::move(entry));
    std::sort(list.begin(), list.end(), [](Craftable const& a, Craftable const& b) { return a.name < b.name; });
    return list;
}

bool CraftOrders::CanDisenchant(uint32 item)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    return proto && proto->DisenchantID && proto->RequiredDisenchantSkill >= 0 &&
        (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR);
}

uint32 CraftOrders::DisenchantSkill(uint32 item)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    return proto && CanDisenchant(item) ? uint32(proto->RequiredDisenchantSkill) : 0;
}

Player* CraftOrders::BestEnchanter(AdventureGuild const& guild, uint32* skill)
{
    Player* best = nullptr;
    for (Player* member : Members(guild))
        if (member->HasSkill(SKILL_ENCHANTING) && (!best || member->GetSkillValue(SKILL_ENCHANTING) > best->GetSkillValue(SKILL_ENCHANTING)))
            best = member;
    if (skill)
        *skill = best ? best->GetSkillValue(SKILL_ENCHANTING) : 0;
    return best;
}

DisenchantResult CraftOrders::DisenchantStored(AdventureGuild const& guild, uint32 item, int32 randomProperty, uint32 count,
    char const* purpose, Player* enchanter)
{
    DisenchantResult result;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    if (!proto || !CanDisenchant(item))
    {
        result.text = "This item cannot be disenchanted.";
        return result;
    }
    uint32 const needed = uint32(proto->RequiredDisenchantSkill);
    if (!enchanter)
        enchanter = BestEnchanter(guild);
    if (!enchanter || !enchanter->HasSkill(SKILL_ENCHANTING) || enchanter->GetSkillValue(SKILL_ENCHANTING) < needed)
    {
        result.text = enchanter ?
            Acore::StringFormat("{} needs Enchanting {} to be disenchanted; the guild's best enchanter in the world, {}, has {}.",
                proto->Name1, needed, enchanter->GetName(), enchanter->GetSkillValue(SKILL_ENCHANTING)) :
            Acore::StringFormat("Nobody of the guild in the world is an enchanter ({} needs Enchanting {}).", proto->Name1, needed);
        return result;
    }
    result.enchanter = enchanter->GetGUID();
    result.enchanterName = enchanter->GetName();

    uint32 const owner = guild.ownerAccount;
    for (; result.done < count; ++result.done)
    {
        if (!sStorageService.Remove(owner, item, randomProperty, 1))
            break;
        // The same table a disenchant in the game rolls on; the materials never touch the enchanter's bags.
        Loot loot;
        loot.FillLoot(proto->DisenchantID, LootTemplates_Disenchant, enchanter, true);
        std::string got;
        for (LootItem const& lootItem : loot.items)
            if (ItemTemplate const* material = sObjectMgr->GetItemTemplate(lootItem.itemid))
            {
                sStorageService.Add(owner, material, 0, lootItem.count);
                result.materials[lootItem.itemid] += lootItem.count;
                got += Acore::StringFormat("{}{} x {}", got.empty() ? "" : ", ", uint32(lootItem.count), material->Name1);
            }
        LOG_INFO(ActivityLog, "activity: {} disenchanted 1 x {} from the guild storage {}: {} into the guild storage",
            enchanter->GetName(), proto->Name1, purpose, got.empty() ? "nothing" : got);
    }
    if (!result.done)
    {
        result.text = Acore::StringFormat("{} is not in the guild storage.", proto->Name1);
        return result;
    }
    result.text = Acore::StringFormat("{} disenchanted {} x {}:", enchanter->GetName(), result.done, proto->Name1);
    for (auto const& [material, amount] : result.materials)
        result.text += Acore::StringFormat(" {} x {}", amount, ItemName(material));
    return result;
}

CraftOrders::Surplus CraftOrders::SurplusOf(std::vector<Player*> const& members, uint32 item, int32 randomProperty, uint32 amount)
{
    Surplus surplus;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    if (!proto || !amount || members.empty() || !CanDisenchant(item) || sProfessions.RecipesFor(item).empty())
        return surplus;
    if (std::none_of(members.begin(), members.end(), [&](Player* member) { return Professions::CanWear(member, proto); }))
    {
        surplus.rank = 2;
        surplus.copies = amount;
        return surplus;
    }
    // One for each character that would put it on (a piece made for somebody waits in the storage for it), one at least.
    uint32 const wearers = uint32(std::count_if(members.begin(), members.end(), [&](Player* member)
    {
        return BotDriver::IsUpgrade(member, item, randomProperty) || sRoleService.IsRoleUpgrade(member, item, randomProperty);
    }));
    uint32 const keep = std::max<uint32>(wearers, 1);
    if (amount > keep)
    {
        surplus.rank = 1;
        surplus.copies = amount - keep;
    }
    return surplus;
}

uint32 CraftOrders::DisenchantSurplus(AdventureGuild const& guild, Player* player)
{
    if (!player->HasSkill(SKILL_ENCHANTING) || !sBotSettings.IsOn(player, Setting::ProfessionsDisenchantSurplus))
        return 0;
    std::vector<Player*> const members = Members(guild);
    uint32 const skill = player->GetSkillValue(SKILL_ENCHANTING);
    uint32 const owner = guild.ownerAccount;
    struct Piece
    {
        StorageItem stored;
        Surplus surplus;
    };
    std::vector<Piece> pieces;
    for (StorageItem const& stored : sStorageService.List(owner))
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(stored.entry);
        // Green and blue (the owner, 2026-11-13), what the enchanter's skill is enough for.
        if (!proto || (proto->Quality != ITEM_QUALITY_UNCOMMON && proto->Quality != ITEM_QUALITY_RARE) || !CanDisenchant(stored.entry) ||
            uint32(proto->RequiredDisenchantSkill) > skill)
            continue;
        Surplus const surplus = SurplusOf(members, stored.entry, stored.randomProperty, stored.amount);
        if (surplus.rank)
            pieces.push_back({ stored, surplus });
    }
    // What nobody can wear first.
    std::stable_sort(pieces.begin(), pieces.end(), [](Piece const& a, Piece const& b) { return a.surplus.rank > b.surplus.rank; });
    uint32 done = 0;
    for (Piece const& piece : pieces)
    {
        if (done >= MaxDisenchantPerTrip)
            break;
        std::string const purpose = piece.surplus.rank == 2 ?
            std::string("as surplus (crafted, nobody of the guild in the world can put it on)") :
            Acore::StringFormat("as surplus (crafted, {} of it lay there)", piece.stored.amount);
        done += DisenchantStored(guild, piece.stored.entry, piece.stored.randomProperty,
            std::min(piece.surplus.copies, MaxDisenchantPerTrip - done), purpose.c_str(), player).done;
    }
    return done;
}

uint32 CraftOrders::DisenchantForMaterials(AdventureGuild const& guild, Player* player, std::vector<uint32> const& wanted, char const* purpose)
{
    if (wanted.empty() || !sBotSettings.IsOn(player, Setting::ProfessionsDisenchantForOrders) || !player->HasSkill(SKILL_ENCHANTING))
        return 0;
    std::vector<Player*> const members = Members(guild);
    uint32 const skill = player->GetSkillValue(SKILL_ENCHANTING);
    uint32 const owner = guild.ownerAccount;
    // Green and blue weapons and armour (the owner, 2026-11-13) the enchanter's skill is enough for.
    std::vector<StorageItem> pieces;
    for (StorageItem const& stored : sStorageService.List(owner))
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(stored.entry);
        if (proto && CanDisenchant(stored.entry) && (proto->Quality == ITEM_QUALITY_UNCOMMON || proto->Quality == ITEM_QUALITY_RARE) &&
            uint32(proto->RequiredDisenchantSkill) <= skill)
            pieces.push_back(stored);
    }
    // What is still missing: a material counts as served once the storage has more of it than when the look began.
    std::map<uint32, uint32> before;
    for (uint32 material : wanted)
        if (material)
            before[material] = sStorageService.GetAmount(owner, material, 0);
    uint32 done = 0;
    while (done < MaxDisenchantPerTrip && !pieces.empty())
    {
        std::vector<uint32> lacking;
        for (auto const& [material, amount] : before)
            if (sStorageService.GetAmount(owner, material, 0) <= amount)
                lacking.push_back(material);
        if (lacking.empty())
            break;
        // The piece most likely to give what is missing (armour for dust, a weapon for an essence), and none that gives
        // it less than about one time in seven. (With "any piece whose table holds it" an enchanter took nine pieces for the
        // dust and the essence of one rod, most of them shields for an essence they give one time in five.)
        // Surplus crafted gear comes first (SurplusOf: what nobody can wear, then spare copies), the likeliest piece
        // within a rank (the owner, 2026-11-13).
        constexpr float MinChance = 15.0f;
        std::size_t best = pieces.size();
        float bestChance = 0.0f;
        uint32 bestRank = 0;
        for (std::size_t i = 0; i < pieces.size(); ++i)
        {
            float chance = 0.0f;
            for (uint32 material : lacking)
                chance = std::max(chance, DisenchantChance(pieces[i].entry, material));
            if (chance < MinChance)
                continue;
            uint32 const rank = SurplusOf(members, pieces[i].entry, pieces[i].randomProperty,
                sStorageService.GetAmount(owner, pieces[i].entry, pieces[i].randomProperty)).rank;
            if (best == pieces.size() || rank > bestRank || (rank == bestRank && chance > bestChance))
            {
                best = i;
                bestChance = chance;
                bestRank = rank;
            }
        }
        if (best == pieces.size())
            break;
        StorageItem const piece = pieces[best];
        pieces.erase(pieces.begin() + best);
        // Not one somebody of the guild in the world would put on (a surplus copy is none: its wearers keep theirs).
        if (!bestRank && std::any_of(members.begin(), members.end(), [&](Player* member)
            {
                return BotDriver::IsUpgrade(member, piece.entry, piece.randomProperty) ||
                    sRoleService.IsRoleUpgrade(member, piece.entry, piece.randomProperty);
            }))
            continue;
        if (DisenchantStored(guild, piece.entry, piece.randomProperty, 1, purpose, player).done)
            ++done;
    }
    return done;
}

bool CraftOrders::DisenchantForOrder(AdventureGuild const& guild, Player* player, SpellInfo const* spell)
{
    // Only when what is missing is something disenchanting gives (dust, essences, shards).
    uint32 const owner = guild.ownerAccount;
    std::vector<uint32> wanted;
    for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        if (spell->Reagent[i] > 0 && spell->ReagentCount[i] && Have(player, owner, uint32(spell->Reagent[i])) < spell->ReagentCount[i])
            if (IsDisenchantProduct(uint32(spell->Reagent[i])))
                wanted.push_back(uint32(spell->Reagent[i]));
    return DisenchantForMaterials(guild, player, wanted, "for a craft order") != 0;
}

bool CraftOrders::Work(AdventureGuild& guild, Player* player, std::string& text)
{
    if (!sBotSettings.IsOn(player, Setting::ProfessionsCraftOrders))
        return false;
    ObjectGuid const guid = player->GetGUID();
    if (_jobs.count(guid) || sCraftService.IsCrafting(guid))
        return false;
    auto itr = _orders.find(guild.ownerAccount);
    if (itr == _orders.end())
        return false;

    uint32 const batch = std::max<uint32>(1, uint32(sBotSettings.Get(player, Setting::ProfessionsCraftBatch)));
    for (CraftOrder const& order : itr->second)
    {
        if (order.made >= order.amount || InWork(order.id))
            continue;
        SpellInfo const* spell = KnownRecipe(player, order.item);
        if (!spell)
            continue;

        // The tool the recipe asks for: made first when the bot knows how and has what that takes.
        SpellInfo const* toolRecipe = nullptr;
        if (std::string const tool = MissingTool(player, spell, &toolRecipe); !tool.empty())
        {
            std::string error;
            if (toolRecipe && (!toolRecipe->RequiresSpellFocus || Professions::FocusNear(player, toolRecipe->RequiresSpellFocus)) &&
                MissingTool(player, toolRecipe, nullptr).empty() && sCraftService.Start(player, toolRecipe->Id, 1, error, text))
            {
                Job job;
                job.item = CreatedItem(toolRecipe);
                job.before = player->GetItemCount(job.item);
                _jobs[guid] = job;
                text = Acore::StringFormat("its tool first, {} (for the order of {})", ItemName(job.item), ItemName(order.item));
                LOG_INFO(ActivityLog, "activity: {} makes {} for itself: the guild's order of {} x {} needs it", player->GetName(),
                    ItemName(job.item), order.amount, ItemName(order.item));
                return true;
            }
            continue;
        }
        if (spell->RequiresSpellFocus && !Professions::FocusNear(player, spell->RequiresSpellFocus))
            continue;
        uint32 craftable = CraftService::CraftableCount(player, spell);
        if (!craftable && DisenchantForOrder(guild, player, spell))
            craftable = CraftService::CraftableCount(player, spell);
        if (!craftable)
            continue;

        uint32 const perCast = CreatedPerCast(spell, order.item);
        uint32 const casts = std::min({ batch, craftable, (order.amount - order.made + perCast - 1) / perCast });
        std::string error;
        if (!sCraftService.Start(player, spell->Id, casts, error, text))
            continue;
        Job job;
        job.orderId = order.id;
        job.item = order.item;
        job.before = player->GetItemCount(order.item);
        _jobs[guid] = job;
        text = Acore::StringFormat("{} x {} ({} of {} made)", casts * perCast, ItemName(order.item), order.made, order.amount);
        LOG_INFO(ActivityLog, "activity: {} works on the guild's craft order: {} x {} ({} of {} made so far)", player->GetName(),
            casts * perCast, ItemName(order.item), order.made, order.amount);
        sGadvProtocol.PushCraftOrders(guild);
        return true;
    }
    return false;
}

void CraftOrders::OnCreated(Player* player, uint32 item, uint32 count)
{
    if (!player || !item || !count)
        return;
    std::lock_guard<std::mutex> guard(_createdLock);
    if (_created.size() < 4096)
        _created.push_back({ player->GetGUID(), item, count, GameTime::GetGameTime().count() });
}

void CraftOrders::Step(AdventureGuild& guild, Player* player)
{
    if (sCraftService.IsCrafting(player->GetGUID()) || player->IsNonMeleeSpellCast(false))
        return;
    uint32 const jobItem = FinishJob(guild, player);
    StoreCrafted(guild, player, jobItem);
    StoreUnworn(guild, player, false);
}

void CraftOrders::StoreCrafted(AdventureGuild& guild, Player* player, uint32 jobItem)
{
    // What this character made since its last step; what was noted for a character that is not planned any more is
    // forgotten after ten minutes.
    std::map<uint32, uint32> made;
    {
        std::lock_guard<std::mutex> guard(_createdLock);
        if (_created.empty())
            return;
        time_t const now = GameTime::GetGameTime().count();
        ObjectGuid const guid = player->GetGUID();
        std::erase_if(_created, [&](Created const& entry)
        {
            if (entry.crafter == guid)
            {
                made[entry.item] += entry.count;
                return true;
            }
            return now - entry.at > 10 * MINUTE;
        });
    }
    if (made.empty())
        return;

    uint32 const owner = guild.ownerAccount;
    std::string stored, kept;
    // Gear it made and would wear goes on first, here and now. What is still in its bags after that goes into the
    // storage like everything else it made. (Each batch used to be kept as "it puts it on": a jeweller made rings in
    // twos and threes for its skill, a bare finger makes any ring an upgrade, of this ring it can wear one, and
    // twelve of them lay in its bags.)
    for (auto const& [entry, count] : made)
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry))
            if ((proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR) && entry != jobItem &&
                !sProfessions.RecipesFor(entry).empty() && BotDriver::IsUpgrade(player, entry, 0))
            {
                BotDriver::EquipUpgrades(player);
                break;
            }
    for (auto const& [entry, count] : made)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        // The job's item is done with (an order's went into the storage, a tool for an order stays).
        if (!proto || entry == jobItem)
            continue;
        // Only what a profession's recipe makes: a spell "creates" a mage's water, a warlock's stones and many a
        // quest item too, and those are not crafted.
        if (sProfessions.RecipesFor(entry).empty() || proto->HasFlag(ITEM_FLAG_CONJURED))
            continue;
        char const* why = nullptr;
        if (proto->TotemCategory || Professions::IsTool(entry) ||
            (proto->Class == ITEM_CLASS_WEAPON && proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE))
            why = "a tool";
        else if (player->HasQuestForItem(entry))
            why = "for its quest";
        if (why)
        {
            kept += Acore::StringFormat("{}{} x {} ({})", kept.empty() ? "" : ", ", count, proto->Name1, why);
            continue;
        }

        // Out of the bags, into the guild storage: the same items, nothing is made or lost on the way. Stack by
        // stack, what the storage does not take (soulbound) stays.
        uint32 left = count;
        auto move = [&](Item* item)
        {
            if (!left || !item || item->GetEntry() != entry || StorageService::RefuseReason(item))
                return;
            uint32 take = std::min<uint32>(left, item->GetCount());
            if (!sStorageService.Add(owner, proto, item->GetItemRandomPropertyId(), take))
                return;
            left -= take;
            player->DestroyItemCount(item, take, true);
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            move(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    move(bag->GetItemByPos(uint8(slot)));
        if (left < count)
            stored += Acore::StringFormat("{}{} x {}", stored.empty() ? "" : ", ", count - left, proto->Name1);
        if (left && player->HasItemOrGemWithIdEquipped(entry, 1))
            kept += Acore::StringFormat("{}{} x {} (it put it on)", kept.empty() ? "" : ", ", left, proto->Name1);
    }
    if (!stored.empty() || !kept.empty())
        LOG_INFO(ActivityLog, "activity: {} put what it made into the guild storage: {}{}{}", player->GetName(),
            stored.empty() ? "nothing" : stored, kept.empty() ? "" : "; it keeps ", kept);
}

void CraftOrders::StoreUnworn(AdventureGuild& guild, Player* player, bool now)
{
    constexpr time_t LookSeconds = MINUTE;
    time_t const time = GameTime::GetGameTime().count();
    time_t& next = _unwornLook[player->GetGUID()];
    if ((!now && time < next) || !player->IsAlive() || player->IsInCombat())
        return;
    next = time + LookSeconds;

    // What it made itself and carries without wearing it: weapons, armour and jewellery of a profession's recipe,
    // and the copies of a tool beyond the first. Not what a quest of its own asks for, not what its dungeon set
    // wants (the role store takes that), not what the storage does not take (soulbound).
    ObjectGuid const self = player->GetGUID();
    auto carried = [&]()
    {
        std::vector<Item*> items;
        std::map<uint32, uint32> tools;
        auto look = [&](Item* item)
        {
            if (!item || item->GetGuidValue(ITEM_FIELD_CREATOR) != self)
                return;
            ItemTemplate const* proto = item->GetTemplate();
            if ((proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR) || proto->HasFlag(ITEM_FLAG_CONJURED) ||
                sProfessions.RecipesFor(proto->ItemId).empty() || player->HasQuestForItem(proto->ItemId) ||
                StorageService::RefuseReason(item) || sRoleService.Wants(player, item))
                return;
            bool const tool = proto->TotemCategory || Professions::IsTool(proto->ItemId) ||
                (proto->Class == ITEM_CLASS_WEAPON && proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE);
            // (Of a tool one stays: the one that is worn, else the first in the bags.)
            if (tool && !player->HasItemOrGemWithIdEquipped(proto->ItemId, 1) && !tools[proto->ItemId]++)
                return;
            items.push_back(item);
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            look(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    look(bag->GetItemByPos(uint8(slot)));
        return items;
    };
    if (carried().empty())
        return;
    // It puts on what it wants of them (the action runs here), the rest goes.
    BotDriver::EquipUpgrades(player);
    std::map<uint32, uint32> moved;
    for (Item* item : carried())
    {
        ItemTemplate const* proto = item->GetTemplate();
        uint32 count = item->GetCount();
        if (!sStorageService.Add(guild.ownerAccount, proto, item->GetItemRandomPropertyId(), count))
            continue;
        moved[proto->ItemId] += count;
        player->DestroyItemCount(item, count, true);
    }
    if (moved.empty())
        return;
    std::string text;
    for (auto const& [entry, count] : moved)
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry))
            text += Acore::StringFormat("{}{} x {}", text.empty() ? "" : ", ", count, proto->Name1);
    LOG_INFO(ActivityLog, "activity: {} put gear it made and does not wear into the guild storage: {}", player->GetName(), text);
}

uint32 CraftOrders::FinishJob(AdventureGuild& guild, Player* player)
{
    auto jobItr = _jobs.find(player->GetGUID());
    if (jobItr == _jobs.end())
        return 0;
    Job const job = jobItr->second;
    _jobs.erase(jobItr);

    uint32 const now = player->GetItemCount(job.item);
    uint32 const made = now > job.before ? now - job.before : 0;
    // (A tool stays with the bot that made it.)
    if (!job.orderId || !made)
        return job.item;
    auto itr = _orders.find(guild.ownerAccount);
    if (itr == _orders.end())
        return job.item;
    for (auto order = itr->second.begin(); order != itr->second.end(); ++order)
    {
        if (order->id != job.orderId)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(job.item);
        if (!proto)
            return job.item;
        // Out of the bags, into the guild storage: the same items, nothing is made or lost on the way.
        uint32 const stored = sStorageService.Add(guild.ownerAccount, proto, 0, made) ? made : 0;
        if (!stored)
        {
            LOG_INFO(ActivityLog, "activity: {} made {} x {} for the guild's craft order; the guild storage does not take it, it stays "
                "in its bags", player->GetName(), made, proto->Name1);
            return job.item;
        }
        player->DestroyItemCount(job.item, made, true);
        order->made += made;
        LOG_INFO(ActivityLog, "activity: {} made {} x {} for the guild's craft order ({} of {}): into the guild storage",
            player->GetName(), made, proto->Name1, std::min(order->made, order->amount), order->amount);
        if (order->made >= order->amount)
        {
            RecordEvent(guild, "CRAFT", player->GetGUID(), Acore::StringFormat("The order of {} x {} is done ({} made the last of it); "
                "it lies in the guild storage.", order->amount, proto->Name1, player->GetName()), player->GetLevel(), player->GetZoneId());
            Erase(order->id);
            itr->second.erase(order);
        }
        else
            Save(guild.ownerAccount, *order);
        sGadvProtocol.PushCraftOrders(guild);
        return job.item;
    }
    return job.item;
}

std::string CraftOrders::BuyAt(AdventureGuild const& guild, Player* player, Creature* vendor)
{
    std::string bought;
    if (!vendor || !sBotSettings.IsOn(player, Setting::ProfessionsCraftOrders))
        return bought;
    auto itr = _orders.find(guild.ownerAccount);
    if (itr == _orders.end())
        return bought;
    uint32 const owner = guild.ownerAccount;

    auto buyFor = [&](SpellInfo const* spell, uint32 casts)
    {
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            if (spell->Reagent[i] <= 0 || !spell->ReagentCount[i])
                continue;
            uint32 const entry = uint32(spell->Reagent[i]);
            uint32 const want = spell->ReagentCount[i] * casts;
            uint32 const have = Have(player, owner, entry);
            if (have >= want || !VendorSells(vendor, entry))
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            uint32 const count = std::min(want - have, MaxBuyPerVisit);
            uint32 const bundle = std::max<uint32>(1, proto->BuyCount);
            uint32 const price = proto->BuyPrice * ((count + bundle - 1) / bundle);
            GuildBudget::Grant const grant = GuildBudget::Request(player, price);
            if (!grant.ok)
                continue;
            uint32 const got = BuyFrom(player, vendor, entry, count);
            if (!got)
            {
                GuildBudget::Cancel(player, price, grant);
                continue;
            }
            bought += Acore::StringFormat("{}{} x {} ({})", bought.empty() ? "" : ", ", got, proto->Name1, GuildBudget::MoneyText(price));
        }
    };

    for (CraftOrder const& order : itr->second)
    {
        if (order.made >= order.amount)
            continue;
        SpellInfo const* spell = KnownRecipe(player, order.item);
        if (!spell)
            continue;
        uint32 const perCast = CreatedPerCast(spell, order.item);
        buyFor(spell, (order.amount - order.made + perCast - 1) / perCast);
        // And for the tool it would have to make first.
        SpellInfo const* toolRecipe = nullptr;
        if (!MissingTool(player, spell, &toolRecipe).empty() && toolRecipe)
            buyFor(toolRecipe, 1);
    }
    return bought;
}

bool CraftOrders::LacksWand(Player const* player)
{
    switch (player->getClass())
    {
        case CLASS_MAGE:
        case CLASS_PRIEST:
        case CLASS_WARLOCK:
            return !player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        default:
            return false;
    }
}

std::string CraftOrders::WandMaker(AdventureGuild const& guild, Player const* caster) const
{
    Craftable const* best = nullptr;
    std::vector<Craftable> const list = Craftables(guild);
    for (Craftable const& entry : list)
    {
        if (entry.itemClass != ITEM_CLASS_WEAPON || entry.itemSubClass != ITEM_SUBCLASS_WEAPON_WAND)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry.item);
        if (!proto || proto->RequiredLevel > caster->GetLevel())
            continue;
        if (!best || entry.itemLevel > best->itemLevel)
            best = &entry;
    }
    return best ? Acore::StringFormat("{} can make {}", best->crafters, best->name) : std::string();
}

std::vector<std::string> CraftOrders::Lines(AdventureGuild const& guild) const
{
    std::vector<std::string> lines;
    std::vector<CraftOrderView> const views = Describe(guild);
    lines.push_back(Acore::StringFormat("Craft orders: {}", views.size()));
    for (CraftOrderView const& view : views)
        lines.push_back(Acore::StringFormat("  #{} {} x {} ({} made): {}", view.order.id, view.order.amount, view.itemName, view.order.made,
            view.note));
    for (Player* member : Members(guild))
        if (LacksWand(member))
        {
            std::string const maker = WandMaker(guild, member);
            lines.push_back(Acore::StringFormat("  {} has no wand: {}", member->GetName(), maker.empty() ?
                std::string("nobody of the guild in the world can make one") : maker));
        }
    return lines;
}

// ---------------------------------------------------------------------------------------------------------------
// Startup and commands
// ---------------------------------------------------------------------------------------------------------------

namespace
{
    class GuildAdventureCraftOrderWorldScript : public WorldScript
    {
    public:
        GuildAdventureCraftOrderWorldScript() : WorldScript("GuildAdventureCraftOrderWorldScript", { WORLDHOOK_ON_STARTUP }) { }

        void OnStartup() override
        {
            if (IsModuleEnabled())
                sCraftOrders.Load();
        }
    };

    // What a character makes with a spell (a recipe): noted for its next planner step (CraftOrders::StoreCrafted).
    // And a guild character the AI plays keeps the tool of its professions: mod-playerbots sold the scribes' inking
    // sets and the jewelcrafters' kits at their vendor visits (white items it has no use for), and the profession plan
    // handed out a new one half an hour later; in between nothing of the profession could be made (on the live server
    // 2026-11-14 until 13:15: 17 inking sets and 10 kits sold).
    class GuildAdventureCraftOrderPlayerScript : public PlayerScript
    {
    public:
        GuildAdventureCraftOrderPlayerScript() : PlayerScript("GuildAdventureCraftOrderPlayerScript",
            { PLAYERHOOK_ON_CREATE_ITEM, PLAYERHOOK_CAN_SELL_ITEM }) { }

        bool OnPlayerCanSellItem(Player* player, Item* item, Creature* /*creature*/) override
        {
            if (!IsModuleEnabled() || !player || !item || !BotDriver::GetAI(player) || !sStorageService.GetOwnerOf(player))
                return true;
            ItemTemplate const* proto = item->GetTemplate();
            // (One stays; a second one may go.)
            if (!sLootService.IsToolOf(player, proto) || player->GetItemCount(proto->ItemId) > item->GetCount())
                return true;
            // A line per character and tool in an hour (the vendor visit asks again each time).
            static std::mutex lock;
            static std::map<std::pair<ObjectGuid, uint32>, uint32> said;
            uint32 const now = std::max<uint32>(getMSTime(), 1);
            bool say = false;
            {
                std::lock_guard<std::mutex> guard(lock);
                uint32& last = said[{ player->GetGUID(), proto->ItemId }];
                if (!last || getMSTimeDiff(last, now) > HOUR * IN_MILLISECONDS)
                {
                    last = now;
                    say = true;
                }
            }
            if (say)
                LOG_INFO(ActivityLog, "activity: {} keeps its {}: the tool of its professions is not sold", player->GetName(), proto->Name1);
            return false;
        }

        void OnPlayerCreateItem(Player* player, Item* item, uint32 count) override
        {
            // Only what a guild character makes while the AI plays it (both checks are safe on a map thread): what
            // the owner makes by hand is his.
            if (IsModuleEnabled() && player && item && BotDriver::GetAI(player) && sStorageService.GetOwnerOf(player))
                sCraftOrders.OnCreated(player, item->GetEntry(), count);
        }
    };

    AdventureGuild* GuildOf(ChatHandler* handler, PlayerIdentifier const& target)
    {
        if (!IsModuleEnabled())
        {
            handler->SendErrorMessage("Guild Adventure is disabled (GuildAdventure.Enable = 0).", false);
            return nullptr;
        }
        AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(target.GetGUID());
        if (!guild)
            handler->SendErrorMessage(Acore::StringFormat("{} is neither the leader nor a bot of an adventure guild.", target.GetName()), false);
        return guild;
    }

    class GuildAdventureCraftOrderCommandScript : public CommandScript
    {
    public:
        GuildAdventureCraftOrderCommandScript() : CommandScript("GuildAdventureCraftOrderCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable orderTable =
            {
                { "add", HandleAdd, SEC_ADMINISTRATOR, Console::Yes },
                { "del", HandleDel, SEC_ADMINISTRATOR, Console::Yes },
                { "work", HandleWork, SEC_ADMINISTRATOR, Console::Yes },
                { "disenchant", HandleDisenchant, SEC_ADMINISTRATOR, Console::Yes }
            };
            static ChatCommandTable adminTable =
            {
                { "craftorder", orderTable }
            };
            static ChatCommandTable gaTable =
            {
                { "craftorders", HandleList, SEC_GAMEMASTER, Console::Yes },
                { "admin", adminTable }
            };
            static ChatCommandTable commandTable =
            {
                { "ga", gaTable }
            };
            return commandTable;
        }

        static bool HandleList(ChatHandler* handler, PlayerIdentifier target)
        {
            AdventureGuild* guild = GuildOf(handler, target);
            if (!guild)
                return false;
            for (std::string const& line : sCraftOrders.Lines(*guild))
                handler->SendSysMessage(line);
            return true;
        }

        static bool HandleAdd(ChatHandler* handler, PlayerIdentifier target, uint32 item, uint32 count)
        {
            AdventureGuild* guild = GuildOf(handler, target);
            if (!guild)
                return false;
            std::string code, text;
            bool const ok = sCraftOrders.Add(*guild, item, count, code, text);
            if (ok)
            {
                handler->SendSysMessage(text);
                sGadvProtocol.PushCraftOrders(*guild);
            }
            else
                handler->SendErrorMessage(text, false);
            return ok;
        }

        static bool HandleDel(ChatHandler* handler, PlayerIdentifier target, uint32 id)
        {
            AdventureGuild* guild = GuildOf(handler, target);
            if (!guild)
                return false;
            std::string text;
            bool const ok = sCraftOrders.Remove(*guild, id, text);
            if (ok)
            {
                handler->SendSysMessage(text);
                sGadvProtocol.PushCraftOrders(*guild);
            }
            else
                handler->SendErrorMessage(text, false);
            return ok;
        }

        // Test aid: the character looks at the guild's craft orders now, as on a town trip.
        static bool HandleWork(ChatHandler* handler, PlayerIdentifier target)
        {
            AdventureGuild* guild = GuildOf(handler, target);
            Player* player = target.GetConnectedPlayer();
            if (!guild || !player || !player->IsInWorld())
            {
                if (guild)
                    handler->SendErrorMessage(Acore::StringFormat("{} is not in the world.", target.GetName()), false);
                return false;
            }
            std::string text;
            if (sCraftOrders.Work(*guild, player, text))
                handler->SendSysMessage(Acore::StringFormat("{} works on a craft order: {}.", player->GetName(), text));
            else
                handler->SendSysMessage(Acore::StringFormat("{} has nothing it can make for a craft order now.", player->GetName()));
            return true;
        }

        // .ga admin craftorder disenchant <character> <item> [count]: an enchanter of that character's guild
        // disenchants items of that kind out of the guild storage (CraftOrders::DisenchantStored, the routine behind
        // disenchanting for an order and the Storage tab's button).
        static bool HandleDisenchant(ChatHandler* handler, PlayerIdentifier target, uint32 item, Optional<uint32> count)
        {
            AdventureGuild* guild = GuildOf(handler, target);
            if (!guild)
                return false;
            handler->SendSysMessage(sCraftOrders.DisenchantStored(*guild, item, 0, std::max<uint32>(1, count.value_or(1)),
                "on an administrator's command").text);
            return true;
        }
    };
}

void AddGuildAdventureCraftOrderScripts()
{
    new GuildAdventureCraftOrderWorldScript();
    new GuildAdventureCraftOrderPlayerScript();
    new GuildAdventureCraftOrderCommandScript();
}
