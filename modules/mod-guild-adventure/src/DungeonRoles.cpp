/*
 * DungeonRoles implementation. See DungeonRoles.h.
 */

#include "DungeonRoles.h"
#include "DungeonSupplies.h"
#include "SpellInfo.h"
#include "Bag.h"
#include "BotDriver.h"
#include "BotSettings.h"
#include "Chat.h"
#include "CommandScript.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GuildAdventureConfig.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildAdventureUtil.h"
#include "GuildBudget.h"
#include "GuildBotHolder.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Optional.h"
#include "OrderService.h"
#include "CharacterCache.h"
#include "Player.h"
#include "QuestDef.h"
#include "Readiness.h"
#include "ScriptMgr.h"
#include "SpellMgr.h"
#include "SpecService.h"
#include "StorageService.h"
#include "LootService.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Tokenize.h"
#include <algorithm>
#include <map>
#include <sstream>
#include <tuple>

using namespace Acore::ChatCommands;
using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";

    constexpr uint8 NoSlot = 255;
    // A look at the bags about once a minute; at once when the loot filter kept something for the role.
    constexpr uint32 StepMs = MINUTE * IN_MILLISECONDS;
    // A piece is an upgrade of the dungeon set when its worth for the role is clearly above what the set has there.
    constexpr float UpgradeFactor = 1.05f;
    constexpr float UpgradeMargin = 0.5f;
    // Pieces a bot takes out of the guild storage for its dungeon set per look.
    constexpr uint32 MaxStorageTakes = 2;
    // How long a computed gear level of the dungeon set is good for.
    constexpr uint32 GearLevelCacheMs = 15 * IN_MILLISECONDS;

    // Places of a dungeon set. A set has one piece per place (two rings); the hands are three places: a one-hand
    // weapon, a two-hand weapon and what the other hand holds (the set wears the better of "two-hand" and
    // "one-hand plus other hand").
    enum SetPlace : uint8
    {
        PlaceNone = 0,
        PlaceHead, PlaceNeck, PlaceShoulders, PlaceBack, PlaceChest, PlaceWrists, PlaceHands, PlaceWaist, PlaceLegs, PlaceFeet,
        PlaceFinger, PlaceMainHand, PlaceTwoHand, PlaceOffHand, PlaceRanged
    };

    constexpr SetPlace BodyPlaces[] = { PlaceHead, PlaceNeck, PlaceShoulders, PlaceBack, PlaceChest, PlaceWrists, PlaceHands, PlaceWaist,
        PlaceLegs, PlaceFeet, PlaceRanged };
    constexpr SetPlace AllPlaces[] = { PlaceHead, PlaceNeck, PlaceShoulders, PlaceBack, PlaceChest, PlaceWrists, PlaceHands, PlaceWaist,
        PlaceLegs, PlaceFeet, PlaceFinger, PlaceMainHand, PlaceTwoHand, PlaceOffHand, PlaceRanged };

    SetPlace PlaceOf(ItemTemplate const* proto)
    {
        if (!proto || (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR))
            return PlaceNone;
        switch (proto->InventoryType)
        {
            case INVTYPE_HEAD: return PlaceHead;
            case INVTYPE_NECK: return PlaceNeck;
            case INVTYPE_SHOULDERS: return PlaceShoulders;
            case INVTYPE_CLOAK: return PlaceBack;
            case INVTYPE_CHEST:
            case INVTYPE_ROBE: return PlaceChest;
            case INVTYPE_WRISTS: return PlaceWrists;
            case INVTYPE_HANDS: return PlaceHands;
            case INVTYPE_WAIST: return PlaceWaist;
            case INVTYPE_LEGS: return PlaceLegs;
            case INVTYPE_FEET: return PlaceFeet;
            case INVTYPE_FINGER: return PlaceFinger;
            case INVTYPE_WEAPON:
            case INVTYPE_WEAPONMAINHAND: return PlaceMainHand;
            case INVTYPE_2HWEAPON: return PlaceTwoHand;
            case INVTYPE_SHIELD:
            case INVTYPE_HOLDABLE: return PlaceOffHand;
            case INVTYPE_RANGED:
            case INVTYPE_RANGEDRIGHT:
            case INVTYPE_RELIC: return PlaceRanged;
            default: return PlaceNone;
        }
    }

    std::vector<uint8> SlotsOf(SetPlace group)
    {
        switch (group)
        {
            case PlaceHead: return { EQUIPMENT_SLOT_HEAD };
            case PlaceNeck: return { EQUIPMENT_SLOT_NECK };
            case PlaceShoulders: return { EQUIPMENT_SLOT_SHOULDERS };
            case PlaceBack: return { EQUIPMENT_SLOT_BACK };
            case PlaceChest: return { EQUIPMENT_SLOT_CHEST };
            case PlaceWrists: return { EQUIPMENT_SLOT_WRISTS };
            case PlaceHands: return { EQUIPMENT_SLOT_HANDS };
            case PlaceWaist: return { EQUIPMENT_SLOT_WAIST };
            case PlaceLegs: return { EQUIPMENT_SLOT_LEGS };
            case PlaceFeet: return { EQUIPMENT_SLOT_FEET };
            case PlaceFinger: return { EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2 };
            case PlaceMainHand:
            case PlaceTwoHand: return { EQUIPMENT_SLOT_MAINHAND };
            case PlaceOffHand: return { EQUIPMENT_SLOT_OFFHAND };
            case PlaceRanged: return { EQUIPMENT_SLOT_RANGED };
            default: return {};
        }
    }

    std::size_t Capacity(SetPlace group)
    {
        return group == PlaceFinger ? 2 : 1;
    }

    char const* SlotName(uint8 slot)
    {
        static char const* const names[EQUIPMENT_SLOT_END] = { "head", "neck", "shoulders", "shirt", "chest", "waist", "legs", "feet",
            "wrists", "hands", "finger", "finger", "trinket", "trinket", "back", "main hand", "off hand", "ranged", "tabard" };
        return slot < EQUIPMENT_SLOT_END ? names[slot] : "";
    }

    // What a role holds in its hands: a warrior or paladin tank a one-hand weapon and a shield, a death knight tank
    // a two-hand weapon; nobody collects thrown weapons.
    bool WantedBy(uint8 classId, DungeonRole role, ItemTemplate const* proto, SetPlace group)
    {
        if (group == PlaceNone)
            return false;
        if (role == DungeonRole::Tank)
        {
            if (classId == CLASS_WARRIOR || classId == CLASS_PALADIN)
            {
                if (group == PlaceTwoHand || (group == PlaceOffHand && proto->InventoryType != INVTYPE_SHIELD))
                    return false;
            }
            else if (classId == CLASS_DEATH_KNIGHT && (group == PlaceMainHand || group == PlaceOffHand))
                return false;
        }
        return true;
    }

    // True if the character can put that kind of item on now. (Player::CanUseItem for a template checks class, race
    // and level, not the proficiency: a priest "could use" mail by it.)
    bool CanWear(Player const* player, ItemTemplate const* proto)
    {
        if (!proto || player->CanUseItem(proto) != EQUIP_ERR_OK)
            return false;
        uint32 const skill = proto->GetSkill();
        return !skill || player->GetSkillValue(skill) > 0;
    }

    float ScoreOf(Player const* player, DungeonRole role, uint32 entry, int32 randomProperty)
    {
        return BotDriver::RoleScore(player, uint8(role), entry, randomProperty);
    }

    // The place a worn piece fills in a set (a second weapon in the off hand fills none).
    SetPlace PlaceOfWorn(Item const* item, uint8 slot)
    {
        SetPlace const group = item ? PlaceOf(item->GetTemplate()) : PlaceNone;
        if (slot == EQUIPMENT_SLOT_OFFHAND && group != PlaceOffHand)
            return PlaceNone;
        return group;
    }

    float WornScore(Player const* player, DungeonRole role, uint8 slot)
    {
        Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        return item ? ScoreOf(player, role, item->GetEntry(), item->GetItemRandomPropertyId()) : 0.0f;
    }

    // What a set has for a place, best first: the pieces of the dungeon set in the store and what the bot wears there.
    struct Standing
    {
        float score = 0.0f;
        int32 index = -1;   // in the store; -1: worn
        uint8 slot = NoSlot;
    };

    // A piece a character wears only once (unique-equipped, or carried at most once): a second one of it is no ring
    // for the other finger (13 of the owner's bots took a second jewelcrafted ring for it, 2026-11-14).
    bool OnceOnly(ItemTemplate const* proto)
    {
        return proto && (proto->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE) || proto->MaxCount == 1);
    }

    std::vector<Standing> StandingOf(Player const* player, BotRole const& state, SetPlace group)
    {
        std::vector<Standing> list;
        for (uint8 slot : SlotsOf(group))
            if (Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); item && PlaceOfWorn(item, slot) == group)
                list.push_back({ ScoreOf(player, state.role, item->GetEntry(), item->GetItemRandomPropertyId()), -1, slot });
        for (std::size_t i = 0; i < state.store.size(); ++i)
        {
            RolePiece const& piece = state.store[i];
            if (piece.kind == RolePieceDungeon && PlaceOf(sObjectMgr->GetItemTemplate(piece.entry)) == group)
                list.push_back({ ScoreOf(player, state.role, piece.entry, piece.randomProperty), int32(i), NoSlot });
        }
        std::stable_sort(list.begin(), list.end(), [](Standing const& a, Standing const& b)
        {
            if (a.score != b.score)
                return a.score > b.score;
            return a.index < b.index; // what is worn before what is stored
        });
        // Of a piece it wears only once the best copy: a second one takes no place of the set (two copies of a unique
        // ring do not fill both fingers).
        std::vector<uint32> seen;
        std::erase_if(list, [&](Standing const& stand)
        {
            Item const* worn = stand.index < 0 ? player->GetItemByPos(INVENTORY_SLOT_BAG_0, stand.slot) : nullptr;
            uint32 const entry = stand.index < 0 ? (worn ? worn->GetEntry() : 0) : state.store[stand.index].entry;
            if (!entry || !OnceOnly(sObjectMgr->GetItemTemplate(entry)))
                return false;
            if (std::find(seen.begin(), seen.end(), entry) != seen.end())
                return true;
            seen.push_back(entry);
            return false;
        });
        return list;
    }

    // What the bot would wear in a dungeon: for each equipment slot that changes, the piece of the store (its id).
    struct SetPlan
    {
        std::map<uint8, uint32> put;
        bool clearOffHand = false;
    };

    SetPlan PlanDungeonSet(Player const* player, BotRole const& state)
    {
        SetPlan plan;
        if (state.role == DungeonRole::None)
            return plan;
        uint8 const classId = player->getClass();

        // The best piece of the store for a place that the bot can wear now (-1: none).
        auto bestStored = [&](SetPlace group, float& score)
        {
            int32 best = -1;
            score = 0.0f;
            for (std::size_t i = 0; i < state.store.size(); ++i)
            {
                RolePiece const& piece = state.store[i];
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(piece.entry);
                if (piece.kind != RolePieceDungeon || PlaceOf(proto) != group || !WantedBy(classId, state.role, proto, group) ||
                    !CanWear(player, proto))
                    continue;
                float const worth = ScoreOf(player, state.role, piece.entry, piece.randomProperty);
                if (worth > score)
                {
                    score = worth;
                    best = int32(i);
                }
            }
            return best;
        };

        for (SetPlace group : BodyPlaces)
        {
            uint8 const slot = SlotsOf(group)[0];
            float score = 0.0f;
            int32 const index = bestStored(group, score);
            if (index >= 0 && score > WornScore(player, state.role, slot))
                plan.put[slot] = state.store[index].id;
        }

        // Rings: the best stored ones onto the fingers with the weakest rings.
        {
            std::vector<std::tuple<float, uint32, uint32>> rings; // score, piece id, entry
            for (RolePiece const& piece : state.store)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(piece.entry);
                if (piece.kind == RolePieceDungeon && PlaceOf(proto) == PlaceFinger && CanWear(player, proto))
                    rings.push_back({ ScoreOf(player, state.role, piece.entry, piece.randomProperty), piece.id, piece.entry });
            }
            std::sort(rings.begin(), rings.end(), [](auto const& a, auto const& b) { return std::get<0>(a) > std::get<0>(b); });
            // Of a ring it wears only once the best copy (OnceOnly).
            {
                std::vector<uint32> seen;
                std::erase_if(rings, [&](auto const& ring)
                {
                    uint32 const entry = std::get<2>(ring);
                    if (!OnceOnly(sObjectMgr->GetItemTemplate(entry)))
                        return false;
                    if (std::find(seen.begin(), seen.end(), entry) != seen.end())
                        return true;
                    seen.push_back(entry);
                    return false;
                });
            }
            std::vector<std::pair<float, uint8>> fingers = {
                { WornScore(player, state.role, EQUIPMENT_SLOT_FINGER1), uint8(EQUIPMENT_SLOT_FINGER1) },
                { WornScore(player, state.role, EQUIPMENT_SLOT_FINGER2), uint8(EQUIPMENT_SLOT_FINGER2) } };
            std::sort(fingers.begin(), fingers.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
            std::map<uint8, uint32> ringEntry;
            for (std::size_t i = 0; i < rings.size() && i < fingers.size(); ++i)
                if (std::get<0>(rings[i]) > fingers[i].first)
                {
                    plan.put[fingers[i].second] = std::get<1>(rings[i]);
                    ringEntry[fingers[i].second] = std::get<2>(rings[i]);
                }
            // Not beside a copy of itself that stays on the other finger.
            for (auto const& [slot, entry] : ringEntry)
            {
                uint8 const other = slot == EQUIPMENT_SLOT_FINGER1 ? uint8(EQUIPMENT_SLOT_FINGER2) : uint8(EQUIPMENT_SLOT_FINGER1);
                Item const* stays = ringEntry.count(other) ? nullptr : player->GetItemByPos(INVENTORY_SLOT_BAG_0, other);
                if (stays && stays->GetEntry() == entry && OnceOnly(stays->GetTemplate()))
                    plan.put.erase(slot);
            }
        }

        // The hands: "one-hand weapon plus what the other hand holds" against "two-hand weapon".
        Item const* wornMain = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        Item const* wornOff = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        SetPlace const wornMainGroup = PlaceOfWorn(wornMain, EQUIPMENT_SLOT_MAINHAND);
        float one = 0.0f, two = 0.0f, off = 0.0f;
        bool haveOne = false, haveTwo = false, haveOff = false;
        int32 oneIndex = -1, twoIndex = -1, offIndex = -1;
        if (wornMainGroup == PlaceMainHand)
        {
            one = WornScore(player, state.role, EQUIPMENT_SLOT_MAINHAND);
            haveOne = true;
        }
        else if (wornMainGroup == PlaceTwoHand)
        {
            two = WornScore(player, state.role, EQUIPMENT_SLOT_MAINHAND);
            haveTwo = true;
        }
        if (PlaceOfWorn(wornOff, EQUIPMENT_SLOT_OFFHAND) == PlaceOffHand &&
            WantedBy(classId, state.role, wornOff->GetTemplate(), PlaceOffHand))
        {
            off = WornScore(player, state.role, EQUIPMENT_SLOT_OFFHAND);
            haveOff = true;
        }
        float score = 0.0f;
        if (int32 const index = bestStored(PlaceMainHand, score); index >= 0 && (!haveOne || score > one))
        {
            one = score;
            oneIndex = index;
            haveOne = true;
        }
        if (int32 const index = bestStored(PlaceTwoHand, score); index >= 0 && (!haveTwo || score > two))
        {
            two = score;
            twoIndex = index;
            haveTwo = true;
        }
        if (int32 const index = bestStored(PlaceOffHand, score); index >= 0 && (!haveOff || score > off))
        {
            off = score;
            offIndex = index;
            haveOff = true;
        }
        // A warrior or paladin tank is one with a shield: without one in the set it keeps the weapon it levels with.
        bool const needsShield = state.role == DungeonRole::Tank && (classId == CLASS_WARRIOR || classId == CLASS_PALADIN);
        if (needsShield && !haveOff)
            return plan;
        if (haveOne && (!haveTwo || one + (haveOff ? off : 0.0f) >= two))
        {
            if (oneIndex >= 0)
                plan.put[EQUIPMENT_SLOT_MAINHAND] = state.store[oneIndex].id;
            if (offIndex >= 0)
                plan.put[EQUIPMENT_SLOT_OFFHAND] = state.store[offIndex].id;
        }
        else if (haveTwo && twoIndex >= 0)
        {
            plan.put[EQUIPMENT_SLOT_MAINHAND] = state.store[twoIndex].id;
            plan.clearOffHand = wornOff != nullptr;
        }
        return plan;
    }

    int32 IndexOf(BotRole const& state, uint32 pieceId)
    {
        for (std::size_t i = 0; i < state.store.size(); ++i)
            if (state.store[i].id == pieceId)
                return int32(i);
        return -1;
    }

    std::string NameOf(uint32 entry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        return proto ? proto->Name1 : Acore::StringFormat("item {}", entry);
    }

    void Append(std::string& list, std::string const& what)
    {
        if (!what.empty())
            list += (list.empty() ? "" : ", ") + what;
    }

    std::vector<Item*> BagItems(Player* player)
    {
        std::vector<Item*> items;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                items.push_back(item);
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bag->GetItemByPos(uint8(slot)))
                        items.push_back(item);
        return items;
    }

    bool IsQuiet(Player const* player)
    {
        return player->IsAlive() && !player->IsInCombat() && !player->IsNonMeleeSpellCast(false) && !player->IsInFlight() &&
            !player->IsBeingTeleported();
    }

    std::string Escaped(std::string text)
    {
        CharacterDatabase.EscapeString(text);
        return text;
    }
}

char const* GuildAdventure::ToToken(DungeonRole role)
{
    switch (role)
    {
        case DungeonRole::Tank: return "tank";
        case DungeonRole::Healer: return "heal";
        default: return "none";
    }
}

char const* GuildAdventure::ToText(DungeonRole role)
{
    switch (role)
    {
        case DungeonRole::Tank: return "tank";
        case DungeonRole::Healer: return "healer";
        default: return "no role";
    }
}

RoleService& RoleService::instance()
{
    static RoleService service;
    return service;
}

void RoleService::Load()
{
    _roles.clear();
    _nextPieceId = 1;
    if (QueryResult result = CharacterDatabase.Query("SELECT bot_guid, role, template_id, worn FROM ga_bot_role"))
    {
        do
        {
            Field* fields = result->Fetch();
            BotRole& state = _roles[fields[0].Get<uint32>()];
            state.role = DungeonRole(std::min<uint8>(fields[1].Get<uint8>(), uint8(DungeonRole::Healer)));
            state.templateId = fields[2].Get<uint32>();
            state.worn = fields[3].Get<uint8>() != 0;
            if (state.worn)
                BotDriver::SetRoleMode(ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>()), uint8(state.role));
        } while (result->NextRow());
    }
    uint32 pieces = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT id, bot_guid, kind, slot, item_entry, random_property, enchantments, "
        "charges, durability, flags, creator, stored_at FROM ga_bot_role_gear ORDER BY id"))
    {
        do
        {
            Field* fields = result->Fetch();
            RolePiece piece;
            piece.id = fields[0].Get<uint32>();
            piece.kind = fields[2].Get<uint8>();
            piece.slot = fields[3].Get<uint8>();
            piece.entry = fields[4].Get<uint32>();
            piece.randomProperty = fields[5].Get<int32>();
            piece.enchantments = fields[6].Get<std::string>();
            piece.charges = fields[7].Get<std::string>();
            piece.durability = fields[8].Get<uint16>();
            piece.flags = fields[9].Get<uint32>();
            piece.creator = fields[10].Get<uint32>();
            piece.storedAt = fields[11].Get<uint32>();
            _nextPieceId = std::max(_nextPieceId, piece.id + 1);
            _roles[fields[1].Get<uint32>()].store.push_back(std::move(piece));
            ++pieces;
        } while (result->NextRow());
    }
    LOG_INFO("module", "mod-guild-adventure: dungeon roles: {} bots with a role or a role store, {} pieces in role stores", _roles.size(),
        pieces);
}

BotRole* RoleService::Find(ObjectGuid bot)
{
    auto itr = _roles.find(bot.GetCounter());
    return itr != _roles.end() ? &itr->second : nullptr;
}

BotRole const* RoleService::Find(ObjectGuid bot) const
{
    auto itr = _roles.find(bot.GetCounter());
    return itr != _roles.end() ? &itr->second : nullptr;
}

std::vector<DungeonRole> RoleService::RolesOf(uint32 owner, uint8 classId) const
{
    bool tank = false, healer = false;
    for (SpecTemplate const* spec : sSpecService.ListFor(owner, classId))
    {
        tank = tank || spec->role == 1;
        healer = healer || spec->role == 2;
    }
    std::vector<DungeonRole> roles;
    if (tank)
        roles.push_back(DungeonRole::Tank);
    if (healer)
        roles.push_back(DungeonRole::Healer);
    return roles;
}

DungeonRole RoleService::GetRole(ObjectGuid bot) const
{
    BotRole const* state = Find(bot);
    return state ? state->role : DungeonRole::None;
}

DungeonRole RoleService::EffectiveRole(GuildBot const& bot) const
{
    if (DungeonRole const role = GetRole(bot.guid); role != DungeonRole::None)
        return role;
    SpecTemplate const* spec = sSpecService.GetLevelling(bot.guid, bot.classId);
    return spec && spec->role <= 2 ? DungeonRole(spec->role) : DungeonRole::None;
}

uint32 RoleService::GetTemplateId(ObjectGuid bot) const
{
    BotRole const* state = Find(bot);
    return state ? state->templateId : 0;
}

SpecTemplate const* RoleService::RoleTemplate(uint32 owner, ObjectGuid bot, uint8 classId) const
{
    BotRole const* state = Find(bot);
    if (!state || state->role == DungeonRole::None)
        return nullptr;
    if (state->templateId)
        if (SpecTemplate const* spec = sSpecService.Get(state->templateId); spec && spec->classId == classId &&
            (!spec->owner || spec->owner == owner))
            return spec;
    // The default of the class for the role: its dungeon template with that role (a priest heals as Holy).
    SpecTemplate const* best = nullptr;
    for (SpecTemplate const* spec : sSpecService.ListFor(owner, classId))
    {
        if (spec->owner || spec->role != uint8(state->role))
            continue;
        bool const dungeon = spec->purpose == uint8(SpecPurpose::Group);
        bool const preferred = classId == CLASS_PRIEST && spec->name.find("Holy") != std::string::npos;
        if (!best || (dungeon && best->purpose != uint8(SpecPurpose::Group)) || (dungeon && preferred))
            best = spec;
    }
    return best;
}

void RoleService::Save(ObjectGuid bot, BotRole const& state, Player* player, bool whole) const
{
    uint32 const low = bot.GetCounter();
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    // The bags and the body in the same transaction as the store: a piece is in exactly one of them, whenever the
    // server stops. A change of set writes the whole character (talents, the active talent spec).
    if (player && player->IsInWorld() && !player->IsDuringRemoveFromWorld())
    {
        if (whole && !player->IsBeingTeleportedFar())
            player->SaveToDB(trans, false, false);
        else
            player->SaveInventoryAndGoldToDB(trans);
    }
    trans->Append("REPLACE INTO ga_bot_role (bot_guid, role, template_id, worn, set_at) VALUES ({}, {}, {}, {}, {})", low,
        uint32(state.role), state.templateId, state.worn ? 1 : 0, uint32(GameTime::GetGameTime().count()));
    trans->Append("DELETE FROM ga_bot_role_gear WHERE bot_guid = {}", low);
    for (RolePiece const& piece : state.store)
        trans->Append("INSERT INTO ga_bot_role_gear (id, bot_guid, kind, slot, item_entry, random_property, enchantments, charges, "
            "durability, flags, creator, stored_at) VALUES ({}, {}, {}, {}, {}, {}, '{}', '{}', {}, {}, {}, {})", piece.id, low,
            uint32(piece.kind), uint32(piece.slot), piece.entry, piece.randomProperty, Escaped(piece.enchantments),
            Escaped(piece.charges), piece.durability, piece.flags, piece.creator, piece.storedAt);
    CharacterDatabase.CommitTransaction(trans);
}

bool RoleService::SetRole(AdventureGuild& guild, GuildBot const& bot, DungeonRole role, uint32 templateId, std::string& error)
{
    if (bot.guid == guild.leader)
    {
        error = "the guild leader takes no dungeon role";
        return false;
    }
    if (role != DungeonRole::None)
    {
        std::vector<DungeonRole> const roles = RolesOf(guild.ownerAccount, bot.classId);
        if (std::find(roles.begin(), roles.end(), role) == roles.end())
        {
            error = Acore::StringFormat("its class cannot be a {}", ToText(role));
            return false;
        }
        if (templateId)
        {
            SpecTemplate const* spec = sSpecService.Get(templateId);
            if (!spec || spec->classId != bot.classId || (spec->owner && spec->owner != guild.ownerAccount) ||
                (!spec->owner && spec->role != uint8(role)))
            {
                error = Acore::StringFormat("that is no talent template of its class for a {}", ToText(role));
                return false;
            }
        }
    }
    else
        templateId = 0;

    BotRole* state = Find(bot.guid);
    if (state && state->worn && state->role != role)
    {
        error = Acore::StringFormat("{} wears its dungeon set right now: change the role after the dungeon run", bot.name);
        return false;
    }
    if (!state)
    {
        if (role == DungeonRole::None)
            return true;
        state = &_roles[bot.guid.GetCounter()];
    }
    bool const changed = state->role != role || state->templateId != templateId;
    state->role = role;
    state->templateId = templateId;
    state->nextStep = 0;
    state->storageSeen = 0;
    state->gearLevelAt = 0;
    if (changed)
    {
        Save(bot.guid, *state, nullptr);
        SpecTemplate const* spec = RoleTemplate(guild.ownerAccount, bot.guid, bot.classId);
        LOG_INFO(ActivityLog, "activity: {} has the dungeon role {} now{}", bot.name, ToText(role),
            spec ? Acore::StringFormat(" (talents for a dungeon run: \"{}\")", spec->name) : std::string());
    }
    return true;
}

bool RoleService::IsDungeonSetOn(ObjectGuid bot) const
{
    BotRole const* state = Find(bot);
    return state && state->worn;
}

std::vector<RolePiece> const* RoleService::GetStore(ObjectGuid bot) const
{
    BotRole const* state = Find(bot);
    return state ? &state->store : nullptr;
}

uint32 RoleService::CountDungeonPieces(ObjectGuid bot) const
{
    BotRole const* state = Find(bot);
    if (!state)
        return 0;
    // In the store while the bot levels; on its body (in place of the put-aside levelling pieces) on a run.
    uint8 const kind = state->worn ? RolePieceLevelling : RolePieceDungeon;
    return uint32(std::count_if(state->store.begin(), state->store.end(), [kind](RolePiece const& piece) { return piece.kind == kind; }));
}

void RoleService::Hold(ObjectGuid bot, bool held)
{
    if (BotRole* state = Find(bot))
        state->held = held;
}

void RoleService::LookSoon(ObjectGuid bot)
{
    if (BotRole* state = Find(bot))
        state->nextStep = 0;
}

bool RoleService::IsRoleUpgrade(Player* bot, uint32 itemEntry, int32 randomProperty) const
{
    BotRole const* state = bot ? Find(bot->GetGUID()) : nullptr;
    if (!state || state->role == DungeonRole::None || !GetSettings().rolesEnable)
        return false;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
    SetPlace const group = PlaceOf(proto);
    if (!WantedBy(bot->getClass(), state->role, proto, group) || !CanWear(bot, proto))
        return false;
    float const score = ScoreOf(bot, state->role, itemEntry, randomProperty);
    if (score <= 0.0f)
        return false;
    std::vector<Standing> const standing = StandingOf(bot, *state, group);
    // A piece it wears only once can only take the place of the copy it has already (worn or in the store), and it
    // still has to be one of the best of its place: a unique ring that beat the copy worn on the other finger was
    // collected although the store held two better rings, and given back in the same minute (Duduheal's Aquamarine
    // Signets, in and out every minute on 2026-11-14).
    if (OnceOnly(proto))
        for (Standing const& stand : standing)
        {
            Item const* worn = stand.index < 0 ? bot->GetItemByPos(INVENTORY_SLOT_BAG_0, stand.slot) : nullptr;
            uint32 const entry = stand.index < 0 ? (worn ? worn->GetEntry() : 0) : state->store[stand.index].entry;
            if (entry == itemEntry)
            {
                if (score <= stand.score * UpgradeFactor + UpgradeMargin)
                    return false;
                break;
            }
        }
    std::size_t const capacity = Capacity(group);
    float const threshold = standing.size() >= capacity ? standing[capacity - 1].score : 0.0f;
    return score > threshold * UpgradeFactor + UpgradeMargin;
}

bool RoleService::Wants(Player* bot, Item const* item) const
{
    BotRole const* state = bot && item ? Find(bot->GetGUID()) : nullptr;
    if (!state || state->role == DungeonRole::None)
        return false;
    // A levelling piece that was taken off during the run: it goes back on afterwards.
    for (auto const& [slot, guid] : state->shared)
        if (guid == item->GetGUID())
            return true;
    return IsRoleUpgrade(bot, item->GetEntry(), item->GetItemRandomPropertyId());
}

int32 RoleService::RewardChoice(Player* bot, Quest const* quest) const
{
    BotRole const* state = bot && quest ? Find(bot->GetGUID()) : nullptr;
    if (!state || state->role == DungeonRole::None)
        return -1;
    int32 best = -1;
    float bestScore = 0.0f;
    for (uint32 i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
    {
        uint32 const item = quest->RewardChoiceItemId[i];
        if (!item || !IsRoleUpgrade(bot, item, 0))
            continue;
        float const score = ScoreOf(bot, state->role, item, 0);
        if (best < 0 || score > bestScore)
        {
            best = int32(i);
            bestScore = score;
        }
    }
    return best;
}

void RoleService::Stash(Player* player, BotRole& state, Item* item, uint8 kind, uint8 slot)
{
    RolePiece piece;
    piece.id = _nextPieceId++;
    piece.kind = kind;
    piece.slot = slot;
    // A soulbound piece that was never worn (a dungeon drop, or one taken out of the guild storage) keeps that fact.
    if (kind == RolePieceDungeon && slot == NoSlot && sStorageService.IsMarkedUnworn(item))
        piece.slot = RoleSlotUnworn;
    piece.entry = item->GetEntry();
    piece.randomProperty = item->GetItemRandomPropertyId();
    std::ostringstream enchants;
    for (uint8 i = 0; i < MAX_ENCHANTMENT_SLOT; ++i)
        enchants << item->GetEnchantmentId(EnchantmentSlot(i)) << ' ' << item->GetEnchantmentDuration(EnchantmentSlot(i)) << ' '
            << item->GetEnchantmentCharges(EnchantmentSlot(i)) << ' ';
    piece.enchantments = enchants.str();
    std::ostringstream charges;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        charges << item->GetSpellCharges(i) << ' ';
    piece.charges = charges.str();
    piece.durability = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
    piece.flags = item->GetUInt32Value(ITEM_FIELD_FLAGS);
    piece.creator = item->GetGuidValue(ITEM_FIELD_CREATOR).GetCounter();
    piece.storedAt = uint32(GameTime::GetGameTime().count());
    state.store.push_back(std::move(piece));
    state.gearLevelAt = 0;
    player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
}

bool RoleService::Unstash(Player* player, BotRole& state, std::size_t index, uint8 slot, Item** placedItem)
{
    if (index >= state.store.size())
        return false;
    RolePiece const piece = state.store[index];
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(piece.entry);
    if (!proto)
        return false;
    // The piece as it went in: created without a random property of its own, then every field item_instance holds.
    Item* item = Item::CreateItem(piece.entry, 1, player, true, 0);
    if (!item)
        return false;
    item->SetInt32Value(ITEM_FIELD_RANDOM_PROPERTIES_ID, piece.randomProperty);
    if (piece.randomProperty < 0)
        item->UpdateItemSuffixFactor();
    {
        std::vector<std::string_view> const tokens = Acore::Tokenize(piece.enchantments, ' ', false);
        if (tokens.size() == MAX_ENCHANTMENT_SLOT * MAX_ENCHANTMENT_OFFSET)
        {
            for (uint32 i = 0; i < tokens.size(); ++i)
                item->SetUInt32Value(ITEM_FIELD_ENCHANTMENT_1_1 + i, Acore::StringTo<uint32>(tokens[i]).value_or(0));
        }
        else if (piece.randomProperty)
        {
            // (No usable enchantment data: at least the random property's own.)
            item->SetItemRandomProperties(piece.randomProperty);
            item->RemoveFromUpdateQueueOf(player);
        }
    }
    {
        std::vector<std::string_view> const tokens = Acore::Tokenize(piece.charges, ' ', false);
        if (tokens.size() == MAX_ITEM_PROTO_SPELLS)
            for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
                item->SetSpellCharges(i, Acore::StringTo<int32>(tokens[i]).value_or(0));
    }
    item->SetUInt32Value(ITEM_FIELD_FLAGS, piece.flags);
    item->SetUInt32Value(ITEM_FIELD_DURABILITY, std::min<uint32>(piece.durability, proto->MaxDurability));
    if (piece.creator)
        item->SetGuidValue(ITEM_FIELD_CREATOR, ObjectGuid::Create<HighGuid::Player>(piece.creator));

    bool placed = false;
    if (slot != NoSlot)
    {
        uint16 dest = 0;
        if (player->CanEquipItem(slot, dest, item, false) == EQUIP_ERR_OK)
        {
            Item* equipped = player->EquipItem(dest, item, true);
            placed = true;
            if (placedItem)
                *placedItem = equipped;
        }
    }
    else
    {
        ItemPosCountVec dest;
        if (player->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) == EQUIP_ERR_OK)
        {
            Item* stored = player->StoreItem(dest, item, true);
            placed = true;
            if (placedItem)
                *placedItem = stored;
            // Never worn when it went in, never worn now: the guild storage takes it again.
            if (stored && piece.kind == RolePieceDungeon && piece.slot == RoleSlotUnworn)
                sStorageService.MarkUnworn(player, stored, "out of its role store");
        }
    }
    if (!placed)
    {
        item->RemoveFromUpdateQueueOf(player);
        delete item;
        return false;
    }
    state.store.erase(state.store.begin() + index);
    state.gearLevelAt = 0;
    return true;
}

std::string RoleService::Release(Player* player, BotRole& state, bool all)
{
    std::vector<uint32> go;
    if (all || state.role == DungeonRole::None)
    {
        for (RolePiece const& piece : state.store)
            if (piece.kind == RolePieceDungeon)
                go.push_back(piece.id);
    }
    else
    {
        // A place keeps its best piece (two rings) when that is better for the role than what the bot wears there.
        std::vector<bool> keep(state.store.size(), false);
        for (SetPlace group : AllPlaces)
        {
            std::vector<Standing> const standing = StandingOf(player, state, group);
            for (std::size_t i = 0; i < standing.size() && i < Capacity(group); ++i)
                if (ItemTemplate const* proto = standing[i].index >= 0 ?
                    sObjectMgr->GetItemTemplate(state.store[standing[i].index].entry) : nullptr;
                    proto && standing[i].score > 0.0f && WantedBy(player->getClass(), state.role, proto, group) &&
                    // (A piece for a later level stays; one of a kind the class never wears goes.)
                    (!proto->GetSkill() || player->GetSkillValue(proto->GetSkill()) > 0))
                    keep[standing[i].index] = true;
        }
        for (std::size_t i = 0; i < state.store.size(); ++i)
            if (state.store[i].kind == RolePieceDungeon && !keep[i])
                go.push_back(state.store[i].id);
    }
    std::string names;
    for (uint32 id : go)
    {
        int32 const index = IndexOf(state, id);
        if (index < 0)
            continue;
        std::string const name = NameOf(state.store[index].entry);
        if (Unstash(player, state, std::size_t(index), NoSlot))
            Append(names, name);
    }
    return names;
}

std::string RoleService::WearForLevelling(Player* player, BotRole& state)
{
    // the owner, 2026-11-14 (a decision card of the maintainers): [quote left out]. Most tanks and
    // healers levelled far below their level while their role store held better pieces (Wartank, level 46: a staff of
    // item level 37 and a necklace of 18, beside a sword of 41, a shield of 48 and a necklace of 41 in its store): a
    // piece goes into the store when it is no levelling upgrade at that moment, and mod-playerbots never rates
    // jewellery as one. Now the store's pieces are rated by the levelling talents once a minute, like what is worn.
    uint32 const mode = uint32(std::clamp<int32>(sBotSettings.Get(player, Setting::DungeonRolesWearForLevelling), 0, 2));
    if (!mode || state.role == DungeonRole::None || state.worn || player->GetActiveSpec() != 0 || !BotDriver::GetAI(player))
        return "";

    auto levelling = [&](uint32 entry, int32 randomProperty) { return BotDriver::ItemScore(player, entry, randomProperty); };
    auto wornScore = [&](uint8 slot)
    {
        Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        return item ? levelling(item->GetEntry(), item->GetItemRandomPropertyId()) : 0.0f;
    };
    // The store's best piece for a place by the levelling talents (0: none it can wear).
    auto bestStored = [&](SetPlace group, std::vector<uint32> const& skip, float& score) -> uint32
    {
        uint32 best = 0;
        score = 0.0f;
        for (RolePiece const& piece : state.store)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(piece.entry);
            if (piece.kind != RolePieceDungeon || PlaceOf(proto) != group || !CanWear(player, proto) ||
                std::find(skip.begin(), skip.end(), piece.id) != skip.end())
                continue;
            float const worth = levelling(piece.entry, piece.randomProperty);
            // (A bare place takes a necklace, ring or relic whatever its stats: B21.)
            if (worth > score || (!best && BotDriver::IsJewellery(proto)))
            {
                score = std::max(worth, score);
                best = piece.id;
            }
        }
        return best;
    };
    auto clearlyBetter = [](float candidate, float worn) { return candidate > worn * UpgradeFactor + UpgradeMargin; };
    // Would the bot keep it on? Armour and weapons by mod-playerbots' own rating of an upgrade (its threshold, its
    // rule for the armour kind and the stats a class uses): otherwise mod-playerbots puts the old piece back on as
    // soon as it sees it in the bags (the first build put a healer's Embersilk pieces on by the stat weights alone,
    // and mod-playerbots rated the pieces they replaced as upgrades over them). Jewellery by the stat weights: a
    // bare place takes it, a worn one when it is clearly better (mod-playerbots rates no jewellery as an upgrade, and
    // so never puts the old one back either).
    auto keeps = [&](uint32 pieceId, float score, uint8 slot)
    {
        int32 const index = IndexOf(state, pieceId);
        if (index < 0)
            return false;
        RolePiece const& piece = state.store[index];
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(piece.entry);
        if (proto && BotDriver::IsJewellery(proto))
            return !BotDriver::WearsUniqueAlready(player, proto, slot) &&
                (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) || clearlyBetter(score, wornScore(slot)));
        return BotDriver::IsUpgrade(player, piece.entry, piece.randomProperty);
    };

    std::string on;
    bool changed = false;
    // A piece of the store into an equipment slot by the game's own swap; what was worn there goes into its bags.
    auto wear = [&](uint32 pieceId, uint8 slot)
    {
        int32 const index = IndexOf(state, pieceId);
        if (index < 0)
            return false;
        uint32 const entry = state.store[index].entry;
        Item* item = nullptr;
        if (!Unstash(player, state, std::size_t(index), NoSlot, &item) || !item)
            return false;
        Item const* before = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        std::string const replaced = before ? before->GetTemplate()->Name1 : std::string();
        player->SwapItem(item->GetPos(), uint16((INVENTORY_SLOT_BAG_0 << 8) | slot));
        if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) != item)
        {
            // It does not go on after all (unique, a two-hand weapon in the way ...): back into the store.
            if (item->IsInWorld() && item->GetOwnerGUID() == player->GetGUID() && !item->IsEquipped())
                Stash(player, state, item, RolePieceDungeon, NoSlot);
            return false;
        }
        Append(on, Acore::StringFormat("{} ({}{})", NameOf(entry), SlotName(slot), replaced.empty() ? "" : ", for " + replaced));
        changed = true;
        return true;
    };
    // What a hand holds into the bags (before a two-hand weapon goes on).
    auto intoBags = [&](uint8 slot) -> Item*
    {
        Item* worn = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!worn)
            return nullptr;
        ItemPosCountVec dest;
        if (player->CanStoreItem(NULL_BAG, NULL_SLOT, dest, worn, false) != EQUIP_ERR_OK)
            return nullptr;
        player->RemoveItem(INVENTORY_SLOT_BAG_0, slot, true);
        return player->StoreItem(dest, worn, true);
    };

    // Armour, the necklace, and the ranged place (a relic always, a bow, gun or wand only with weapons).
    for (SetPlace group : BodyPlaces)
    {
        uint8 const slot = SlotsOf(group)[0];
        float score = 0.0f;
        uint32 const id = bestStored(group, {}, score);
        if (!id)
            continue;
        if (group == PlaceRanged && mode < 2)
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(state.store[IndexOf(state, id)].entry);
                !proto || proto->InventoryType != INVTYPE_RELIC)
                continue;
        if (keeps(id, score, slot))
            wear(id, slot);
    }

    // Rings: the store's best onto the finger with the weaker ring.
    {
        std::vector<uint32> tried;
        for (uint32 round = 0; round < 2; ++round)
        {
            float score = 0.0f;
            uint32 const id = bestStored(PlaceFinger, tried, score);
            if (!id)
                break;
            tried.push_back(id);
            float const first = wornScore(EQUIPMENT_SLOT_FINGER1), second = wornScore(EQUIPMENT_SLOT_FINGER2);
            bool const bareFirst = !player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_FINGER1);
            bool const bareSecond = !player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_FINGER2);
            uint8 const slot = bareFirst ? uint8(EQUIPMENT_SLOT_FINGER1) : bareSecond ? uint8(EQUIPMENT_SLOT_FINGER2) :
                first <= second ? uint8(EQUIPMENT_SLOT_FINGER1) : uint8(EQUIPMENT_SLOT_FINGER2);
            bool const bare = slot == EQUIPMENT_SLOT_FINGER1 ? bareFirst : bareSecond;
            if (bare || clearlyBetter(score, slot == EQUIPMENT_SLOT_FINGER1 ? first : second))
                if (keeps(id, score, slot))
                    wear(id, slot);
        }
    }

    // The hands, with weapons, piece by piece the way mod-playerbots rates them (a weapon against the one in the
    // main hand, what the other hand holds against what it holds): a sword and a shield against a two-hand weapon
    // would be put back at once, because mod-playerbots takes the two-hand weapon from the bags when it alone is
    // better than the sword (seen on the first build: a healer put on a one-hand blade and an orb for its staff,
    // and wore the staff again within the minute).
    if (mode >= 2)
    {
        Item const* main = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        bool const mainTwoHand = main && main->GetTemplate()->InventoryType == INVTYPE_2HWEAPON;
        float const mainScore = wornScore(EQUIPMENT_SLOT_MAINHAND);
        float twoScore = 0.0f, oneScore = 0.0f;
        uint32 const two = bestStored(PlaceTwoHand, {}, twoScore);
        uint32 const one = bestStored(PlaceMainHand, {}, oneScore);
        // The better of the two for the main hand, when it is clearly better than what is there.
        bool const wantTwo = two && twoScore >= oneScore && (!main || clearlyBetter(twoScore, mainScore)) &&
            keeps(two, twoScore, EQUIPMENT_SLOT_MAINHAND);
        bool const wantOne = !wantTwo && one && (!main || clearlyBetter(oneScore, mainScore)) &&
            keeps(one, oneScore, EQUIPMENT_SLOT_MAINHAND);
        if (wantTwo)
        {
            Item* aside = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND) ? intoBags(EQUIPMENT_SLOT_OFFHAND) : nullptr;
            if (!wear(two, EQUIPMENT_SLOT_MAINHAND) && aside && aside->IsInWorld())
                player->SwapItem(aside->GetPos(), uint16((INVENTORY_SLOT_BAG_0 << 8) | EQUIPMENT_SLOT_OFFHAND));
        }
        else if (wantOne)
            wear(one, EQUIPMENT_SLOT_MAINHAND);
        // The other hand, beside a one-hand weapon.
        Item const* nowMain = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        if (!(nowMain && nowMain->GetTemplate()->InventoryType == INVTYPE_2HWEAPON) && !(mainTwoHand && !wantOne))
        {
            float offScore = 0.0f;
            uint32 const offPiece = bestStored(PlaceOffHand, {}, offScore);
            Item const* off = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            if (offPiece && (off ? clearlyBetter(offScore, wornScore(EQUIPMENT_SLOT_OFFHAND)) : offScore > 0.0f) &&
                keeps(offPiece, offScore, EQUIPMENT_SLOT_OFFHAND))
                wear(offPiece, EQUIPMENT_SLOT_OFFHAND);
        }
    }
    if (changed)
        player->AutoUnequipOffhandIfNeed();
    return on;
}

std::string RoleService::Collect(Player* player, BotRole& state)
{
    std::string collected;
    if (state.role != DungeonRole::None)
        for (Item* item : BagItems(player))
        {
            uint32 const entry = item->GetEntry();
            int32 const randomProperty = item->GetItemRandomPropertyId();
            if (PlaceOf(item->GetTemplate()) == PlaceNone || !IsRoleUpgrade(player, entry, randomProperty))
                continue;
            // Levelling comes first: what the bot would put on for its levelling talents is left to it.
            if (BotDriver::IsUpgrade(player, entry, randomProperty))
                continue;
            Append(collected, item->GetTemplate()->Name1);
            Stash(player, state, item, RolePieceDungeon, NoSlot);
        }
    return collected;
}

bool RoleService::SwitchTalents(GuildBot const& bot, Player* player, SpecTemplate const* roleSpec, bool toRole)
{
    if (player->GetLevel() < 10)
        return false;
    if (toRole)
    {
        if (!roleSpec)
            return false;
        if (uint32 const cost = GetSettings().rolesRespecCost; cost && player->GetActiveSpec() != 1)
        {
            if (!player->HasEnoughMoney(cost))
            {
                LOG_INFO(ActivityLog, "activity: {} cannot pay {} for the change to its dungeon talents: it keeps its levelling talents",
                    bot.name, GuildBudget::MoneyText(cost));
                return false;
            }
            player->ModifyMoney(-int32(cost));
        }
        if (player->GetSpecsCount() < 2)
            player->UpdateSpecCount(2);
    }
    uint8 const want = toRole ? 1 : 0;
    if (player->GetActiveSpec() != want)
    {
        // The game empties mana and energy on a change of spec; the guild's change is free of that too.
        Powers const type = player->getPowerType();
        uint32 const mana = player->GetPower(POWER_MANA);
        uint32 const power = player->GetPower(type);
        player->ActivateSpec(want);
        player->SetPower(POWER_MANA, std::min(mana, player->GetMaxPower(POWER_MANA)));
        if (type == POWER_ENERGY)
            player->SetPower(type, std::min(power, player->GetMaxPower(type)));
    }
    if (toRole)
    {
        sSpecService.SetRunTemplate(bot.guid, roleSpec->id);
        if (std::string const done = sSpecService.ApplyTemplate(player, *roleSpec); !done.empty())
            LOG_INFO(ActivityLog, "activity: {} (level {}) dungeon talents: {}", bot.name, uint32(player->GetLevel()), done);
    }
    else
    {
        sSpecService.ClearRunTemplate(bot.guid);
        if (std::string const done = sSpecService.Maintain(player); !done.empty())
            LOG_INFO(ActivityLog, "activity: {} (level {}) {}", bot.name, uint32(player->GetLevel()), done);
    }
    // mod-playerbots takes its strategies (tank, healer, damage; the rotation) from the talents that are active now.
    BotDriver::ResetStrategies(player);
    return true;
}

bool RoleService::PutDungeonSetOn(Player* player, BotRole& state, std::string& moved)
{
    SetPlan const plan = PlanDungeonSet(player, state);
    std::string on, aside;

    std::string lastAside;
    auto putAside = [&](uint8 slot)
    {
        lastAside.clear();
        if (Item* worn = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            lastAside = worn->GetTemplate()->Name1;
            Stash(player, state, worn, RolePieceLevelling, slot);
            return true;
        }
        return false;
    };
    // An empty place that gets a piece of the dungeon set is remembered as empty, so it is empty again afterwards.
    auto markEmpty = [&](uint8 slot)
    {
        RolePiece marker;
        marker.id = _nextPieceId++;
        marker.kind = RolePieceLevelling;
        marker.slot = slot;
        marker.storedAt = uint32(GameTime::GetGameTime().count());
        state.store.push_back(std::move(marker));
    };

    if (plan.clearOffHand && putAside(EQUIPMENT_SLOT_OFFHAND))
        Append(aside, lastAside);
    // The main hand first (a two-hand weapon there blocks the other hand).
    std::vector<uint8> order;
    if (plan.put.count(EQUIPMENT_SLOT_MAINHAND))
        order.push_back(EQUIPMENT_SLOT_MAINHAND);
    for (auto const& [slot, id] : plan.put)
        if (slot != EQUIPMENT_SLOT_MAINHAND)
            order.push_back(slot);
    std::vector<uint8> changed;
    for (uint8 slot : order)
    {
        uint32 const pieceId = plan.put.at(slot);
        if (!putAside(slot))
            markEmpty(slot);
        int32 const index = IndexOf(state, pieceId);
        std::string const name = index >= 0 ? NameOf(state.store[index].entry) : std::string();
        if (index >= 0 && Unstash(player, state, std::size_t(index), slot))
        {
            Append(on, name);
            Append(aside, lastAside);
        }
        else
        {
            // It does not go on after all: the levelling piece (or the empty place) comes back at once.
            for (std::size_t i = 0; i < state.store.size(); ++i)
                if (state.store[i].kind == RolePieceLevelling && state.store[i].slot == slot)
                {
                    if (!state.store[i].entry)
                        state.store.erase(state.store.begin() + i);
                    else
                        Unstash(player, state, i, slot);
                    break;
                }
        }
        changed.push_back(slot);
    }
    player->AutoUnequipOffhandIfNeed();

    // The levelling pieces that stay on: one that is taken off during the run is put on again after it.
    state.shared.clear();
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (std::find(changed.begin(), changed.end(), slot) == changed.end())
            if (Item const* worn = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                state.shared[slot] = worn->GetGUID();

    if (!on.empty())
        moved = "on: " + on + (aside.empty() ? "" : "; into its role store: " + aside);
    return true;
}

bool RoleService::PutLevellingSetOn(Player* player, BotRole& state, std::string& moved)
{
    std::string on, intoStore, left;

    auto intoDungeonSet = [&](uint8 slot)
    {
        if (Item* worn = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            Append(intoStore, worn->GetTemplate()->Name1);
            Stash(player, state, worn, RolePieceDungeon, NoSlot);
        }
    };

    // The pieces that were put aside, the main hand first.
    std::vector<std::pair<uint8, uint32>> back;
    for (RolePiece const& piece : state.store)
        if (piece.kind == RolePieceLevelling)
            back.push_back({ piece.slot, piece.id });
    std::stable_sort(back.begin(), back.end(), [](auto const& a, auto const& b)
    {
        return (a.first == EQUIPMENT_SLOT_MAINHAND) > (b.first == EQUIPMENT_SLOT_MAINHAND);
    });
    // A two-hand weapon comes back into the main hand: the other hand is emptied first.
    for (auto const& [slot, id] : back)
        if (int32 const index = IndexOf(state, id); index >= 0 && slot == EQUIPMENT_SLOT_MAINHAND)
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(state.store[index].entry);
                proto && proto->InventoryType == INVTYPE_2HWEAPON)
                intoDungeonSet(EQUIPMENT_SLOT_OFFHAND);

    for (auto const& [slot, id] : back)
    {
        int32 index = IndexOf(state, id);
        if (index < 0)
            continue;
        if (slot < EQUIPMENT_SLOT_END)
            intoDungeonSet(slot);
        index = IndexOf(state, id);
        if (!state.store[index].entry)
        {
            // The place was empty before the run.
            state.store.erase(state.store.begin() + index);
            continue;
        }
        std::string const name = NameOf(state.store[index].entry);
        if (slot < EQUIPMENT_SLOT_END && Unstash(player, state, std::size_t(index), slot))
            Append(on, name);
        else if (Unstash(player, state, std::size_t(IndexOf(state, id)), NoSlot))
            Append(left, name + " (into its bags)");
        else
            Append(left, name + " (stays in its role store)");
    }

    // Levelling pieces that stayed on and were taken off during the run (the tank put a dungeon drop on in their
    // place): they go back on, the piece from the run joins the dungeon set.
    for (auto const& [slot, guid] : state.shared)
    {
        Item* worn = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (worn && worn->GetGUID() == guid)
            continue;
        Item* old = player->GetItemByGuid(guid);
        if (!old || old->IsEquipped())
            continue;
        if (worn)
        {
            Append(intoStore, worn->GetTemplate()->Name1);
            Stash(player, state, worn, RolePieceDungeon, NoSlot);
        }
        uint16 dest = 0;
        if (player->CanEquipItem(slot, dest, old, false) == EQUIP_ERR_OK)
        {
            std::string const name = old->GetTemplate()->Name1;
            player->RemoveItem(old->GetBagSlot(), old->GetSlot(), true);
            player->EquipItem(dest, old, true);
            Append(on, name);
        }
    }
    state.shared.clear();
    player->AutoUnequipOffhandIfNeed();

    if (!on.empty() || !intoStore.empty() || !left.empty())
        moved = (on.empty() ? std::string() : "on again: " + on) +
            (intoStore.empty() ? std::string() : std::string(on.empty() ? "" : "; ") + "into its role store: " + intoStore) +
            (left.empty() ? std::string() : std::string(on.empty() && intoStore.empty() ? "" : "; ") + "not put on: " + left);
    return true;
}

bool RoleService::EnterDungeonMode(AdventureGuild& guild, GuildBot const& bot, Player* player)
{
    BotRole* state = Find(bot.guid);
    if (!state || state->role == DungeonRole::None || !GetSettings().rolesEnable || state->worn)
        return true;
    if (!IsQuiet(player))
        return false;

    SpecTemplate const* spec = RoleTemplate(guild.ownerAccount, bot.guid, bot.classId);
    bool const talents = SwitchTalents(bot, player, spec, true);
    std::string moved;
    PutDungeonSetOn(player, *state, moved);
    state->worn = true;
    BotDriver::SetRoleMode(bot.guid, uint8(state->role));
    Save(bot.guid, *state, player, true);
    LOG_INFO(ActivityLog, "activity: {} gets ready for the dungeon as {}: talents {}; dungeon set {}; gear level {}", bot.name,
        ToText(state->role), talents && spec ? Acore::StringFormat("\"{}\" ({})", spec->name, SpecService::CurrentSummary(player)) :
        std::string("unchanged"), moved.empty() ? std::string("has no piece of its own yet, it goes in what it wears") : moved,
        uint32(Readiness::GearLevel(player)));
    return true;
}

bool RoleService::LeaveDungeonMode(GuildBot const& bot, Player* player)
{
    BotRole* state = Find(bot.guid);
    if (!state || !state->worn)
    {
        // (A run template without the set: the role was taken away in between.)
        if (sSpecService.HasRunTemplate(bot.guid) && IsQuiet(player))
            SwitchTalents(bot, player, nullptr, false);
        return true;
    }
    if (!IsQuiet(player))
        return false;

    std::string moved;
    PutLevellingSetOn(player, *state, moved);
    bool const talents = SwitchTalents(bot, player, nullptr, false);
    state->worn = false;
    BotDriver::SetRoleMode(bot.guid, 0);
    state->held = false;
    state->nextStep = 0;
    Save(bot.guid, *state, player, true);
    LOG_INFO(ActivityLog, "activity: {} is back from the dungeon: talents {}; levelling set {}", bot.name,
        talents ? Acore::StringFormat("for levelling again ({})", SpecService::CurrentSummary(player)) : std::string("unchanged"),
        moved.empty() ? std::string("was on all the time") : moved);
    return true;
}

void RoleService::Step(AdventureGuild& guild, GuildBot const& bot, Player* player, bool onRun)
{
    BotRole* state = Find(bot.guid);
    if (!state || !GetSettings().rolesEnable || bot.guid == guild.leader)
        return;

    // The dungeon set is on without a run (the server stopped in the middle of one, the order was changed): back.
    if (state->worn)
    {
        if (!onRun && !state->held)
            LeaveDungeonMode(bot, player);
        else if (!sSpecService.HasRunTemplate(bot.guid) && player->GetActiveSpec() == 1)
            if (SpecTemplate const* spec = RoleTemplate(guild.ownerAccount, bot.guid, bot.classId))
                sSpecService.SetRunTemplate(bot.guid, spec->id); // after a restart during a run
        return;
    }
    // The second talent spec is active although the levelling set is on (a stop between the two saves).
    if (!onRun && !state->held && player->GetActiveSpec() == 1 && IsQuiet(player))
        SwitchTalents(bot, player, nullptr, false);

    uint32 const now = getMSTime();
    if (state->nextStep && now < state->nextStep)
        return;
    if (!IsQuiet(player))
        return;
    state->nextStep = std::max<uint32>(now + StepMs, 1);

    bool dirty = false;
    // Levelling pieces still in the store (a change back that could not finish: full bags): another try.
    for (std::size_t i = 0; i < state->store.size();)
    {
        RolePiece const& piece = state->store[i];
        if (piece.kind != RolePieceLevelling)
        {
            ++i;
            continue;
        }
        dirty = true;
        if (!piece.entry)
        {
            state->store.erase(state->store.begin() + i);
            continue;
        }
        std::string const name = NameOf(piece.entry);
        bool const free = piece.slot < EQUIPMENT_SLOT_END && !player->GetItemByPos(INVENTORY_SLOT_BAG_0, piece.slot);
        if ((free && Unstash(player, *state, i, piece.slot)) || Unstash(player, *state, i, NoSlot))
            LOG_INFO(ActivityLog, "activity: {} took {} out of its role store again (a levelling piece)", bot.name, name);
        else
            ++i;
    }

    // The pieces of the dungeon set that are better for levelling than what it wears go on (they stay part of the
    // set: a run finds them on it). What they replace goes into the bags: the role store takes it when the set wants
    // it (Collect), the guild's loot filter the rest.
    bool woreFromStore = false;
    if (!onRun && !state->held)
        if (std::string const worn = WearForLevelling(player, *state); !worn.empty())
        {
            dirty = true;
            woreFromStore = true;
            LOG_INFO(ActivityLog, "activity: {} (level {}) wears pieces of its dungeon set ({}) while it levels: they are better for its "
                "levelling talents than what it wore: {}", bot.name, uint32(player->GetLevel()), ToText(state->role), worn);
        }
    if (std::string const collected = Collect(player, *state); !collected.empty())
    {
        dirty = true;
        LOG_INFO(ActivityLog, "activity: {} put into its role store for its dungeon set ({}): {}", bot.name, ToText(state->role), collected);
    }
    if (std::string const released = Release(player, *state, false); !released.empty())
    {
        dirty = true;
        LOG_INFO(ActivityLog, "activity: {} took out of its role store what its dungeon set no longer needs: {}", bot.name, released);
    }
    if (dirty)
        Save(bot.guid, *state, player);
    if (woreFromStore)
        sLootService.SweepBags(player);
}

std::string RoleService::TakeFromStorage(Player* bot, uint32 owner, std::vector<StorageItem> const& stock)
{
    BotRole* state = bot ? Find(bot->GetGUID()) : nullptr;
    Settings const& settings = GetSettings();
    if (!state || state->role == DungeonRole::None || state->worn || !settings.rolesEnable || !settings.rolesFromStorage ||
        !IsQuiet(bot))
        return "";
    AdventureGuild* guild = sGuildAdventureMgr.FindGuild(owner);
    if (!guild)
        return "";

    // Only when the storage or the bot's level changed since the last look.
    std::pair<uint32, uint64> const totals = sStorageService.GetTotals(owner);
    uint64 const fingerprint = ((uint64(totals.first) << 40) ^ totals.second) | 1;
    if (state->storageSeen == fingerprint && state->storageLevel == bot->GetLevel())
        return "";
    state->storageSeen = fingerprint;
    state->storageLevel = bot->GetLevel();

    // The upgrades per place, the best first.
    struct Pick
    {
        float score = 0.0f;
        StorageItem const* item = nullptr;
    };
    std::map<SetPlace, std::vector<Pick>> candidates;
    for (StorageItem const& item : stock)
    {
        if ((item.itemClass != ITEM_CLASS_WEAPON && item.itemClass != ITEM_CLASS_ARMOR) || !item.amount)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.entry);
        SetPlace const group = PlaceOf(proto);
        if (group == PlaceNone || !IsRoleUpgrade(bot, item.entry, item.randomProperty))
            continue;
        candidates[group].push_back({ ScoreOf(bot, state->role, item.entry, item.randomProperty), &item });
    }

    // Levelling comes first: a piece somebody of the guild would wear for levelling (this bot too) stays for it.
    std::vector<Player*> members;
    {
        std::vector<ObjectGuid> characters = { guild->leader };
        for (auto const& [guid, member] : guild->bots)
            characters.push_back(guid);
        for (ObjectGuid const& guid : characters)
            if (Player* member = ObjectAccessor::FindPlayer(guid); member && member->IsInWorld() && BotDriver::GetAI(member))
                members.push_back(member);
    }
    auto forLevelling = [&](StorageItem const& item)
    {
        return std::any_of(members.begin(), members.end(), [&](Player* member)
        {
            return BotDriver::IsUpgrade(member, item.entry, item.randomProperty);
        });
    };

    std::string taken;
    uint32 count = 0;
    for (auto& [group, picks] : candidates)
    {
        if (count >= MaxStorageTakes)
            break;
        std::sort(picks.begin(), picks.end(), [](Pick const& a, Pick const& b) { return a.score > b.score; });
        for (std::size_t i = 0; i < picks.size() && i < 3; ++i)
        {
            Pick const& pick = picks[i];
            if (forLevelling(*pick.item))
                continue;
            std::string error;
            if (!sStorageService.Withdraw(bot, pick.item->entry, pick.item->randomProperty, 1, error))
                continue;
            // Straight on into the role store.
            for (Item* item : BagItems(bot))
                if (item->GetEntry() == pick.item->entry && item->GetItemRandomPropertyId() == pick.item->randomProperty &&
                    // (A never-worn bind-on-pickup piece comes out of the storage soulbound to the taker, with its mark.)
                    (!item->IsSoulBound() || sStorageService.IsMarkedUnworn(item)))
                {
                    Append(taken, item->GetTemplate()->Name1);
                    Stash(bot, *state, item, RolePieceDungeon, NoSlot);
                    ++count;
                    break;
                }
            break;
        }
    }
    if (count)
    {
        // The storage changed by this: the next look finds the next place.
        state->storageSeen = 0;
        Save(bot->GetGUID(), *state, bot);
    }
    return taken;
}

uint16 RoleService::RoleGearLevel(Player const* bot) const
{
    BotRole const* state = bot ? Find(bot->GetGUID()) : nullptr;
    if (!state || state->role == DungeonRole::None || state->worn)
        return bot ? Readiness::GearLevel(bot) : 0;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (state->gearLevelAt && getMSTimeDiff(state->gearLevelAt, now) < GearLevelCacheMs)
        return state->gearLevel;
    SetPlan const plan = PlanDungeonSet(bot, *state);
    state->gearLevelAt = now;
    return state->gearLevel = Readiness::GearLevelWith(bot, [&](uint8 slot) -> uint32
    {
        if (auto itr = plan.put.find(slot); itr != plan.put.end())
        {
            int32 const index = IndexOf(*state, itr->second);
            ItemTemplate const* proto = index >= 0 ? sObjectMgr->GetItemTemplate(state->store[index].entry) : nullptr;
            return proto ? proto->ItemLevel : 0;
        }
        if (slot == EQUIPMENT_SLOT_OFFHAND && plan.clearOffHand)
            return 0;
        Item const* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        return item ? item->GetTemplate()->ItemLevel : 0;
    });
}

std::string RoleService::ReadinessSummary(Player const* bot) const
{
    if (!bot || GetRole(bot->GetGUID()) == DungeonRole::None)
        return "";
    uint16 const gearLevel = RoleGearLevel(bot);
    std::string text;
    for (DungeonInfo const* dungeon : sReadiness.InReach(bot->GetLevel(), uint8(bot->GetTeamId())))
        text += Acore::StringFormat("{}{}:{}", text.empty() ? "" : ",", dungeon->id,
            uint32(Readiness::JudgeWith(bot, *dungeon, gearLevel).percent));
    return text;
}

std::vector<std::string> RoleService::Describe(AdventureGuild const& guild, GuildBot const& bot, Player* player) const
{
    std::vector<std::string> lines;
    BotRole const* state = Find(bot.guid);
    DungeonRole const role = state ? state->role : DungeonRole::None;
    std::string can;
    for (DungeonRole possible : RolesOf(guild.ownerAccount, bot.classId))
        Append(can, ToText(possible));
    SpecTemplate const* spec = RoleTemplate(guild.ownerAccount, bot.guid, bot.classId);
    lines.push_back(Acore::StringFormat("{}: dungeon role {}{} (its class can be: {}).", bot.name, ToText(role),
        spec ? Acore::StringFormat(", talents for a run \"{}\"{}", spec->name, state && state->templateId ? "" : " (the default)") :
        std::string(), can.empty() ? "damage only" : can));
    if (!state)
        return lines;
    lines.push_back(Acore::StringFormat("  It wears its {} set{}.", state->worn ? "dungeon" : "levelling",
        player ? Acore::StringFormat("; talent spec {} of {} is active ({})", uint32(player->GetActiveSpec()) + 1,
            uint32(player->GetSpecsCount()), SpecService::CurrentSummary(player)) : std::string()));
    if (player && role != DungeonRole::None)
    {
        lines.push_back(Acore::StringFormat("  Gear level in its dungeon set: {} (in what it wears now: {}).", uint32(RoleGearLevel(player)),
            uint32(Readiness::GearLevel(player))));
        uint16 const gearLevel = RoleGearLevel(player);
        for (DungeonInfo const* dungeon : sReadiness.InReach(player->GetLevel(), uint8(player->GetTeamId())))
        {
            if (dungeon->raid)
                continue;
            ReadinessVerdict const verdict = Readiness::JudgeWith(player, *dungeon, gearLevel);
            lines.push_back(Acore::StringFormat("  as {} in {} (id {}): {}%{}", ToText(role), dungeon->name, dungeon->id,
                uint32(verdict.percent), verdict.why.empty() ? "" : " (" + verdict.why + ")"));
        }
    }
    lines.push_back(Acore::StringFormat("  Role store: {} piece{}.", state->store.size(), state->store.size() == 1 ? "" : "s"));
    for (RolePiece const& piece : state->store)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(piece.entry);
        if (!piece.entry)
            lines.push_back(Acore::StringFormat("    [levelling] nothing in the {} slot before the run", SlotName(piece.slot)));
        else
            lines.push_back(Acore::StringFormat("    [{}] {} (item {}, item level {}{}{}){}", piece.kind == RolePieceDungeon ? "dungeon" :
                "levelling", NameOf(piece.entry), piece.entry, proto ? proto->ItemLevel : 0,
                piece.randomProperty ? Acore::StringFormat(", random property {}", piece.randomProperty) : std::string(),
                player && role != DungeonRole::None ? Acore::StringFormat(", worth {:.1f} for the role",
                    ScoreOf(player, role, piece.entry, piece.randomProperty)) : std::string(),
                piece.kind == RolePieceLevelling ? Acore::StringFormat(", worn in the {} slot", SlotName(piece.slot)) :
                piece.slot == RoleSlotUnworn ? std::string(", never worn") : std::string()));
    }
    return lines;
}

namespace
{
    class GuildAdventureRoleWorldScript : public WorldScript
    {
    public:
        GuildAdventureRoleWorldScript() : WorldScript("GuildAdventureRoleWorldScript", {
            WORLDHOOK_ON_STARTUP
        }) { }

        void OnStartup() override
        {
            if (IsModuleEnabled())
                sRoleService.Load();
        }
    };

    // The guild and the roster bot behind a character name (an error for the handler when it is none).
    bool FindBot(ChatHandler* handler, PlayerIdentifier const& target, AdventureGuild*& guild, GuildBot*& bot)
    {
        if (!IsModuleEnabled())
        {
            handler->SendErrorMessage("Guild Adventure is disabled (GuildAdventure.Enable = 0).", false);
            return false;
        }
        guild = sGuildAdventureMgr.FindGuildOfMember(target.GetGUID());
        bot = guild ? guild->FindBot(target.GetGUID()) : nullptr;
        if (!bot)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is no bot of an adventure guild.", target.GetName()), false);
            return false;
        }
        return true;
    }

    class GuildAdventureRoleCommandScript : public CommandScript
    {
    public:
        GuildAdventureRoleCommandScript() : CommandScript("GuildAdventureRoleCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable adminTable =
            {
                { "role", HandleSetRole, SEC_ADMINISTRATOR, Console::Yes },
                { "roleset", HandleRoleSet, SEC_ADMINISTRATOR, Console::Yes },
                { "rolescore", HandleRoleScore, SEC_ADMINISTRATOR, Console::Yes },
                { "dungeonrun", HandleDungeonRun, SEC_ADMINISTRATOR, Console::Yes },
                { "dungeonstop", HandleDungeonStop, SEC_ADMINISTRATOR, Console::Yes },
                { "dungeondone", HandleDungeonDone, SEC_ADMINISTRATOR, Console::Yes },
                { "dungeonskip", HandleDungeonSkip, SEC_ADMINISTRATOR, Console::Yes },
                { "dungeonitems", HandleDungeonItems, SEC_ADMINISTRATOR, Console::Yes }
            };
            static ChatCommandTable gaTable =
            {
                { "role", HandleRole, SEC_GAMEMASTER, Console::Yes },
                { "admin", adminTable }
            };
            static ChatCommandTable commandTable =
            {
                { "ga", gaTable }
            };
            return commandTable;
        }

        // .ga admin dungeonrun <dungeon id> <runs> <bot> [<bot> ...] [bosses | rooms | bigpulls]: what the addon's run window
        // starts (test aid): the bots become a group for the run and are taken into the dungeon; 0 runs = until it is
        // stopped. The first tank leads, else the first bot. The last word is the run window's mode (else the bosses).
        static bool HandleDungeonRun(ChatHandler* handler, uint32 dungeonId, uint32 runs, Tail names)
        {
            AdventureGuild* guild = nullptr;
            std::vector<ObjectGuid> bots;
            int32 mode = -1;
            for (std::string_view token : Acore::Tokenize(names, ' ', false))
            {
                if (token == "bosses" || token == "rooms" || token == "bigpulls" || token == "tribute")
                {
                    mode = token == "bosses" ? OrderService::ModeBosses : token == "rooms" ? OrderService::ModeRooms : token == "tribute" ? OrderService::ModeTribute : OrderService::ModeBigPulls;
                    continue;
                }
                ObjectGuid const guid = sCharacterCache->GetCharacterGuidByName(std::string(token));
                // (A bot of the guild, or its leader's own character, which can come along: OrderService::StartDungeonGroup.)
                AdventureGuild* its = guid ? sGuildAdventureMgr.FindGuildOfMember(guid) : nullptr;
                if (!its || (guild && its != guild))
                {
                    handler->SendErrorMessage(Acore::StringFormat("{} is no character of that guild.", token), false);
                    return false;
                }
                guild = its;
                bots.push_back(guid);
            }
            std::string error;
            OrderService::DungeonRunOptions options;
            options.hasRules = true;
            options.rules = OrderService::DefaultFightRules();
            options.rules.mode = uint8(std::max<int32>(mode, 0));
            if (!guild || !sOrderService.StartDungeonGroup(*guild, dungeonId, bots, ObjectGuid::Empty,
                    runs == 0 ? OrderService::DungeonRunsEndless : runs, error, false, mode >= 0 ? &options : nullptr))
            {
                handler->SendErrorMessage(Acore::StringFormat("Not started: {}.", error.empty() ? "name the bots of the group" : error), false);
                return false;
            }
            handler->PSendSysMessage("A group of {} goes into dungeon {}.", bots.size(), dungeonId);
            return true;
        }

        // .ga admin dungeonstop <bot that leads the run> [now]: the run that is going is the order's last, or it ends
        // at once.
        static bool HandleDungeonStop(ChatHandler* handler, PlayerIdentifier target, Optional<std::string> when)
        {
            AdventureGuild* guild = nullptr;
            GuildBot* bot = nullptr;
            if (!FindBot(handler, target, guild, bot))
                return false;
            std::string error;
            if (!sOrderService.StopDungeonRun(*guild, bot->guid, when && *when == "now", error))
            {
                handler->SendErrorMessage(Acore::StringFormat("Not stopped: {}.", error), false);
                return false;
            }
            handler->PSendSysMessage("{}: {}.", bot->name, when && *when == "now" ? "its run ends now" : "the run that is going is its last");
            return true;
        }

        // .ga admin dungeonskip <bot that leads the run> <boss number>: test aid: the run that is going inside its
        // dungeon goes on at that boss of its order; the bosses before it still alive are killed by the leader.
        static bool HandleDungeonSkip(ChatHandler* handler, PlayerIdentifier target, uint32 number)
        {
            AdventureGuild* guild = nullptr;
            GuildBot* bot = nullptr;
            if (!FindBot(handler, target, guild, bot))
                return false;
            std::string result;
            if (!sOrderService.SkipDungeonBosses(*guild, bot->guid, number, result))
            {
                handler->SendErrorMessage(Acore::StringFormat("Not skipped: {}.", result), false);
                return false;
            }
            handler->PSendSysMessage("{}: {}.", bot->name, result);
            return true;
        }

        // .ga admin dungeondone <character of the guild> [<dungeon id> <runs>]: the dungeons in their order for that
        // guild, with what it has finished and what is open; with a dungeon and a number, a test aid: the guild has
        // finished it that often (0: not at all).
        static bool HandleDungeonDone(ChatHandler* handler, PlayerIdentifier target, Optional<uint32> dungeonId, Optional<uint32> runs)
        {
            AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(target.GetGUID());
            if (!guild)
            {
                handler->SendErrorMessage(Acore::StringFormat("{} is in no adventure guild.", target.GetName()), false);
                return false;
            }
            if (dungeonId && runs)
                sOrderService.SetDungeonsFinished(*guild, *dungeonId, *runs);
            uint32 place = 0;
            for (uint32 id : sOrderService.DungeonOrder(*guild))
            {
                DungeonInfo const* info = sReadiness.Find(id);
                std::string need;
                bool const open = sOrderService.IsDungeonUnlocked(*guild, id, &need);
                handler->PSendSysMessage("{}. {} (id {}, level {}-{}): finished {} time{}, {}", ++place, info ? info->name : "?", id,
                    info ? uint32(info->minLevel) : 0u, info ? uint32(info->maxLevel) : 0u, sOrderService.GetDungeonsFinished(*guild, id),
                    sOrderService.GetDungeonsFinished(*guild, id) == 1 ? "" : "s",
                    open ? std::string("open") : "locked (" + need + " first)");
            }
            return true;
        }

        // .ga admin dungeonitems <character> [<item> [clear]]: what the guild storage of that character's guild holds
        // for a dungeon run (DungeonSupplies.h); with an item, a test aid: the character uses that kind out of the
        // storage now, as a run's group does (when it helps it and it lacks its effect); with "clear" the effect of
        // that item is taken off the character (so that a test can be repeated).
        static bool HandleDungeonItems(ChatHandler* handler, PlayerIdentifier target, Optional<uint32> item, Optional<std::string> what)
        {
            AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(target.GetGUID());
            if (!guild)
            {
                handler->SendErrorMessage(Acore::StringFormat("{} is in no adventure guild.", target.GetName()), false);
                return false;
            }
            if (item)
            {
                Player* player = target.GetConnectedPlayer();
                if (!player || !player->IsInWorld())
                {
                    handler->SendErrorMessage(Acore::StringFormat("{} is not in the world.", target.GetName()), false);
                    return false;
                }
                if (what && *what == "clear")
                {
                    SpellInfo const* info = nullptr;
                    if (DungeonSupplies::Classify(sObjectMgr->GetItemTemplate(*item), &info) == SupplyKind::None || !info)
                    {
                        handler->SendErrorMessage("That is no item a dungeon run uses.", false);
                        return false;
                    }
                    player->RemoveAurasDueToSpell(info->Id);
                    // (An elixir of the same kind, battle or guardian, keeps a character from using this one: off too.
                    // A probe run found its bots with another guardian elixir of an earlier round.)
                    std::vector<uint32> others;
                    for (SpellGroup group : { SPELL_GROUP_ELIXIR_BATTLE, SPELL_GROUP_ELIXIR_GUARDIAN })
                        if (sSpellMgr->IsSpellMemberOfSpellGroup(info->Id, group))
                            for (auto const& [spellId, application] : player->GetAppliedAuras())
                                if (sSpellMgr->IsSpellMemberOfSpellGroup(spellId, group))
                                    others.push_back(spellId);
                    for (uint32 spellId : others)
                        player->RemoveAurasDueToSpell(spellId);
                    handler->PSendSysMessage("{}: the effect of item {} is off{}.", target.GetName(), *item,
                        others.empty() ? "" : " (and another elixir of its kind)");
                    return true;
                }
                std::string const used = DungeonSupplies::Use(player, { *item });
                handler->PSendSysMessage("{}: {}.", target.GetName(), used.empty() ? "nothing used (it has the effect, it would not help it, "
                    "the storage has none, or it fights)" : "used " + used);
                return true;
            }
            static char const* const kinds[] = { "-", "elixir or flask", "scroll", "stone or oil for a weapon", "scope" };
            std::vector<SupplyItem> const list = DungeonSupplies::List(guild->ownerAccount);
            handler->PSendSysMessage("The guild storage holds {} kind{} of items a dungeon run can use:", list.size(), list.size() == 1 ? "" : "s");
            for (SupplyItem const& supply : list)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(supply.entry))
                    handler->PSendSysMessage("  {} x {} (item {}, {}, from level {})", supply.amount, proto->Name1, supply.entry,
                        kinds[std::min<uint8>(uint8(supply.kind), 4)], proto->RequiredLevel);
            return true;
        }

        // .ga role <character>: its dungeon role, its dungeon set and its role store.
        static bool HandleRole(ChatHandler* handler, PlayerIdentifier target)
        {
            AdventureGuild* guild = nullptr;
            GuildBot* bot = nullptr;
            if (!FindBot(handler, target, guild, bot))
                return false;
            for (std::string const& line : sRoleService.Describe(*guild, *bot, target.GetConnectedPlayer()))
                handler->SendSysMessage(line);
            return true;
        }

        // .ga admin role <bot> <none | tank | healer> [<talent template id>]
        static bool HandleSetRole(ChatHandler* handler, PlayerIdentifier target, std::string roleName, Optional<uint32> templateId)
        {
            AdventureGuild* guild = nullptr;
            GuildBot* bot = nullptr;
            if (!FindBot(handler, target, guild, bot))
                return false;
            DungeonRole role = DungeonRole::None;
            if (roleName == "tank")
                role = DungeonRole::Tank;
            else if (roleName == "healer" || roleName == "heal")
                role = DungeonRole::Healer;
            else if (roleName != "none")
            {
                handler->SendErrorMessage("The role is none, tank or healer.", false);
                return false;
            }
            std::string error;
            if (!sRoleService.SetRole(*guild, *bot, role, templateId.value_or(0), error))
            {
                handler->SendErrorMessage(Acore::StringFormat("Not set: {}.", error), false);
                return false;
            }
            for (std::string const& line : sRoleService.Describe(*guild, *bot, target.GetConnectedPlayer()))
                handler->SendSysMessage(line);
            return true;
        }

        // .ga admin roleset <bot> <on | off | look>: test aid. Puts the dungeon set and the role's talents on as a
        // dungeon run does, changes back, or makes the bot look at its bags for its dungeon set now.
        static bool HandleRoleSet(ChatHandler* handler, PlayerIdentifier target, std::string what)
        {
            AdventureGuild* guild = nullptr;
            GuildBot* bot = nullptr;
            if (!FindBot(handler, target, guild, bot))
                return false;
            Player* player = target.GetConnectedPlayer();
            if (!player || !player->IsInWorld() || !sGuildBotHolder.IsAIDriven(player->GetGUID()))
            {
                handler->SendErrorMessage(Acore::StringFormat("{} is not in the world as a bot.", target.GetName()), false);
                return false;
            }
            if (what == "on")
            {
                sRoleService.Hold(bot->guid, true);
                if (!sRoleService.EnterDungeonMode(*guild, *bot, player))
                    handler->SendSysMessage("Not now (it fights, casts or is dead).");
            }
            else if (what == "off")
            {
                if (!sRoleService.LeaveDungeonMode(*bot, player))
                    handler->SendSysMessage("Not now (it fights, casts or is dead).");
            }
            else if (what == "look")
            {
                sRoleService.LookSoon(bot->guid);
                sRoleService.Step(*guild, *bot, player, sRoleService.IsDungeonSetOn(bot->guid));
            }
            else
            {
                handler->SendErrorMessage("on, off or look.", false);
                return false;
            }
            for (std::string const& line : sRoleService.Describe(*guild, *bot, player))
                handler->SendSysMessage(line);
            return true;
        }

        // .ga admin rolescore <bot> <item> [<random property>]: what a kind of item is worth for the bot's role, and
        // whether its dungeon set would take it.
        static bool HandleRoleScore(ChatHandler* handler, PlayerIdentifier target, uint32 item, Optional<int32> randomProperty)
        {
            AdventureGuild* guild = nullptr;
            GuildBot* bot = nullptr;
            if (!FindBot(handler, target, guild, bot))
                return false;
            Player* player = target.GetConnectedPlayer();
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            if (!player || !proto)
            {
                handler->SendErrorMessage(player ? "No such item." : "The character is not in the world.", false);
                return false;
            }
            int32 const property = randomProperty.value_or(0);
            DungeonRole const role = sRoleService.GetRole(bot->guid);
            handler->SendSysMessage(Acore::StringFormat("{} for {}: as tank {:.1f}, as healer {:.1f}; its role is {}: {}; for levelling: {}.",
                proto->Name1, bot->name, BotDriver::RoleScore(player, 1, item, property), BotDriver::RoleScore(player, 2, item, property),
                ToText(role), sRoleService.IsRoleUpgrade(player, item, property) ? "its dungeon set would take it" :
                "nothing for its dungeon set", BotDriver::IsUpgrade(player, item, property) ? "an upgrade" : "no upgrade"));
            return true;
        }
    };
}

void AddGuildAdventureRoleScripts()
{
    new GuildAdventureRoleWorldScript();
    new GuildAdventureRoleCommandScript();
}
