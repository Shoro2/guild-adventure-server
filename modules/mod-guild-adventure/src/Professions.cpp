/*
 * Professions: see Professions.h.
 */

#include "Professions.h"
#include "Ammunition.h"
#include "QuestInteraction.h"
#include "GuildBotHolder.h"
#include "BotDriver.h"
#include "BotSettings.h"
#include "GuildBudget.h"
#include "CellImpl.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "DungeonRoles.h"
#include "DungeonSupplies.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GearUpkeep.h"
#include "GuildAdventureMgr.h"
#include "CraftOrders.h"
#include "GuildAdventureSettings.h"
#include "Item.h"
#include "Bag.h"
#include "LootMgr.h"
#include "Spell.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Storage/CraftService.h"
#include "Storage/LootService.h"
#include "Storage/StorageService.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Trainer.h"
#include "WorldIndex.h"
#include "World.h"
#include <algorithm>
#include <cmath>
#include <list>
#include <limits>
#include <optional>
#include <map>
#include <unordered_set>

using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";

    // Whether a character can put a piece on now, by every requirement of the item (the owner, 2026-11-13: [quote left out]; Flying Tiger Goggles ask for Engineering 100):
    // the core's own check for a kind of item (side, class and race, a profession and its rank, a spell, the level),
    // then the weapon or armour proficiency and the reputation, which that check leaves out.
    bool CanWearCrafted(Player const* wearer, ItemTemplate const* proto)
    {
        if (wearer->CanUseItem(proto) != EQUIP_ERR_OK)
            return false;
        if (uint32 const proficiency = proto->GetSkill(); proficiency && !wearer->GetSkillValue(proficiency))
            return false;
        return !proto->RequiredReputationFaction ||
            uint32(wearer->GetReputationRank(proto->RequiredReputationFaction)) >= proto->RequiredReputationRank;
    }

    // The tools a profession cannot be worked without (a pick, a knife, a hammer). A bot gets them with the
    // profession, like its spells: without the pick the mining spell fails at every vein. (Not the fishing pole: it
    // comes with a fishing trip, OrderService::StepFishing.)
    struct Tool
    {
        uint32 skill;
        uint32 item;
    };
    constexpr Tool Tools[] =
    {
        { SKILL_MINING, 2901 },         // Mining Pick
        { SKILL_SKINNING, 7005 },       // Skinning Knife
        { SKILL_BLACKSMITHING, 5956 },  // Blacksmith Hammer
        { SKILL_ENGINEERING, 5956 },    // Blacksmith Hammer
        { SKILL_ENGINEERING, 6219 },    // Arclight Spanner
        { SKILL_INSCRIPTION, 39505 },   // Virtuoso Inking Set
        { SKILL_JEWELCRAFTING, 20815 }, // Jeweler's Kit
    };

    constexpr char const* ActivityLogName = "module.ga.activity";

    // The permanent enchantment a recipe puts on an item; 0 for any other spell.
    uint32 EnchantOf(SpellInfo const* info)
    {
        if (!info || !(info->GetExplicitTargetMask() & TARGET_FLAG_ITEM_MASK))
            return 0;
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.IsEffect(SPELL_EFFECT_ENCHANT_ITEM) && effect.MiscValue > 0)
                return uint32(effect.MiscValue);
        return 0;
    }

    // The rod (or other tool) a recipe asks for is in the bags.
    bool HasTools(Player const* player, SpellInfo const* info)
    {
        for (uint8 i = 0; i < 2; ++i)
        {
            if (info->Totem[i] && !player->HasItemCount(info->Totem[i], 1))
                return false;
            if (info->TotemCategory[i] && !player->HasItemTotemCategory(info->TotemCategory[i]))
                return false;
        }
        return true;
    }

    // Every chest-type game object (nodes are chests) within a range of an object.
    class ChestInRange
    {
    public:
        ChestInRange(WorldObject const* object, float range) : _object(object), _range(range) { }
        bool operator()(GameObject* go) const { return go->GetGoType() == GAMEOBJECT_TYPE_CHEST && _object->IsWithinDist(go, _range, false); }

    private:
        WorldObject const* _object;
        float _range;
    };

    // The skill a spell belongs to (its first skill line), 0 for none.
    uint32 SkillOfSpell(uint32 spellId)
    {
        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        return bounds.first != bounds.second ? bounds.first->second->SkillLine : 0;
    }

    // Enchantments for the guild's gear (Professions::EnchantScrollForGuild, UseEnchantScroll): only on a piece of blue
    // quality or better and item level 60 or more (the owner, 2026-11-25; the item level a guild bot setting since
    // 2026-11-26, Professions.EnchantMinItemLevel).
    constexpr uint32 GuildEnchantQuality = ITEM_QUALITY_RARE;
    // How long a piece a scroll is being written for is left to that enchanter (the cast takes five seconds).
    constexpr uint32 ScrollClaimMs = 3 * MINUTE * IN_MILLISECONDS;

    bool WorthAGuildEnchant(Item const* item, uint32 minItemLevel)
    {
        if (!item)
            return false;
        ItemTemplate const* proto = item->GetTemplate();
        return proto->Quality >= GuildEnchantQuality && proto->ItemLevel >= minItemLevel;
    }

    // The recipe's enchantment takes on the piece, as the core's Spell::CheckItems asks: the piece's class, kind and
    // place on the body, its level at least the recipe's, and from a scroll no more than the recipe's highest.
    bool TakesEnchant(Item* item, SpellInfo const* info)
    {
        if (!item->IsFitToSpellRequirements(info))
            return false;
        ItemTemplate const* proto = item->GetTemplate();
        if (!info->HasAttribute(SPELL_ATTR2_ALLOW_LOW_LEVEL_BUFF) && (proto->RequiredLevel ? proto->RequiredLevel : proto->ItemLevel) < info->BaseLevel)
            return false;
        return !info->MaxLevel || proto->ItemLevel <= info->MaxLevel;
    }

    // The role an enchantment is chosen for: the dungeon role (tank 1, healer 2), else the talents (0).
    int8 EnchantRoleOf(Player const* player)
    {
        switch (sRoleService.GetRole(player->GetGUID()))
        {
            case DungeonRole::Tank:
                return 1;
            case DungeonRole::Healer:
                return 2;
            default:
                return 0;
        }
    }

    char const* EnchantRoleName(int8 role)
    {
        return role == 1 ? "its role as a tank" : role == 2 ? "its role as a healer" : "its talents";
    }

    // Clearly better: 5 % and half a point of the stat weights above what the piece has.
    bool EnchantBetter(float score, float have)
    {
        return score > have * 1.05f && score - have >= 0.5f;
    }

    // An enchantment's name ("+9 Stamina", "Crusader"); "none" for 0.
    std::string EnchantName(uint32 enchant)
    {
        if (!enchant)
            return "none";
        SpellItemEnchantmentEntry const* entry = sSpellItemEnchantmentStore.LookupEntry(enchant);
        return entry && entry->description[0] && *entry->description[0] ? std::string(entry->description[0]) : std::string("an enchantment");
    }

    // Every scroll of an enchanting recipe (the recipe written on a vellum): the scroll's item and the recipe. Built once.
    std::vector<std::pair<uint32, SpellInfo const*>> const& EnchantScrolls()
    {
        static std::vector<std::pair<uint32, SpellInfo const*>> scrolls;
        static bool built = false;
        if (!built)
        {
            built = true;
            for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
                for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
                {
                    if (proto.Spells[i].SpellId <= 0 || proto.Spells[i].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE ||
                        SkillOfSpell(uint32(proto.Spells[i].SpellId)) != SKILL_ENCHANTING)
                        continue;
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(uint32(proto.Spells[i].SpellId)); info && EnchantOf(info))
                    {
                        scrolls.emplace_back(entry, info);
                        break;
                    }
                }
        }
        return scrolls;
    }

    // The scrolls the guild storage holds above its reserves.
    std::vector<std::pair<uint32, SpellInfo const*>> StoredScrolls(uint32 owner)
    {
        std::vector<std::pair<uint32, SpellInfo const*>> stored;
        for (auto const& [entry, info] : EnchantScrolls())
            if (sStorageService.GetAmount(owner, entry, 0))
                stored.emplace_back(entry, info);
        return stored;
    }

    // The vellum an enchantment is written on: a weapon vellum for a weapon's, an armor vellum for an armour piece's; of
    // those whose level holds the recipe (as Spell::CheckItems: the vellum's level at least the recipe's) the first in
    // its bags or the guild storage, else the lowest. 0: none takes it.
    uint32 VellumFor(Player* player, uint32 owner, SpellInfo const* info)
    {
        static constexpr uint32 WeaponVellums[] = { 39349, 39350, 43146 }; // Weapon Vellum, II, III
        static constexpr uint32 ArmorVellums[] = { 38682, 37602, 43145 };  // Armor Vellum, II, III
        if (info->EquippedItemClass != ITEM_CLASS_WEAPON && info->EquippedItemClass != ITEM_CLASS_ARMOR)
            return 0;
        uint32 const* list = info->EquippedItemClass == ITEM_CLASS_WEAPON ? WeaponVellums : ArmorVellums;
        uint32 lowest = 0;
        for (uint8 i = 0; i < 3; ++i)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(list[i]);
            if (!proto || (proto->RequiredLevel ? proto->RequiredLevel : proto->ItemLevel) < info->BaseLevel)
                continue;
            if (player->HasItemCount(list[i], 1) || (owner && sStorageService.GetAmount(owner, list[i], 0)))
                return list[i];
            if (!lowest)
                lowest = list[i];
        }
        return lowest;
    }

    // Up to `max` of an item out of the character's bags into the guild storage (the same items: nothing is made or
    // lost on the way). Returns how many went.
    uint32 MoveIntoStorage(Player* player, uint32 owner, uint32 entry, uint32 max)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!owner || !proto || !max)
            return 0;
        uint32 moved = 0;
        auto move = [&](Item* item)
        {
            if (moved >= max || !item || item->GetEntry() != entry || StorageService::RefuseReason(item))
                return;
            uint32 take = std::min<uint32>(max - moved, item->GetCount());
            if (!sStorageService.Add(owner, proto, item->GetItemRandomPropertyId(), take))
                return;
            moved += take;
            player->DestroyItemCount(item, take, true);
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            move(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    move(bag->GetItemByPos(uint8(slot)));
        if (moved)
            sStorageService.Touch(player);
        return moved;
    }

    // The profession a trainer teaches: the skill most of its spells ask for, or the one its first-rank spell gives.
    uint32 SkillOfTrainer(Trainer::Trainer const& trainer)
    {
        std::unordered_map<uint32, uint32> votes;
        for (Trainer::Spell const& spell : trainer.GetSpells())
        {
            if (spell.ReqSkillLine)
            {
                ++votes[spell.ReqSkillLine];
                continue;
            }
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.SpellId);
            if (!info)
                continue;
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.IsEffect(SPELL_EFFECT_LEARN_SPELL))
                    if (uint32 const skill = SkillOfSpell(effect.TriggerSpell))
                        ++votes[skill];
        }
        uint32 best = 0, most = 0;
        for (auto const& [skill, count] : votes)
            if (count > most)
            {
                best = skill;
                most = count;
            }
        return best;
    }
}

Professions& Professions::Instance()
{
    static Professions instance;
    return instance;
}

void Professions::Load()
{
    uint32 const start = getMSTime();
    _teachers.clear();
    std::unordered_map<uint32, std::size_t> spells; // skill -> spells of its teacher so far
    for (auto const& [entry, creature] : *sObjectMgr->GetCreatureTemplates())
    {
        if (!(creature.npcflag & UNIT_NPC_FLAG_TRAINER))
            continue;
        Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(entry);
        // (A trainer with a requirement teaches a specialisation, not the profession.)
        if (!trainer || trainer->GetTrainerType() != Trainer::Type::Tradeskill || trainer->GetTrainerRequirement())
            continue;
        uint32 const skill = SkillOfTrainer(*trainer);
        if (!skill || !::IsProfessionSkill(skill) || trainer->GetSpells().size() <= spells[skill])
            continue;
        spells[skill] = trainer->GetSpells().size();
        _teachers[skill] = trainer;
    }
    // Which recipes make which item: the profession spells with a "create item" effect.
    _recipes.clear();
    _toolsMade.clear();
    std::unordered_set<uint32> focusKinds; // the objects recipes are made at
    for (uint32 spellId = 1; spellId < sSpellMgr->GetSpellInfoStoreSize(); ++spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || !CraftService::IsCraftRecipe(info) || !::IsProfessionSkill(SkillOfSpell(spellId)))
            continue;
        if (info->RequiresSpellFocus)
            focusKinds.insert(info->RequiresSpellFocus);
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.IsEffect(SPELL_EFFECT_CREATE_ITEM) && effect.ItemType)
            {
                std::vector<uint32>& recipes = _recipes[effect.ItemType];
                if (recipes.empty())
                    if (ItemTemplate const* made = sObjectMgr->GetItemTemplate(effect.ItemType); made && made->TotemCategory)
                        _toolsMade[made->TotemCategory].push_back(effect.ItemType);
                recipes.push_back(spellId);
            }
    }
    // The ammunition vendors sell for gold (what a hunter buys anyway: Ammo::BuyAt; AmmoForGuild makes nothing worse).
    _vendorAmmo.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT n.item FROM npc_vendor n JOIN item_template i ON i.entry = n.item "
        "WHERE i.class = {} AND n.ExtendedCost = 0", uint32(ITEM_CLASS_PROJECTILE)))
        do
            _vendorAmmo.push_back(result->Fetch()[0].Get<uint32>());
        while (result->NextRow());
    // Which herbs milling turns into which pigment (the tables name references).
    _pigmentHerbs.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT m.Entry, COALESCE(r.Item, m.Item) FROM milling_loot_template m "
        "LEFT JOIN reference_loot_template r ON m.Reference <> 0 AND r.Entry = m.Reference"))
        do
        {
            Field const* f = result->Fetch();
            if (sObjectMgr->GetItemTemplate(f[0].Get<uint32>()) && sObjectMgr->GetItemTemplate(f[1].Get<uint32>()))
                _pigmentHerbs[f[1].Get<uint32>()].push_back(f[0].Get<uint32>());
        } while (result->NextRow());
    // Where mining and herb nodes spawn, with the skill they ask for.
    _nodes.clear();
    _focus.clear();
    _nodeEntries.clear();
    // A node gives something to anyone who opens it; quest objects with a herb or mining lock only give their quest
    // item (a Plant Bundle, a Tear of Tilloa).
    std::unordered_set<uint32> freeLoot;
    if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT Entry FROM gameobject_loot_template WHERE QuestRequired = 0"))
        do
            freeLoot.insert(result->Fetch()[0].Get<uint32>());
        while (result->NextRow());
    std::size_t places = 0, focusPlaces = 0;
    for (auto const& [entry, info] : *sObjectMgr->GetGameObjectTemplates())
    {
        // Anvils, forges, fires: where recipes are made.
        if (info.type == GAMEOBJECT_TYPE_SPELL_FOCUS && focusKinds.count(info.spellFocus.focusId))
        {
            for (SpawnPoint const& spawn : sWorldIndex.GetGameObjectSpawns(entry))
            {
                _focus[spawn.mapId].push_back({ spawn, info.spellFocus.focusId, float(info.spellFocus.dist) });
                ++focusPlaces;
            }
            continue;
        }
        if (info.type != GAMEOBJECT_TYPE_CHEST)
            continue;
        LockEntry const* lock = sLockStore.LookupEntry(info.GetLockId());
        if (!lock)
            continue;
        uint16 skill = 0, required = 0;
        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
            if (lock->Type[i] == LOCK_KEY_SKILL && (lock->Index[i] == LOCKTYPE_MINING || lock->Index[i] == LOCKTYPE_HERBALISM))
            {
                skill = lock->Index[i] == LOCKTYPE_MINING ? SKILL_MINING : SKILL_HERBALISM;
                required = uint16(lock->Skill[i]);
            }
        if (!skill || !freeLoot.count(info.GetLootId()))
            continue;
        _nodeEntries.insert(entry);
        for (SpawnPoint const& spawn : sWorldIndex.GetGameObjectSpawns(entry))
        {
            _nodes[spawn.mapId].push_back({ spawn, skill, required });
            ++places;
        }
    }
    // What is fished where: the fishing loot of zones and areas, with one level of references.
    _fishZones.clear();
    std::unordered_map<uint32, std::vector<uint32>> references; // reference -> zones and areas that use it
    if (QueryResult result = WorldDatabase.Query("SELECT Entry, Item, Reference FROM fishing_loot_template"))
        do
        {
            Field* fields = result->Fetch();
            uint32 const zone = fields[0].Get<uint32>();
            if (int32 const reference = fields[2].Get<int32>())
                references[uint32(std::abs(reference))].push_back(zone);
            else
                _fishZones[fields[1].Get<uint32>()].push_back(zone);
        } while (result->NextRow());
    if (!references.empty())
        if (QueryResult result = WorldDatabase.Query("SELECT Entry, Item FROM reference_loot_template WHERE Reference = 0"))
            do
            {
                Field* fields = result->Fetch();
                auto used = references.find(fields[0].Get<uint32>());
                if (used == references.end())
                    continue;
                std::vector<uint32>& zones = _fishZones[fields[1].Get<uint32>()];
                for (uint32 zone : used->second)
                    if (std::find(zones.begin(), zones.end(), zone) == zones.end())
                        zones.push_back(zone);
            } while (result->NextRow());
    // The schools of fish: they mark water a bot can fish in.
    _pools.clear();
    std::size_t pools = 0;
    for (auto const& [entry, info] : *sObjectMgr->GetGameObjectTemplates())
        if (info.type == GAMEOBJECT_TYPE_FISHINGHOLE)
            for (SpawnPoint const& spawn : sWorldIndex.GetGameObjectSpawns(entry))
            {
                _pools[spawn.mapId].push_back(spawn);
                ++pools;
            }
    LOG_INFO("module", "mod-guild-adventure: professions: {} professions have a trainer to learn from, {} items have a recipe, {} "
        "places of gathering nodes, {} places to craft at, {} items are fished, {} schools of fish ({} ms)", _teachers.size(),
        _recipes.size(), places, focusPlaces, _fishZones.size(), pools, GetMSTimeDiffToNow(start));
}

std::vector<uint32> const& Professions::GetFishZones(uint32 itemId) const
{
    static std::vector<uint32> const none;
    auto itr = _fishZones.find(itemId);
    return itr != _fishZones.end() ? itr->second : none;
}

std::vector<SpawnPoint> const& Professions::GetFishingPools(uint32 mapId) const
{
    static std::vector<SpawnPoint> const none;
    auto itr = _pools.find(mapId);
    return itr != _pools.end() ? itr->second : none;
}

std::vector<FocusSpawn> const& Professions::GetFocusSpawns(uint32 mapId) const
{
    static std::vector<FocusSpawn> const none;
    auto itr = _focus.find(mapId);
    return itr != _focus.end() ? itr->second : none;
}

bool Professions::FocusNear(Player* player, uint32 focus)
{
    GameObject* found = nullptr;
    Acore::GameObjectFocusCheck check(player, focus);
    Acore::GameObjectSearcher<Acore::GameObjectFocusCheck> searcher(player, found, check);
    Cell::VisitObjects(player, searcher, player->GetVisibilityRange());
    return found != nullptr;
}

bool Professions::CanSkin(Player const* player, uint8 level)
{
    // Spell::CheckCast, SPELL_EFFECT_SKINNING.
    int32 const skill = player->GetSkillValue(SKILL_SKINNING);
    return skill > 0 && (skill < 100 ? (int32(level) - 10) * 10 : int32(level) * 5) <= skill;
}

bool Professions::SkinTeaches(Player const* player, uint8 level)
{
    // Spell::EffectSkinning: grey 100 points above what the creature's level asks for.
    int32 const required = level < 10 ? 0 : level < 20 ? (int32(level) - 10) * 10 : int32(level) * 5;
    return int32(player->GetPureSkillValue(SKILL_SKINNING)) < required + 100;
}

bool Professions::IsSkinKind(uint32 creatureEntry)
{
    CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(creatureEntry);
    return info && info->SkinLootId && info->GetRequiredLootSkill() == SKILL_SKINNING;
}

bool Professions::IsSkinnableCorpse(Player const* player, Creature const* creature)
{
    if (creature->IsAlive() || creature->IsPet() || !creature->isTappedBy(player) || !IsSkinKind(creature->GetEntry()) ||
        !CanSkin(player, creature->GetLevel()) || QuestInteraction::KeepsCorpse(player->GetGUID(), creature->GetEntry()))
        return false;
    if (creature->HasUnitFlag(UNIT_FLAG_SKINNABLE))
        return true;
    // Its loot is still in it: skinnable once that is taken (not a corpse that was skinned: its loot is the skin).
    return creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE) && creature->loot.loot_type != LOOT_SKINNING;
}

Creature* Professions::FindSkinnable(Player* player, float range)
{
    if (!player->HasSkill(SKILL_SKINNING))
        return nullptr;
    std::list<Creature*> corpses;
    player->GetDeadCreatureListInGrid(corpses, range);
    Creature* best = nullptr;
    float bestDistance = 0.0f;
    for (Creature* corpse : corpses)
    {
        if (!IsSkinnableCorpse(player, corpse))
            continue;
        float const distance = player->GetDistance(corpse);
        if (!best || distance < bestDistance)
        {
            best = corpse;
            bestDistance = distance;
        }
    }
    return best;
}

std::vector<NodeSpawn> const& Professions::GetNodeSpawns(uint32 mapId) const
{
    static std::vector<NodeSpawn> const none;
    auto itr = _nodes.find(mapId);
    return itr != _nodes.end() ? itr->second : none;
}

bool Professions::CanWear(Player const* wearer, ItemTemplate const* proto)
{
    return wearer && proto && CanWearCrafted(wearer, proto);
}

uint32 Professions::ZoneGatherFloor(uint32 mapId, uint32 zoneId, uint32 skill) const
{
    uint64 const key = (uint64(zoneId) << 16) | (skill & 0xFFFF);
    if (auto itr = _zoneFloors.find(key); itr != _zoneFloors.end())
        return itr->second;
    // Only what lies in the zone's map rectangle is looked up (the zone of a spawn is a terrain lookup).
    float x1 = 0.0f, y1 = 0.0f, x2 = 100.0f, y2 = 100.0f;
    Zone2MapCoordinates(x1, y1, zoneId);
    Zone2MapCoordinates(x2, y2, zoneId);
    float const minX = std::min(x1, x2), maxX = std::max(x1, x2), minY = std::min(y1, y2), maxY = std::max(y1, y2);
    uint32 floor = 0;
    auto consider = [&](SpawnPoint const& spawn, uint32 required)
    {
        required = std::max<uint32>(required, 1);
        if ((floor && required >= floor) || spawn.x < minX || spawn.x > maxX || spawn.y < minY || spawn.y > maxY)
            return;
        if (sWorldIndex.GetSpawnZone(spawn) == zoneId)
            floor = required;
    };
    if (skill == SKILL_SKINNING)
    {
        for (MobSpawn const& mob : sWorldIndex.GetMobSpawns(mapId))
            if (IsSkinKind(mob.spawn.entry))
                consider(mob.spawn, SkinSkillFor(mob.maxLevel));
    }
    else
        for (NodeSpawn const& node : GetNodeSpawns(mapId))
            if (node.skill == skill)
                consider(node.spawn, node.required);
    _zoneFloors[key] = floor;
    return floor;
}

uint32 Professions::ZoneGatherUsual(uint32 mapId, uint32 zoneId, uint32 skill) const
{
    uint64 const key = (uint64(zoneId) << 16) | (skill & 0xFFFF);
    if (auto itr = _zoneUsual.find(key); itr != _zoneUsual.end())
        return itr->second;
    float x1 = 0.0f, y1 = 0.0f, x2 = 100.0f, y2 = 100.0f;
    Zone2MapCoordinates(x1, y1, zoneId);
    Zone2MapCoordinates(x2, y2, zoneId);
    float const minX = std::min(x1, x2), maxX = std::max(x1, x2), minY = std::min(y1, y2), maxY = std::max(y1, y2);
    std::vector<uint32> asked;
    auto consider = [&](SpawnPoint const& spawn, uint32 required)
    {
        if (spawn.x < minX || spawn.x > maxX || spawn.y < minY || spawn.y > maxY)
            return;
        if (sWorldIndex.GetSpawnZone(spawn) == zoneId)
            asked.push_back(std::max<uint32>(required, 1));
    };
    if (skill == SKILL_SKINNING)
    {
        for (MobSpawn const& mob : sWorldIndex.GetMobSpawns(mapId))
            if (IsSkinKind(mob.spawn.entry))
                consider(mob.spawn, SkinSkillFor(mob.maxLevel));
    }
    else
        for (NodeSpawn const& node : GetNodeSpawns(mapId))
            if (node.skill == skill)
                consider(node.spawn, node.required);
    uint32 usual = 0;
    if (!asked.empty())
    {
        std::nth_element(asked.begin(), asked.begin() + asked.size() / 2, asked.end());
        usual = asked[asked.size() / 2];
    }
    _zoneUsual[key] = usual;
    return usual;
}

bool Professions::HasWork(GuildBot const& bot, Player const* player, uint32 skill)
{
    if (!player->HasSkill(skill))
        return false;
    uint32 target = 0;
    bool planned = false;
    for (std::size_t i = 0; i < 2; ++i)
        if (bot.professions.primary[i] == skill)
        {
            planned = true;
            target = bot.professions.target[i];
        }
    for (uint16 secondary : bot.professions.secondary)
        planned = planned || secondary == skill;
    return planned && player->GetPureSkillValue(skill) < (target ? target : 300u);
}

std::vector<uint32> const& Professions::RecipesFor(uint32 itemId) const
{
    static std::vector<uint32> const none;
    auto itr = _recipes.find(itemId);
    return itr != _recipes.end() ? itr->second : none;
}

std::vector<uint32> Professions::Planned(GuildBot const& bot)
{
    std::vector<uint32> skills;
    for (uint16 skill : bot.professions.primary)
        if (skill)
            skills.push_back(skill);
    for (uint16 skill : bot.professions.secondary)
        if (skill && std::find(skills.begin(), skills.end(), uint32(skill)) == skills.end())
            skills.push_back(skill);
    return skills;
}

uint32 Professions::LearnPlanned(GuildBot const& bot, Player* player) const
{
    uint32 learned = 0;
    for (uint32 skill : Planned(bot))
    {
        auto teacher = _teachers.find(skill);
        if (teacher == _teachers.end() || !teacher->second->IsTrainerValidForPlayer(player))
            continue;
        Trainer::Trainer const* trainer = teacher->second;
        uint32 here = 0;
        // A few passes: a rank opens the next recipes.
        for (uint32 pass = 0; pass < 3; ++pass)
        {
            std::vector<Trainer::Spell const*> spells;
            for (Trainer::Spell const& spell : trainer->GetSpells())
                if ((!spell.ReqSkillLine || spell.ReqSkillLine == skill) && trainer->CanTeachSpell(player, &spell))
                    spells.push_back(&spell);
            if (spells.empty())
                break;
            for (Trainer::Spell const* spell : spells)
            {
                if (!spell->IsCastable())
                    player->learnSpell(spell->SpellId, false);
                else if (sScriptMgr->OnPlayerCanLearnSpell(player, spell->SpellId))
                    player->CastSpell(player, spell->SpellId, true);
                ++here;
            }
        }
        // Its tools, once it has the profession. Out of the guild storage when one lies there; a new one only when
        // neither the character (bags, bank) nor the storage has one, and not more than once in half an hour per
        // character and tool. (A Jeweler's Kit is no tool by its item data, the recipes name it as an item: the bag sweep
        // put it into the guild storage, the next look here made a new one, once a minute: 117 kits in a guild's
        // storage after a night.)
        if (player->HasSkill(skill))
            for (Tool const& tool : Tools)
            {
                ItemTemplate const* proto = tool.skill == skill ? sObjectMgr->GetItemTemplate(tool.item) : nullptr;
                if (!proto || player->HasItemCount(tool.item, 1, true))
                    continue;
                constexpr uint32 ToolGapMs = 30 * MINUTE * IN_MILLISECONDS;
                static std::unordered_map<uint64, uint32> lastHandOut; // (character, tool) -> getMSTime(); world thread only
                uint32 const now = std::max<uint32>(getMSTime(), 1);
                uint32& last = lastHandOut[(uint64(player->GetGUID().GetCounter()) << 32) | tool.item];
                if (last && getMSTimeDiff(last, now) < ToolGapMs)
                    continue;
                last = now;
                uint32 const owner = sStorageService.GetOwnerOf(player);
                bool const stored = owner && sStorageService.GetAmount(owner, tool.item, 0) > 0 &&
                    sStorageService.Remove(owner, tool.item, 0, 1) == 1;
                if (player->AddItem(tool.item, 1))
                {
                    if (stored)
                        LOG_INFO(ActivityLog, "activity: {} took 1 x {} from the guild storage: the tool of its profession plan", bot.name,
                            proto->Name1);
                    else
                        LOG_INFO(ActivityLog, "activity: {} got its tool for the profession plan: {}", bot.name, proto->Name1);
                }
                else if (stored)
                    sStorageService.Add(owner, proto, 0, 1); // no room in its bags: back it goes
            }
        if (here)
        {
            SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill);
            LOG_INFO(ActivityLog, "activity: {} learned {} thing{} of {} where it stands (profession plan; skill {} of {})", bot.name, here,
                here == 1 ? "" : "s", line ? line->name[0] : "its profession", player->GetPureSkillValue(skill),
                player->GetPureMaxSkillValue(skill));
        }
        learned += here;
    }
    return learned;
}

namespace
{
    // The item a recipe makes, and how many of it a cast gives.
    uint32 MadeBy(SpellInfo const* info, uint32* perCast = nullptr);

    // How many casts of the recipe crafting for skill may make before the guild storage holds enough of its product:
    // ammunition up to Professions.AmmoStock, any other stackable consumable up to Professions.SkillCraftStock. 0 when
    // it holds that much already; no limit for anything else.
    // What the guild has of an item a crafter makes for it: the guild storage and the crafter's own bags. What it made
    // goes into the storage only when its crafting has ended, and the next look comes before that: a warlock made
    // 5 x Gift of Arthas three times in a row, each look counting the storage's 15, and 30 lay there against a stock of
    // 20 (2026-11-27).
    uint32 GuildStock(Player* player, uint32 owner, uint32 item)
    {
        return sStorageService.GetAmount(owner, item, 0) + player->GetItemCount(item, false);
    }

    uint32 CastsBelowStock(Player* player, uint32 owner, SpellInfo const* info)
    {
        uint32 perCast = 1;
        uint32 const product = MadeBy(info, &perCast);
        ItemTemplate const* proto = product ? sObjectMgr->GetItemTemplate(product) : nullptr;
        if (!owner || !proto || proto->GetMaxStackSize() <= 1)
            return std::numeric_limits<uint32>::max();
        uint32 cap = 0;
        if (proto->Class == ITEM_CLASS_PROJECTILE)
            cap = uint32(sBotSettings.Get(player, Setting::ProfessionsAmmoStock));
        // (Explosives are trade goods in this client: Rough Dynamite, Coarse Dynamite, the bombs.)
        else if (proto->Class == ITEM_CLASS_CONSUMABLE || (proto->Class == ITEM_CLASS_TRADE_GOODS && proto->SubClass == ITEM_SUBCLASS_EXPLOSIVES))
            cap = uint32(sBotSettings.Get(player, Setting::ProfessionsSkillCraftStock));
        if (!cap)
            return std::numeric_limits<uint32>::max();
        uint32 const have = GuildStock(player, owner, product);
        perCast = std::max<uint32>(perCast, 1);
        return have >= cap ? 0 : (cap - have + perCast - 1) / perCast;
    }

    uint32 MadeBy(SpellInfo const* info, uint32* perCast)
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (info->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM && info->Effects[i].ItemType)
            {
                if (perCast)
                    *perCast = uint32(std::max<int32>(1, info->Effects[i].CalcValue()));
                return info->Effects[i].ItemType;
            }
        return 0;
    }

    // How the bot gets to cast a recipe `casts` times. Piece: now. Material: a material is made first (`spell`,
    // `count` casts of it, `item`). Buy: a vendor's material is missing (`item`, `count`). Focus: it is made
    // elsewhere. None: not by itself (a material nobody sells and it cannot make, a tool it does not carry).
    // A material a vendor of the bot's own side (or a neutral one) sells. (Bolts of linen, wool and silk are sold by
    // one clothier, in Silvermoon: the Alliance's tailors counted them as bought and did nothing for hours.)
    bool Buyable(Player* player, uint32 entry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto || !proto->BuyPrice)
            return false;
        FactionTemplateEntry const* own = player->GetFactionTemplateEntry();
        for (SpawnPoint const& vendor : sWorldIndex.GetItemVendors(entry))
        {
            CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(vendor.entry);
            FactionTemplateEntry const* faction = creature ? sFactionTemplateStore.LookupEntry(creature->faction) : nullptr;
            if (faction && (!own || !faction->IsHostileTo(*own)))
                return true;
        }
        return false;
    }

    GearStep WayToCast(Player* player, SpellInfo const* info, uint32 casts, uint32 owner, std::unordered_map<uint32, uint32> const& makers,
        uint8 depth)
    {
        GearStep way;
        if (!HasTools(player, info) || player->HasSpellCooldown(info->Id))
            return way;
        GearStep first; // what comes first: making before buying, the place last
        auto rankOf = [](GearStep::Kind kind) { return kind == GearStep::Kind::Material ? 3 : kind == GearStep::Kind::Buy ? 2 : 1; };
        for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
        {
            if (info->Reagent[r] <= 0 || !info->ReagentCount[r])
                continue;
            uint32 const entry = uint32(info->Reagent[r]);
            uint32 const needed = info->ReagentCount[r] * casts;
            uint32 const have = player->GetItemCount(entry) + (owner ? sStorageService.GetAmount(owner, entry, 0) : 0);
            if (have >= needed)
                continue;
            GearStep lacks;
            // Making before buying: what the bot makes out of the guild storage itself is made.
            auto maker = makers.find(entry);
            if (maker != makers.end() && depth && maker->second != info->Id)
            {
                SpellInfo const* sub = sSpellMgr->GetSpellInfo(maker->second);
                uint32 perCast = 1;
                if (sub && MadeBy(sub, &perCast))
                {
                    uint32 const subCasts = (needed - have + perCast - 1) / perCast;
                    lacks = WayToCast(player, sub, subCasts, owner, makers, depth - 1);
                    if (lacks.kind == GearStep::Kind::Piece)
                    {
                        lacks.kind = GearStep::Kind::Material;
                        lacks.spell = sub->Id;
                        lacks.item = entry;
                        lacks.count = subCasts;
                    }
                }
            }
            if (lacks.kind == GearStep::Kind::None)
            {
                if (!Buyable(player, entry))
                    return way;
                lacks.kind = GearStep::Kind::Buy;
                lacks.item = entry;
                lacks.count = needed - have;
            }
            if (first.kind == GearStep::Kind::None || rankOf(lacks.kind) > rankOf(first.kind))
                first = lacks;
        }
        if (first.kind != GearStep::Kind::None)
            return first;
        if (info->RequiresSpellFocus && !Professions::FocusNear(player, info->RequiresSpellFocus))
        {
            way.kind = GearStep::Kind::Focus;
            way.focus = info->RequiresSpellFocus;
            return way;
        }
        // (What the crafting queue itself counts: the bags, the storage above its reserves.)
        if (CraftService::CraftableCount(player, info) < casts)
            return way;
        way.kind = GearStep::Kind::Piece;
        way.spell = info->Id;
        way.count = casts;
        return way;
    }

    // The item level of what a character has in the place a piece would go to (0: the place is bare).
    uint32 WornLevelFor(Player* who, ItemTemplate const* proto)
    {
        uint8 const slot = who->FindEquipSlot(proto, NULL_SLOT, true);
        if (slot == NULL_SLOT)
            return 0;
        Item const* worn = who->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        return worn ? worn->GetTemplate()->ItemLevel : 0;
    }
}

namespace
{
    // The chance of a skill point per cast, as the core gives it (Player::UpdateCraftSkill, CraftSkillGainChance):
    // orange up to the yellow threshold, then falling in a straight line to grey.
    float CraftChance(uint32 value, uint32 grey, uint32 yellow)
    {
        float const orange = float(sWorld->getIntConfig(CONFIG_SKILL_CHANCE_ORANGE)) / 100.0f;
        float const gray = float(sWorld->getIntConfig(CONFIG_SKILL_CHANCE_GREY)) / 100.0f;
        if (grey <= yellow)
            return value < grey ? orange : gray;
        if (value <= yellow)
            return orange;
        if (value >= grey)
            return gray;
        return gray + float(grey - value) * (orange - gray) / float(grey - yellow);
    }

    // Green begins halfway between yellow and grey (the client's colours).
    bool IsGreen(uint32 value, uint32 grey, uint32 yellow)
    {
        return value >= (grey + yellow) / 2;
    }

    // The tools (spell tool categories) the orange and yellow recipes and enchantments of the planned professions
    // lack. (Every enchanter of the guild stood at 150 to 154 with its Runed Copper Rod: what gives skill from there
    // asks for the Runed Silver Rod, and the green enchantments the copper rod still served gave a point in forty
    // casts, the dust disenchanted out of the guild storage for each.)
    std::unordered_set<uint32> LackedTools(Player* player, std::vector<uint32> const& planned, uint32 onlySkill)
    {
        std::unordered_set<uint32> lacked;
        for (auto const& [spellId, state] : player->GetSpellMap())
        {
            if (!state || state->State == PLAYERSPELL_REMOVED || !state->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || HasTools(player, info))
                continue;
            SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
            for (auto ability = bounds.first; ability != bounds.second; ++ability)
            {
                uint32 const skill = ability->second->SkillLine;
                uint32 const value = player->GetPureSkillValue(skill);
                if (std::find(planned.begin(), planned.end(), skill) == planned.end() || (onlySkill && skill != onlySkill) || !value ||
                    value >= ability->second->TrivialSkillLineRankHigh ||
                    IsGreen(value, ability->second->TrivialSkillLineRankHigh, ability->second->TrivialSkillLineRankLow))
                    continue;
                for (uint8 i = 0; i < 2; ++i)
                    if (info->TotemCategory[i] && !player->HasItemTotemCategory(info->TotemCategory[i]))
                        lacked.insert(info->TotemCategory[i]);
            }
        }
        return lacked;
    }

    // How many casts of a recipe make sense in one go: an orange or yellow one up to the change to green, as far as the
    // chance per cast goes (an orange recipe turns yellow on the way), a green one up to its next skill point, so that
    // the next look can take a recipe the bot has learned meanwhile, or stop. `top`: the skill's maximum now.
    uint32 CastsInColour(uint32 value, uint32 grey, uint32 yellow, uint32 top, uint32 count, uint32 batch)
    {
        float expected = 0.0f;
        uint32 const end = IsGreen(value, grey, yellow) ? value + 1 : std::min(top, (grey + yellow) / 2);
        for (uint32 v = value; v < end; ++v)
            expected += 1.0f / std::max(CraftChance(v, grey, yellow), 0.05f);
        uint32 const casts = std::max<uint32>(1, uint32(std::ceil(expected)));
        return std::max<uint32>(1, std::min({ count, std::max<uint32>(batch, 1), casts }));
    }
}

bool Professions::CraftForSkill(GuildBot const& bot, Player* player, uint32 batch, std::string& text, CraftNeed* need, uint32 notSkill,
    uint32* skillOut, uint32 onlySkill) const
{
    // The recipes are taken in this order (the owner, 2026-11-14: [quote left out]):
    // an orange or yellow one it can make now; an orange or yellow one whose lack it can see to by itself (a material
    // it makes, a vendor's, the place: not one creatures drop); a green one; never a grey one. Within that, a recipe
    // of another profession than `notSkill`, then the least material's worth per skill point. (A hunter took Heavy
    // Armor Kits on from orange through green, the cheapest per point by the chance the core gives, in batches of 20
    // past the change of colour, while the yellow Guardian Pants could be made: 66 kits, about half of them green.)
    struct Pick
    {
        uint32 spell = 0, count = 0, skill = 0, value = 0, grey = 0, yellow = 0, top = 0;
        bool green = false, other = false, tool = false;
        float score = 0.0f;
    };
    auto const better = [](bool green, bool other, float score, Pick const& than)
    {
        if (!than.spell || green != than.green)
            return !than.spell || !green;
        if (other != than.other)
            return other;
        return score < than.score;
    };
    Pick best;
    CraftNeed blocked, blockedTop;   // the best blocked recipe of any colour; the best orange or yellow one it can make
    Pick blockedPick, blockedTopPick;
    batch = std::max<uint32>(batch, 1);
    uint32 const owner = GetSettings().storageReagents ? sStorageService.GetOwnerOf(player) : 0;
    uint32 const stockOwner = GetSettings().storageEnable ? sStorageService.GetOwnerOf(player) : 0;
    std::vector<uint32> const planned = Planned(bot);
    // What the bot can make at all (any profession it has: a smith smelts with its Mining), by product: a material a
    // blocked recipe lacks that it makes itself comes first (`need`).
    std::unordered_map<uint32, uint32> makers;
    if (need)
        for (auto const& [makerId, makerState] : player->GetSpellMap())
        {
            if (!makerState || makerState->State == PLAYERSPELL_REMOVED || !makerState->Active)
                continue;
            SpellInfo const* maker = sSpellMgr->GetSpellInfo(makerId);
            if (maker && CraftService::IsCraftRecipe(maker))
                if (uint32 const product = MadeBy(maker))
                    makers.emplace(product, makerId);
        }
    // The tools its orange and yellow recipes (and enchantments) lack: a recipe that makes one counts as orange, the
    // tool comes first.
    std::unordered_set<uint32> const lackedTools = LackedTools(player, planned, onlySkill);
    // (A green recipe below this chance of a point a cast is not made at all.)
    float const greenMin = float(sBotSettings.Get(player, Setting::ProfessionsGreenMinChance)) / 100.0f;
    for (auto const& [spellId, state] : player->GetSpellMap())
    {
        if (!state || state->State == PLAYERSPELL_REMOVED || !state->Active)
            continue;
        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto ability = bounds.first; ability != bounds.second; ++ability)
        {
            uint32 const skill = ability->second->SkillLine;
            if (std::find(planned.begin(), planned.end(), skill) == planned.end() || (onlySkill && skill != onlySkill))
                continue;
            uint32 const value = player->GetPureSkillValue(skill);
            uint32 const grey = ability->second->TrivialSkillLineRankHigh;
            uint32 const yellow = ability->second->TrivialSkillLineRankLow;
            // Nothing to gain: the recipe is grey, or the skill is at the top of its rank.
            if (!value || value >= grey || value >= player->GetPureMaxSkillValue(skill))
                continue;
            // Smelting raises Mining while it is orange or yellow (the owner, 2026-11-14: [quote left out]); a green smelt gives a point one time in four and uses the guild's
            // ore for it. (Green begins halfway between yellow and grey: Player::UpdateCraftSkill.)
            if (skill == SKILL_MINING && value >= (grey + yellow) / 2)
                continue;
            // The target of a primary profession's plan is as far as the bot takes it.
            bool atTarget = false;
            for (std::size_t i = 0; i < 2; ++i)
                if (bot.professions.primary[i] == skill && bot.professions.target[i] && value >= bot.professions.target[i])
                    atTarget = true;
            if (atTarget)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || !CraftService::IsCraftRecipe(info))
                continue;
            // A recipe on its cooldown cannot be made now (a scribe's research once in twenty hours, an alchemist's
            // transmutation): a scribe "crafted" Minor Inscription Research fourteen times in twenty minutes, each
            // time with a walk to a fire, and nothing else got its turn.
            if (player->HasSpellCooldown(spellId))
                continue;
            // Not what the guild has plenty of: ammunition the storage holds Professions.AmmoStock of, a consumable it
            // holds Professions.SkillCraftStock of. Two death knights with Engineering 1 to 75 made 8400 Crafted Light
            // Shot for their skill in one round (round 174); nothing is sold or deleted, another recipe gets its turn.
            uint32 const stockRoom = stockOwner ? CastsBelowStock(player, stockOwner, info) : std::numeric_limits<uint32>::max();
            if (!stockRoom)
                continue;
            uint32 const count = CraftService::CraftableCount(player, info);
            if (!count && !need)
                continue;
            // What a skill point costs: the material's worth over the chance of a point.
            float cost = 1.0f;
            for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
                if (info->Reagent[r] > 0)
                    if (ItemTemplate const* item = sObjectMgr->GetItemTemplate(uint32(info->Reagent[r])))
                        cost += float(item->SellPrice) * float(info->ReagentCount[r]);
            float const chance = CraftChance(value, grey, yellow);
            float const score = cost / std::max(chance, 0.05f);
            ItemTemplate const* product = lackedTools.empty() ? nullptr : sObjectMgr->GetItemTemplate(MadeBy(info));
            bool const tool = product && product->TotemCategory && lackedTools.count(product->TotemCategory);
            bool const green = IsGreen(value, grey, yellow) && !tool;
            if (green && chance < greenMin)
                continue;
            bool const other = skill != notSkill;
            Pick const pick{ spellId, count, skill, value, grey, yellow, player->GetPureMaxSkillValue(skill), green, other, tool, score };
            // Made at an anvil, a forge, a fire: only where one stands.
            bool const placeHere = !info->RequiresSpellFocus || FocusNear(player, info->RequiresSpellFocus);
            // (A recipe whose tool is not in the bags cannot be cast: the scribe's inking set, a rod.)
            if (count && placeHere && HasTools(player, info))
            {
                if (better(green, other, score, best))
                    best = pick;
                continue;
            }
            if (!need || (!better(green, other, score, blockedPick) && (green || !better(green, other, score, blockedTopPick))))
                continue;
            // What it lacks, if the bot can get that by itself: the place, or materials vendors sell (the first of
            // them: the next look names the next).
            CraftNeed lacks;
            lacks.spell = spellId;
            bool possible = true;
            bool subBuy = false;   // the vendor's material is one a material it makes takes (counted already)
            uint32 matPer = 0, matPerCast = 1, matHave = 0, matLeast = 0; // the material made here: per cast, per make
            uint32 others = batch; // how often what is there of the other materials lets it be made
            for (uint8 r = 0; r < MAX_SPELL_REAGENTS && possible; ++r)
            {
                if (info->Reagent[r] <= 0 || !info->ReagentCount[r])
                    continue;
                uint32 const entry = uint32(info->Reagent[r]);
                uint32 const have = player->GetItemCount(entry) + (owner ? sStorageService.GetAmount(owner, entry, 0) : 0);
                if (have >= info->ReagentCount[r])
                {
                    others = std::min(others, have / info->ReagentCount[r]);
                    continue;
                }
                // A material it makes itself with a recipe it knows, whatever its colour for the skill that makes it:
                // bars out of the guild storage's ore, a bolt out of its cloth, bronze out of copper and tin (the owner,
                // 2026-11-14: [quote left out]; a smith with Mining stood at
                // Blacksmithing 123 beside 600 ore in the storage and mined on). Its own place comes first.
                ItemTemplate const* item = sObjectMgr->GetItemTemplate(entry);
                bool made = false;
                if (auto const maker = item ? makers.find(entry) : makers.end(); maker != makers.end() && maker->second != spellId)
                {
                    SpellInfo const* sub = sSpellMgr->GetSpellInfo(maker->second);
                    uint32 perCast = 1;
                    uint32 const least = sub && MadeBy(sub, &perCast) ? (info->ReagentCount[r] - have + perCast - 1) / perCast : 0;
                    GearStep const way = least ? WayToCast(player, sub, least, owner, makers, 1) : GearStep();
                    made = way.kind != GearStep::Kind::None;
                    switch (way.kind)
                    {
                        case GearStep::Kind::Piece:
                            if (!lacks.matSpell)
                            {
                                lacks.matSpell = sub->Id;
                                lacks.matItem = entry;
                                lacks.matCount = std::max(least, CraftService::CraftableCount(player, sub));
                                matPer = info->ReagentCount[r];
                                matPerCast = perCast;
                                matHave = have;
                                matLeast = least;
                            }
                            break;
                        case GearStep::Kind::Material:
                            if (!lacks.matSpell)
                            {
                                lacks.matSpell = way.spell;
                                lacks.matItem = way.item;
                                lacks.matCount = way.count;
                            }
                            break;
                        case GearStep::Kind::Focus:
                            if (!lacks.matFocus)
                                lacks.matFocus = way.focus;
                            break;
                        case GearStep::Kind::Buy:
                            if (!lacks.buyItem)
                            {
                                lacks.buyItem = way.item;
                                lacks.buyCount = way.count;
                                subBuy = true;
                            }
                            break;
                        default:
                            break;
                    }
                }
                // (Bars out of ore, a bolt out of cloth: making before buying, and a vendor only of its own side.)
                if (made)
                    continue;
                if (item && Buyable(player, entry))
                {
                    if (!lacks.buyItem)
                    {
                        lacks.buyItem = entry;
                        lacks.buyCount = info->ReagentCount[r]; // times `others`, below
                    }
                }
                // What the bot makes out of the guild storage itself: a scribe its pigments, an enchanter its dust
                // and essences.
                // (More than one of them: the enchanter's first rod takes dust and an essence, and with the second
                // counted as "nothing the bot can get" no enchanter ever made its rod.)
                else if (item && ((player->HasSkill(SKILL_INSCRIPTION) && IsPigment(entry)) ||
                    (player->HasSkill(SKILL_ENCHANTING) && sCraftOrders.IsDisenchantProduct(entry))))
                {
                    if (!lacks.makeItem)
                    {
                        lacks.makeItem = entry;
                        lacks.makeCount = info->ReagentCount[r] * batch - have;
                    }
                    else if (!lacks.makeItem2)
                        lacks.makeItem2 = entry;
                }
                // What creatures drop: the profession mode hunts for it.
                else if (item && WorldIndex::IsHuntMaterial(item) && sWorldIndex.HasCreatureSources(entry))
                {
                    if (!lacks.farmItem)
                    {
                        lacks.farmItem = entry;
                        lacks.farmCount = info->ReagentCount[r] * batch - have;
                    }
                }
                else
                    possible = false;
            }
            if (!possible || !HasTools(player, info))
                continue;
            if (lacks.buyItem && !subBuy)
                lacks.buyCount = lacks.buyCount * std::max<uint32>(others, 1) - std::min(player->GetItemCount(lacks.buyItem) +
                    (owner ? sStorageService.GetAmount(owner, lacks.buyItem, 0) : 0), lacks.buyCount * std::max<uint32>(others, 1) - 1);
            // As much of the material as the other materials let the recipe be made (a batch at most).
            if (matPer)
            {
                // (No more than the casts the guild storage takes up to its stock: round 174.)
                uint32 const casts = std::max<uint32>(std::min<uint32>(others, stockRoom), 1);
                lacks.matCount = std::max(matLeast, std::min(lacks.matCount,
                    (matPer * casts - std::min(matHave, matPer * casts - 1) + matPerCast - 1) / matPerCast));
            }
            if (!placeHere)
                lacks.focus = info->RequiresSpellFocus;
            // (The material's anvil or forge first, when nothing of it can be made here.)
            if (lacks.matFocus && !lacks.matSpell)
                lacks.focus = lacks.matFocus;
            if (better(green, other, score, blockedPick))
            {
                blocked = lacks;
                blockedPick = pick;
            }
            // (What creatures drop does not make a recipe one it can make: the hunt takes it away for long.)
            if (!green && !lacks.farmItem && better(green, other, score, blockedTopPick))
            {
                blockedTop = lacks;
                blockedTopPick = pick;
            }
        }
    }
    if (best.spell && best.green && need && blockedTop.spell)
    {
        // An orange or yellow recipe it can make once it has seen to what it lacks comes before a green one.
        *need = blockedTop;
        need->greenSpell = best.spell;
        return false;
    }
    if (!best.spell)
    {
        // (What it can see to by itself first: a recipe whose material creatures drop only when there is none.)
        if (need)
            *need = blockedTop.spell ? blockedTop : blocked;
        return false;
    }
    if (skillOut)
        *skillOut = best.skill;
    std::string error;
    // (A recipe with a cooldown is made once.)
    SpellInfo const* const info = sSpellMgr->GetSpellInfo(best.spell);
    // (A tool is made once.)
    uint32 casts = best.tool || (info && (info->RecoveryTime || info->CategoryRecoveryTime)) ? 1 :
        CastsInColour(best.value, best.grey, best.yellow, best.top, best.count, batch);
    // (And no more than the guild storage takes up to its stock: a batch of 20 casts of Crafted Light Shot is 4000
    // rounds.)
    if (info && GetSettings().storageEnable)
        casts = std::max<uint32>(1, std::min(casts, CastsBelowStock(player, sStorageService.GetOwnerOf(player), info)));
    if (!sCraftService.Start(player, best.spell, casts, error, text))
        return false;
    if (best.green && need)
        text += " (green: nothing orange or yellow can be made here)";
    return true;
}

bool Professions::LackedTool(GuildBot const& bot, Player* player, uint32 skill, std::vector<ToolNeed>& needs) const
{
    needs.clear();
    std::unordered_set<uint32> const lacked = LackedTools(player, Planned(bot), skill);
    if (lacked.empty())
        return false;
    uint32 const owner = sStorageService.GetOwnerOf(player);
    // What the bot makes itself, by product.
    std::unordered_map<uint32, uint32> makers;
    for (auto const& [spellId, state] : player->GetSpellMap())
        if (state && state->State != PLAYERSPELL_REMOVED && state->Active)
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId); info && CraftService::IsCraftRecipe(info))
                if (uint32 const product = MadeBy(info))
                    makers.emplace(product, spellId);
    // Each tool it lacks (an enchanter at 175 lacks the Runed Silver Rod and the Runed Golden Rod).
    auto const look = [&](uint32 category, std::vector<uint32> const& tools) -> std::optional<ToolNeed>
    {
        ToolNeed need;
        need.category = category;
        // One of that kind in the guild storage: it takes it out.
        for (uint32 tool : tools)
            if (owner && sStorageService.GetAmount(owner, tool, 0) > 0)
            {
                need.tool = tool;
                need.inStorage = true;
                return need;
            }
        // A recipe of its own for such a tool: what it lacks that only another profession makes (a material it can make,
        // buy, mill or disenchant itself it sees to: CraftForSkill).
        bool ownRecipe = false;
        for (uint32 tool : tools)
        {
            auto const maker = makers.find(tool);
            SpellInfo const* info = maker != makers.end() ? sSpellMgr->GetSpellInfo(maker->second) : nullptr;
            if (!info)
                continue;
            ownRecipe = true;
            for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
            {
                if (info->Reagent[r] <= 0 || !info->ReagentCount[r])
                    continue;
                uint32 const entry = uint32(info->Reagent[r]);
                uint32 const have = player->GetItemCount(entry) + (owner ? sStorageService.GetAmount(owner, entry, 0) : 0);
                if (have >= info->ReagentCount[r] || makers.count(entry) || Buyable(player, entry) ||
                    (player->HasSkill(SKILL_INSCRIPTION) && IsPigment(entry)) ||
                    (player->HasSkill(SKILL_ENCHANTING) && sCraftOrders.IsDisenchantProduct(entry)) || RecipesFor(entry).empty())
                    continue;
                need.tool = tool;
                need.part = entry;
                need.count = info->ReagentCount[r] - have;
                return need;
            }
        }
        // No recipe of its own for it: the tool itself, from another profession.
        if (!ownRecipe)
        {
            need.tool = need.part = tools.front();
            need.count = 1;
            return need;
        }
        return std::nullopt;
    };
    for (uint32 category : lacked)
        if (auto const made = _toolsMade.find(category); made != _toolsMade.end()) // (a vendor's tool: the plan sees to it)
            if (std::optional<ToolNeed> need = look(category, made->second))
                needs.push_back(*need);
    return !needs.empty();
}

bool Professions::CookForGuild(Player* player, std::vector<uint8> const& levels, uint32 stock, uint32 batch, std::string& text,
    uint32* focus) const
{
    constexpr uint32 FoodSpellCategory = 11;
    // A character eats the food with the highest required level it may eat; the kinds lie ten levels apart.
    constexpr uint32 FoodLevelsBelow = 12;
    if (focus)
        *focus = 0;
    uint32 const owner = GetSettings().storageEnable ? sStorageService.GetOwnerOf(player) : 0;
    if (!owner || !stock || levels.empty() || !player->HasSkill(SKILL_COOKING))
        return false;

    uint32 bestSpell = 0, bestCasts = 0, bestRank = 0, blockedRank = 0;
    batch = std::max<uint32>(batch, 1);
    for (auto const& [spellId, state] : player->GetSpellMap())
    {
        if (!state || state->State == PLAYERSPELL_REMOVED || !state->Active)
            continue;
        bool cooking = false;
        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto ability = bounds.first; ability != bounds.second; ++ability)
            cooking = cooking || ability->second->SkillLine == SKILL_COOKING;
        SpellInfo const* info = cooking ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!info || !CraftService::IsCraftRecipe(info) || player->HasSpellCooldown(spellId))
            continue;
        uint32 item = 0, perCast = 1;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS && !item; ++i)
            if (info->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM && info->Effects[i].ItemType)
            {
                item = info->Effects[i].ItemType;
                perCast = uint32(std::max<int32>(1, info->Effects[i].CalcValue()));
            }
        ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item) : nullptr;
        if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE || proto->Spells[0].SpellCategory != FoodSpellCategory ||
            !proto->Spells[0].SpellId || StorageService::RefuseReason(proto))
            continue;
        // Who eats it.
        uint32 eaters = 0;
        for (uint8 level : levels)
            if (proto->RequiredLevel <= level && uint32(level) <= proto->RequiredLevel + FoodLevelsBelow)
                ++eaters;
        uint32 const have = GuildStock(player, owner, item);
        if (!eaters || have >= stock)
            continue;
        uint32 const count = CraftService::CraftableCount(player, info);
        if (!count || !HasTools(player, info))
            continue;
        // (Food that leaves its eater "well fed" triggers that from the meal.)
        SpellInfo const* meal = sSpellMgr->GetSpellInfo(uint32(proto->Spells[0].SpellId));
        bool const buff = meal && meal->HasAura(SPELL_AURA_PERIODIC_TRIGGER_SPELL);
        uint32 const rank = (buff ? 100000u : 0u) + std::min<uint32>(eaters, 99) * 100 + proto->RequiredLevel + 1;
        if (info->RequiresSpellFocus && !FocusNear(player, info->RequiresSpellFocus))
        {
            if (focus && rank > blockedRank)
            {
                blockedRank = rank;
                *focus = info->RequiresSpellFocus;
            }
            continue;
        }
        if (rank > bestRank)
        {
            bestRank = rank;
            bestSpell = spellId;
            bestCasts = std::min({ batch, count, (stock - have + perCast - 1) / perCast });
        }
    }
    if (!bestSpell)
        return false;
    if (focus)
        *focus = 0;
    std::string error;
    return sCraftService.Start(player, bestSpell, bestCasts, error, text);
}

bool Professions::AmmoForGuild(Player* player, std::vector<Player*> const& shooters, uint32 stock, uint32 batch, std::string& text,
    uint32* focus, bool start) const
{
    if (focus)
        *focus = 0;
    uint32 const owner = GetSettings().storageEnable ? sStorageService.GetOwnerOf(player) : 0;
    if (!owner || !stock || shooters.empty())
        return false;

    // What the character makes, by product (materials first), and its recipes for ammunition.
    std::unordered_map<uint32, uint32> makers;
    std::vector<ItemTemplate const*> ammo;
    for (auto const& [spellId, state] : player->GetSpellMap())
    {
        if (!state || state->State == PLAYERSPELL_REMOVED || !state->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        uint32 const product = info && CraftService::IsCraftRecipe(info) ? MadeBy(info) : 0;
        if (!product)
            continue;
        makers.emplace(product, spellId);
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(product); proto && proto->Class == ITEM_CLASS_PROJECTILE)
            ammo.push_back(proto);
    }
    if (ammo.empty())
        return false;

    // Each shooter's kind: the best of the engineer's recipes it can use, when that is no worse than what a vendor sells
    // it (Ammo::BuyAt buys the vendors' best, and a hunter takes ammunition out of the storage only when it is no worse
    // than what it carries). A kind nobody shoots is not made: an engineer at 300 knows every bullet, the guild's
    // level-60 hunters shoot one of them. And none worse than the vendors' for them: an engineer with Engineering 63
    // made 4800 Crafted Light Shot (a level-5 bullet, 2 damage) "for" two level-60 hunters that carry Impact Shot (22),
    // round 171; Thorium Shells (17.5) are below that too.
    std::unordered_map<uint32, uint32> users;
    for (Player* shooter : shooters)
    {
        ItemTemplate const* best = nullptr;
        for (ItemTemplate const* proto : ammo)
            if (Ammo::Fits(shooter, proto) && (!best || Ammo::Better(proto, best)))
                best = proto;
        ItemTemplate const* bought = Ammo::VendorBest(shooter);
        if (best && (!bought || !Ammo::Better(bought, best)))
            ++users[best->ItemId];
    }
    std::vector<std::pair<uint32, uint32>> order(users.begin(), users.end());
    std::sort(order.begin(), order.end(), [](auto const& a, auto const& b)
    {
        if (a.second != b.second)
            return a.second > b.second;
        return Ammo::Better(sObjectMgr->GetItemTemplate(a.first), sObjectMgr->GetItemTemplate(b.first));
    });

    batch = std::max<uint32>(batch, 1);
    for (auto const& [item, shooting] : order)
    {
        uint32 const have = GuildStock(player, owner, item);
        if (have >= stock)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(makers[item]);
        uint32 perCast = 1;
        if (!info || !MadeBy(info, &perCast))
            continue;
        // As many casts as the materials give, up to the batch and the stock; with none, what a batch would take
        // (its materials are made first).
        uint32 casts = std::min(batch, (stock - have + perCast - 1) / perCast);
        if (uint32 const craftable = CraftService::CraftableCount(player, info))
            casts = std::min(casts, craftable);
        GearStep const way = WayToCast(player, info, casts, owner, makers, 2);
        if (way.kind == GearStep::Kind::Focus)
        {
            if (focus && !*focus)
                *focus = way.focus;
            if (!start)
                return true;
            continue;
        }
        if (way.kind != GearStep::Kind::Piece && way.kind != GearStep::Kind::Material)
            continue; // (a material only a vendor sells: nothing is bought for it)
        if (!start)
            return true;
        std::string error;
        std::string started;
        if (!sCraftService.Start(player, way.spell, way.count, error, started))
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
        text = Acore::StringFormat("{}{} ({} of the guild's hunters shoot {}; the guild storage and its bags hold {} of it)", started,
            way.kind == GearStep::Kind::Material ? " (a material first)" : "", shooting, proto ? proto->Name1 : "it", have);
        if (focus)
            *focus = 0;
        return true;
    }
    return false;
}

bool Professions::ConsumablesForGuild(Player* player, uint8 topLevel, uint32 stock, uint32 batch, std::string& text, GearStep& step,
    bool start, std::unordered_set<uint32> const* skip) const
{
    // Endgame: nothing more than this many item levels below the guild's highest level (Elixir of Ogre's Strength,
    // item level 30, Solid Sharpening Stone 35 and Scroll of Stamina IV 50 were what a level-60 group of the guild got
    // at a dungeon's start, round 229; Dense Sharpening Stone is 45, Elixir of the Mongoose 56).
    constexpr uint32 EndgameLevelsBelow = 15;
    step = GearStep();
    uint32 const owner = GetSettings().storageEnable ? sStorageService.GetOwnerOf(player) : 0;
    if (!owner || !stock || !topLevel)
        return false;

    // Whether the buff of an elixir, flask or scroll is one a fight uses (not detection, water breathing, a vision).
    auto fightBuff = [](SpellInfo const* info, uint32& key)
    {
        static constexpr AuraType Useful[] = { SPELL_AURA_MOD_STAT, SPELL_AURA_MOD_ATTACK_POWER, SPELL_AURA_MOD_RANGED_ATTACK_POWER,
            SPELL_AURA_MOD_DAMAGE_DONE, SPELL_AURA_MOD_HEALING_DONE, SPELL_AURA_MOD_RESISTANCE, SPELL_AURA_MOD_BASE_RESISTANCE,
            SPELL_AURA_MOD_INCREASE_HEALTH, SPELL_AURA_MOD_INCREASE_HEALTH_2, SPELL_AURA_MOD_INCREASE_ENERGY, SPELL_AURA_MOD_POWER_REGEN,
            SPELL_AURA_MOD_POWER_REGEN_PERCENT, SPELL_AURA_MOD_WEAPON_CRIT_PERCENT, SPELL_AURA_MOD_SPELL_CRIT_CHANCE,
            SPELL_AURA_MOD_HIT_CHANCE, SPELL_AURA_MOD_SPELL_HIT_CHANCE, SPELL_AURA_MOD_RATING, SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE };
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (info->Effects[i].Effect != SPELL_EFFECT_APPLY_AURA)
                continue;
            AuraType const aura = info->Effects[i].ApplyAuraName;
            if (std::find(std::begin(Useful), std::end(Useful), aura) == std::end(Useful))
                continue;
            key = (uint32(aura) << 16) | (uint32(info->Effects[i].MiscValue) & 0xFFFF);
            return true;
        }
        return false;
    };

    // What the character makes, by product (materials first), and the candidates by kind.
    struct Candidate
    {
        ItemTemplate const* proto = nullptr;
        SpellInfo const* recipe = nullptr;
        uint32 perCast = 1;
    };
    std::unordered_map<uint32, uint32> makers;
    std::map<std::pair<uint8, uint32>, std::vector<Candidate>> kinds;
    for (auto const& [spellId, state] : player->GetSpellMap())
    {
        if (!state || state->State == PLAYERSPELL_REMOVED || !state->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        uint32 perCast = 1;
        uint32 const product = info && CraftService::IsCraftRecipe(info) ? MadeBy(info, &perCast) : 0;
        if (!product)
            continue;
        makers.emplace(product, spellId);
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(product);
        // (Nothing that binds when it is made: the guild storage never takes it. A warlock made Firestones, bound and
        // unique, six times for a storage that stayed at 0, 2026-11-27.)
        if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE || StorageService::RefuseReason(proto) ||
            proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM ||
            proto->ItemLevel + EndgameLevelsBelow < topLevel || proto->RequiredLevel > topLevel || (skip && skip->count(product)))
            continue;
        // Nothing of a content beyond the guild's level: at the cap of 60 an Elixir of Healing Power (item level 61, a
        // recipe of the Burning Crusade whose vial only vendors in Outland sell) sent a druid into the profession mode,
        // and the mode found no vendor for it and walked it to an inn 2800 yd away (2026-11-27, 03:24).
        if (proto->ItemLevel > topLevel)
            continue;
        SpellInfo const* use = nullptr;
        SupplyKind const kind = DungeonSupplies::Classify(proto, &use);
        uint32 key = 0;
        uint8 group = 0;
        if (kind == SupplyKind::Elixir || kind == SupplyKind::Scroll)
        {
            if (!use || !fightBuff(use, key))
                continue;
            group = uint8(kind);
        }
        else if (kind == SupplyKind::WeaponCoat || kind == SupplyKind::Scope)
        {
            // The kind of a stone, a weightstone, an oil or a scope: the weapons it is for and what its enchantment does.
            uint32 enchant = 0;
            for (uint8 i = 0; use && i < MAX_SPELL_EFFECTS && !enchant; ++i)
                if (use->Effects[i].Effect == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY || use->Effects[i].Effect == SPELL_EFFECT_ENCHANT_ITEM)
                    enchant = uint32(use->Effects[i].MiscValue);
            SpellItemEnchantmentEntry const* entry = enchant ? sSpellItemEnchantmentStore.LookupEntry(enchant) : nullptr;
            if (!entry)
                continue;
            key = (uint32(use->EquippedItemSubClassMask) & 0xFFFFF) ^ (entry->type[0] << 24) ^ (entry->spellid[0] << 20);
            group = uint8(kind);
        }
        else if (proto->SubClass == ITEM_SUBCLASS_POTION)
        {
            // A healing or a mana potion (what a fight drinks: OrderService::StepPotion).
            SpellInfo const* drink = proto->Spells[0].SpellId > 0 ? sSpellMgr->GetSpellInfo(uint32(proto->Spells[0].SpellId)) : nullptr;
            if (!drink)
                continue;
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS && !key; ++i)
                if (drink->Effects[i].Effect == SPELL_EFFECT_HEAL)
                    key = 1;
                else if (drink->Effects[i].Effect == SPELL_EFFECT_ENERGIZE && drink->Effects[i].MiscValue == POWER_MANA)
                    key = 2;
            if (!key)
                continue;
            group = 10;
        }
        else
            continue;
        kinds[{ group, key }].push_back({ proto, info, perCast });
    }
    if (kinds.empty())
        return false;

    // Of each kind the best it can make: the highest item level first; a kind the storage holds enough of at that
    // level or above needs nothing; one whose best it cannot make (a Black Lotus missing) falls to the next.
    batch = std::max<uint32>(batch, 1);
    struct Choice
    {
        Candidate made;
        uint32 have = 0;
        GearStep way;
    };
    std::vector<Choice> choices;
    for (auto& [kind, candidates] : kinds)
    {
        std::sort(candidates.begin(), candidates.end(), [](Candidate const& a, Candidate const& b)
        {
            return a.proto->ItemLevel != b.proto->ItemLevel ? a.proto->ItemLevel > b.proto->ItemLevel : a.proto->ItemId > b.proto->ItemId;
        });
        for (Candidate const& candidate : candidates)
        {
            uint32 const have = GuildStock(player, owner, candidate.proto->ItemId);
            if (have >= stock)
                break; // the kind has enough at this level
            // (Fewer casts when the materials are not there for all: the look at the level cap asks for 5, the
            // profession mode for 20, and a mage was sent in for Wizard Oil again and again that the mode then found
            // no way to make, 2026-11-27.)
            GearStep way;
            for (uint32 casts = std::min(batch, (stock - have + candidate.perCast - 1) / candidate.perCast); casts; casts /= 2)
            {
                way = WayToCast(player, candidate.recipe, casts, owner, makers, 2);
                if (way.kind != GearStep::Kind::None)
                    break;
            }
            if (way.kind == GearStep::Kind::None)
                continue;
            choices.push_back({ candidate, have, way });
            break;
        }
    }
    if (choices.empty())
        return false;
    // What the storage has least of first; then what it makes here and now (no walk, no purchase).
    std::sort(choices.begin(), choices.end(), [](Choice const& a, Choice const& b)
    {
        auto rank = [](GearStep::Kind kind) { return kind == GearStep::Kind::Piece ? 0 : kind == GearStep::Kind::Material ? 1 : 2; };
        if (a.have != b.have)
            return a.have < b.have;
        return rank(a.way.kind) < rank(b.way.kind);
    });
    for (Choice const& choice : choices)
    {
        step = choice.way;
        step.gain = choice.made.proto->ItemId; // (which consumable the step is for)
        if (choice.way.kind != GearStep::Kind::Piece && choice.way.kind != GearStep::Kind::Material)
        {
            text = choice.made.proto->Name1;
            return true;
        }
        if (!start)
        {
            text = choice.made.proto->Name1;
            return true;
        }
        std::string error;
        std::string started;
        if (!sCraftService.Start(player, choice.way.spell, choice.way.count, error, started))
        {
            // (Said, so a look that sees a way and a craft that does not start can be told apart.)
            LOG_INFO(ActivityLog, "activity: {} could not start {} for the guild: {}", player->GetName(),
                choice.made.proto->Name1, error.empty() ? std::string("no reason given") : error);
            continue;
        }
        text = Acore::StringFormat("{}{} (for {}; the guild storage and its bags hold {} of it)", started,
            choice.way.kind == GearStep::Kind::Material ? " (a material first)" : "", choice.made.proto->Name1, choice.have);
        return true;
    }
    step = GearStep();
    return false;
}

bool Professions::ProcessForGuild(Player* player, uint32 above, uint32 stock, uint32 batch, std::string& text, GearStep& step,
    bool start) const
{
    step = GearStep();
    uint32 const owner = GetSettings().storageEnable ? sStorageService.GetOwnerOf(player) : 0;
    if (!owner || !above)
        return false;
    batch = std::max<uint32>(batch, 1);

    struct Choice
    {
        ItemTemplate const* proto = nullptr;
        SpellInfo const* recipe = nullptr;
        uint32 spare = 0;     // what lies above `above` of its scarcest raw material
        uint32 have = 0;
        GearStep way;
    };
    std::unordered_map<uint32, uint32> makers;
    std::vector<std::pair<SpellInfo const*, uint32>> recipes; // recipe, what one cast makes
    for (auto const& [spellId, state] : player->GetSpellMap())
    {
        if (!state || state->State == PLAYERSPELL_REMOVED || !state->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        uint32 perCast = 1;
        uint32 const product = info && CraftService::IsCraftRecipe(info) ? MadeBy(info, &perCast) : 0;
        if (!product)
            continue;
        makers.emplace(product, spellId);
        recipes.push_back({ info, perCast });
    }
    std::vector<Choice> choices;
    for (auto const& [info, perCast] : recipes)
    {
        uint32 const product = MadeBy(info);
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(product);
        if (!proto || proto->Class != ITEM_CLASS_TRADE_GOODS || StorageService::RefuseReason(proto))
            continue;
        // A material, not a thing made of one: no explosive or device. Retadk (60, an engineer) walked from Arathi into
        // Alterac for 15 minutes to buy Coarse Blasting Powder 1400 yd away, to turn the guild's surplus Linen Cloth
        // into Coarse Dynamite, round 257.
        if (proto->SubClass == ITEM_SUBCLASS_EXPLOSIVES || proto->SubClass == ITEM_SUBCLASS_DEVICES)
            continue;
        // Its raw materials: every one no vendor sells lies in the storage above `above`, and there is one.
        uint32 casts = UINT32_MAX;
        bool raw = false;
        bool fits = true;
        for (uint8 r = 0; r < MAX_SPELL_REAGENTS && fits; ++r)
        {
            if (info->Reagent[r] <= 0 || !info->ReagentCount[r])
                continue;
            uint32 const entry = uint32(info->Reagent[r]);
            if (Buyable(player, entry))
                continue;
            uint32 const stored = sStorageService.GetAmount(owner, entry, 0);
            if (stored <= above)
            {
                fits = false;
                continue;
            }
            raw = true;
            casts = std::min(casts, (stored - above) / info->ReagentCount[r]);
        }
        if (!fits || !raw || !casts || casts == UINT32_MAX)
            continue;
        // Not what breaks a material apart (a Greater Magic Essence into three lesser ones): with both above the line it
        // would go back and forth with the recipe that joins them.
        uint32 inputs = 0;
        for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
            if (info->Reagent[r] > 0)
                inputs += info->ReagentCount[r];
        if (perCast > inputs)
            continue;
        // Bars out of ore whatever their stock; anything else up to `stock`.
        bool const smelting = SkillOfSpell(info->Id) == SKILL_MINING;
        uint32 const have = GuildStock(player, owner, product);
        if (!smelting && stock)
        {
            if (have >= stock)
                continue;
            casts = std::min(casts, (stock - have + perCast - 1) / perCast);
        }
        // Not for a handful: whenever a miner's ore brought the storage one above the line, level-60 miners walked to a
        // forge to smelt one Thorium Bar ("1 of its scarcest material above 1000"), up to nine of them at once and 200
        // such walks in a day, round 257.
        constexpr uint32 ProcessMinCasts = 20;
        if (casts < ProcessMinCasts)
            continue;
        uint32 const surplus = casts;
        casts = std::min(casts, batch);
        GearStep const way = WayToCast(player, info, casts, owner, makers, 0);
        if (way.kind == GearStep::Kind::None)
            continue;
        // A walk to an anvil or forge only for a surplus worth it: a level-60 warrior walked 218 s from the Western
        // Plaguelands to the forge of Aerie Peak for the 30 Thorium Ore above the line and smelted 20 bars there
        // (2026-11-27, 03:22). Where the place is near or here the twenty do.
        constexpr uint32 ProcessWalkCasts = 100;
        if (way.kind == GearStep::Kind::Focus && surplus < ProcessWalkCasts)
            continue;
        uint32 spare = UINT32_MAX;
        for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
            if (info->Reagent[r] > 0 && info->ReagentCount[r] && !Buyable(player, uint32(info->Reagent[r])))
                spare = std::min(spare, sStorageService.GetAmount(owner, uint32(info->Reagent[r]), 0) - above);
        choices.push_back({ proto, info, spare, have, way });
    }
    if (choices.empty())
        return false;
    // The most to spare first; then what it makes here and now (no walk, no purchase).
    std::sort(choices.begin(), choices.end(), [](Choice const& a, Choice const& b)
    {
        auto rank = [](GearStep::Kind kind) { return kind == GearStep::Kind::Piece ? 0 : kind == GearStep::Kind::Material ? 1 : 2; };
        if (a.spare != b.spare)
            return a.spare > b.spare;
        return rank(a.way.kind) < rank(b.way.kind);
    });
    for (Choice const& choice : choices)
    {
        step = choice.way;
        step.gain = choice.proto->ItemId; // (which product the step is for)
        if (!start || (choice.way.kind != GearStep::Kind::Piece && choice.way.kind != GearStep::Kind::Material))
        {
            text = choice.proto->Name1;
            return true;
        }
        std::string error;
        std::string started;
        if (!sCraftService.Start(player, choice.way.spell, choice.way.count, error, started))
            continue;
        text = Acore::StringFormat("{} (for {}; the guild storage and its bags hold {} of it, and the storage {} of its scarcest material above {})", started,
            choice.proto->Name1, choice.have, choice.spare, above);
        return true;
    }
    step = GearStep();
    return false;
}

bool Professions::KeysForGuild(Player* player, uint32 batch, std::string& text, GearStep& step, bool start) const
{
    constexpr uint32 KeyStock = 10;
    step = GearStep();
    uint32 const owner = GetSettings().storageEnable ? sStorageService.GetOwnerOf(player) : 0;
    if (!owner || !player->HasSkill(SKILL_BLACKSMITHING))
        return false;
    struct KeyRecipe
    {
        int32 skill = 0;
        SpellInfo const* recipe = nullptr;
        ItemTemplate const* proto = nullptr;
        uint32 perCast = 1;
        uint32 demand = 0;
    };
    std::vector<KeyRecipe> keys;
    std::unordered_map<uint32, uint32> makers;
    for (auto const& [spellId, state] : player->GetSpellMap())
    {
        if (!state || state->State == PLAYERSPELL_REMOVED || !state->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        uint32 perCast = 1;
        uint32 const product = info && CraftService::IsCraftRecipe(info) ? MadeBy(info, &perCast) : 0;
        if (!product)
            continue;
        makers.emplace(product, spellId);
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(product);
        if (int32 const skill = LootService::KeySkill(proto); skill > 0 && player->CanUseItem(proto) == EQUIP_ERR_OK)
            keys.push_back({ skill, info, proto, std::max<uint32>(perCast, 1), 0 });
    }
    if (keys.empty())
        return false;
    std::sort(keys.begin(), keys.end(), [](KeyRecipe const& a, KeyRecipe const& b) { return a.skill < b.skill; });
    for (StorageItem const& stored : sStorageService.List(owner))
    {
        ItemTemplate const* proto = stored.amount ? sObjectMgr->GetItemTemplate(stored.entry) : nullptr;
        if (LootService::ContainerKindOf(proto) != LootService::ContainerKind::Locked)
            continue;
        int32 const need = LootService::PickLockSkill(proto);
        if (need < 0)
            continue;
        for (KeyRecipe& key : keys)
            if (key.skill >= need)
            {
                key.demand += stored.amount;
                break;
            }
    }
    for (KeyRecipe const& key : keys)
    {
        if (!key.demand)
            continue;
        uint32 const have = GuildStock(player, owner, key.proto->ItemId);
        uint32 const target = std::min<uint32>(key.demand, KeyStock);
        if (have >= target)
            continue;
        uint32 const casts = std::min<uint32>((target - have + key.perCast - 1) / key.perCast, std::max<uint32>(batch, 1));
        GearStep const way = WayToCast(player, key.recipe, casts, owner, makers, 0);
        if (way.kind == GearStep::Kind::None)
            continue;
        step = way;
        step.gain = key.proto->ItemId;
        if (!start || (way.kind != GearStep::Kind::Piece && way.kind != GearStep::Kind::Material))
        {
            text = key.proto->Name1;
            return true;
        }
        std::string error;
        std::string started;
        if (!sCraftService.Start(player, way.spell, way.count, error, started))
            continue;
        text = Acore::StringFormat("{} (for {}: {} lockboxes of the guild storage it opens, {} of the key there)", started,
            key.proto->Name1, key.demand, have);
        return true;
    }
    step = GearStep();
    return false;
}

std::vector<Professions::RecipeOffer> Professions::RecipeOffers(Player* player, VendorItemData const* items, Creature* vendor)
{
    std::vector<RecipeOffer> offers;
    if (!items || !sBotSettings.IsOn(player, Setting::ProfessionsBuyRecipes))
        return offers;
    uint32 const cap = std::min<uint32>(uint32(sBotSettings.Get(player, Setting::ProfessionsRecipeMaxPrice)), GuildBudget::Available(player));
    float const discount = vendor ? player->GetReputationPriceDiscount(vendor) : 1.0f;
    for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
    {
        VendorItem const* item = items->GetItem(slot);
        ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
        if (!proto || item->ExtendedCost || !proto->BuyPrice || proto->Class != ITEM_CLASS_RECIPE || !proto->RequiredSkill ||
            !player->HasSkill(proto->RequiredSkill))
            continue;
        // What it teaches.
        uint32 teaches = 0;
        for (auto const& spell : proto->Spells)
            if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_LEARN_SPELL_ID)
                teaches = uint32(spell.SpellId);
        if (!teaches || !sSpellMgr->GetSpellInfo(teaches) || player->HasSpell(teaches))
            continue;
        uint32 const price = uint32(float(proto->BuyPrice) * discount);
        if (price > cap || player->CanUseItem(proto) != EQUIP_ERR_OK)
            continue;
        if (vendor && item->maxcount && !vendor->GetVendorItemCurrentCount(item))
            continue;
        offers.push_back({ slot, proto, price, teaches });
    }
    return offers;
}

std::string Professions::LearnCarriedRecipes(Player* player)
{
    if (!player->IsAlive() || player->IsInCombat() || !sBotSettings.IsOn(player, Setting::ProfessionsBuyRecipes))
        return "";
    // What it carries and could learn (the bags are not changed while they are looked through).
    std::vector<std::pair<ItemTemplate const*, uint32>> found;
    auto look = [&](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto || proto->Class != ITEM_CLASS_RECIPE || !proto->RequiredSkill || !player->HasSkill(proto->RequiredSkill))
            return;
        uint32 teaches = 0;
        for (auto const& spell : proto->Spells)
            if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_LEARN_SPELL_ID)
                teaches = uint32(spell.SpellId);
        if (!teaches || !sSpellMgr->GetSpellInfo(teaches) || player->HasSpell(teaches) || player->CanUseItem(proto) != EQUIP_ERR_OK)
            return;
        for (auto const& [known, spell] : found)
            if (spell == teaches)
                return;
        found.emplace_back(proto, teaches);
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        look(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = player->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                look(bag->GetItemByPos(uint8(slot)));
    std::string learned;
    for (auto const& [proto, teaches] : found)
    {
        player->DestroyItemCount(proto->ItemId, 1, true);
        player->learnSpell(teaches, false);
        SkillLineEntry const* line = sSkillLineStore.LookupEntry(proto->RequiredSkill);
        learned += Acore::StringFormat("{}{} ({} {})", learned.empty() ? "" : ", ", proto->Name1, line ? line->name[0] : "skill",
            player->GetPureSkillValue(proto->RequiredSkill));
    }
    return learned;
}

bool Professions::SellsRecipe(Player* player, uint32 vendorEntry)
{
    VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(vendorEntry);
    return items && !RecipeOffers(player, items, nullptr).empty();
}

std::string Professions::BuyRecipesAt(Player* player, Creature* vendor)
{
    constexpr uint32 MaxRecipesPerVisit = 5;
    VendorItemData const* items = vendor ? vendor->GetVendorItems() : nullptr;
    if (!items || !player->IsAlive() || player->IsInCombat())
        return "";
    std::string bought;
    uint32 count = 0;
    for (RecipeOffer const& offer : RecipeOffers(player, items, vendor))
    {
        if (count >= MaxRecipesPerVisit)
            break;
        // (Two recipes of a list can teach the same thing.)
        if (player->HasSpell(offer.teaches))
            continue;
        GuildBudget::Grant const grant = GuildBudget::Request(player, offer.price);
        if (!grant.ok)
            continue;
        // (The core's return value only says whether the vendor's list changed: the bags say whether it was bought.)
        uint32 const before = player->GetItemCount(offer.proto->ItemId, true);
        player->BuyItemFromVendorSlot(vendor->GetGUID(), offer.slot, offer.proto->ItemId, 1, NULL_BAG, NULL_SLOT);
        if (player->GetItemCount(offer.proto->ItemId, true) <= before)
        {
            GuildBudget::Cancel(player, offer.price, grant);
            continue;
        }
        // Learned at once: the recipe is used up, as when a player reads it.
        player->DestroyItemCount(offer.proto->ItemId, 1, true);
        player->learnSpell(offer.teaches, false);
        ++count;
        SkillLineEntry const* line = sSkillLineStore.LookupEntry(offer.proto->RequiredSkill);
        bought += Acore::StringFormat("{}{} ({} {}; {}, {})", bought.empty() ? "" : ", ", offer.proto->Name1, line ? line->name[0] : "skill",
            player->GetPureSkillValue(offer.proto->RequiredSkill), GuildBudget::MoneyText(offer.price),
            grant.fromGuild ? "the guild bank paid " + GuildBudget::MoneyText(grant.fromGuild) : std::string("from its own money"));
    }
    return bought;
}

bool Professions::IsTool(uint32 itemId)
{
    for (Tool const& tool : Tools)
        if (tool.item == itemId)
            return true;
    return false;
}

uint32 Professions::LearnStoredRecipes(AdventureGuild const& guild) const
{
    uint32 const owner = guild.ownerAccount;
    if (!GetSettings().storageEnable)
        return 0;
    // The guild's characters the AI plays, in the world.
    std::vector<Player*> members;
    auto consider = [&](ObjectGuid guid)
    {
        Player* player = sGuildBotHolder.GetPlannedPlayer(guid);
        if (player && player->IsInWorld() && player->IsAlive() && !player->IsDuringRemoveFromWorld())
            members.push_back(player);
    };
    for (auto const& [guid, bot] : guild.bots)
        consider(guid);
    if (guild.leaderPlanned)
        consider(guild.leader);
    if (members.empty())
        return 0;

    // How many recipes of a profession a character knows (the tie-break).
    auto knownRecipes = [](Player* player, uint32 skill)
    {
        uint32 known = 0;
        for (auto const& [spellId, state] : player->GetSpellMap())
        {
            if (!state || state->State == PLAYERSPELL_REMOVED || !state->Active)
                continue;
            SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
            for (auto ability = bounds.first; ability != bounds.second; ++ability)
                if (ability->second->SkillLine == skill)
                {
                    ++known;
                    break;
                }
        }
        return known;
    };

    uint32 learned = 0;
    for (StorageItem const& stored : sStorageService.List(owner))
    {
        if (stored.itemClass != ITEM_CLASS_RECIPE || !stored.amount)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(stored.entry);
        if (!proto || !proto->RequiredSkill)
            continue;
        // What it teaches.
        uint32 teaches = 0;
        for (auto const& spell : proto->Spells)
            if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_LEARN_SPELL_ID)
                teaches = uint32(spell.SpellId);
        if (!teaches || !sSpellMgr->GetSpellInfo(teaches))
            continue;
        // Who can learn it now, the best first: the highest skill, then the most recipes known.
        std::vector<Player*> able;
        for (Player* member : members)
            if (!member->HasSpell(teaches) && member->HasSkill(proto->RequiredSkill) && member->CanUseItem(proto) == EQUIP_ERR_OK)
                able.push_back(member);
        if (able.empty())
            continue;
        uint32 const skill = proto->RequiredSkill;
        std::sort(able.begin(), able.end(), [&](Player* a, Player* b)
        {
            uint32 const sa = a->GetPureSkillValue(skill), sb = b->GetPureSkillValue(skill);
            if (sa != sb)
                return sa > sb;
            uint32 const ka = knownRecipes(a, skill), kb = knownRecipes(b, skill);
            if (ka != kb)
                return ka > kb;
            return a->GetGUID() < b->GetGUID();
        });
        SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill);
        uint32 copies = stored.amount;
        for (Player* member : able)
        {
            if (!copies || sStorageService.Remove(owner, stored.entry, stored.randomProperty, 1) != 1)
                break;
            --copies;
            member->learnSpell(teaches, false);
            LOG_INFO(ActivityLogName, "activity: {} took 1 x {} from the guild storage (a recipe it can learn) and learned it: {} ({} {}{})",
                member->GetName(), proto->Name1, sSpellMgr->GetSpellInfo(teaches)->SpellName[0], line ? line->name[0] : "skill",
                member->GetPureSkillValue(skill), able.size() > 1 ? Acore::StringFormat("; {} of the guild could have learned it", able.size()) :
                std::string());
            ++learned;
        }
    }
    return learned;
}

uint32 Professions::MillStored(uint32 owner, Player* player, uint32 pigment, uint32 count, uint32 maxStacks) const
{
    constexpr uint32 MillStack = 5; // what the Milling spell takes
    auto herbs = _pigmentHerbs.find(pigment);
    if (!owner || !player->HasSkill(SKILL_INSCRIPTION) || herbs == _pigmentHerbs.end())
        return 0;
    uint32 const skill = player->GetSkillValue(SKILL_INSCRIPTION);
    uint32 stacks = 0;
    for (uint32 herb : herbs->second)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(herb);
        if (!proto || !proto->HasFlag(ITEM_FLAG_IS_MILLABLE) || proto->RequiredSkillRank > skill)
            continue;
        while (stacks < maxStacks && sStorageService.GetAmount(owner, pigment, 0) < count && sStorageService.GetAmount(owner, herb, 0) >= MillStack)
        {
            if (sStorageService.Remove(owner, herb, 0, MillStack) < MillStack)
                break;
            // The table the Milling spell rolls on; the pigments never touch the scribe's bags.
            Loot loot;
            loot.FillLoot(herb, LootTemplates_Milling, player, true);
            std::string got;
            for (LootItem const& lootItem : loot.items)
                if (ItemTemplate const* made = sObjectMgr->GetItemTemplate(lootItem.itemid))
                {
                    sStorageService.Add(owner, made, 0, lootItem.count);
                    got += Acore::StringFormat("{}{} x {}", got.empty() ? "" : ", ", uint32(lootItem.count), made->Name1);
                }
            LOG_INFO(ActivityLogName, "activity: {} milled {} x {} from the guild storage for its Inscription: {} into the guild storage",
                player->GetName(), MillStack, proto->Name1, got.empty() ? "nothing" : got);
            ++stacks;
        }
    }
    return stacks;
}

bool Professions::EnchantForSkill(GuildBot const& bot, Player* player, uint32 batch, std::string& text, CraftNeed* need)
{
    constexpr uint32 Skill = SKILL_ENCHANTING;
    std::vector<uint32> const planned = Planned(bot);
    if (std::find(planned.begin(), planned.end(), Skill) == planned.end() || !player->HasSkill(Skill) || _enchanting.count(player->GetGUID()))
        return false;
    uint32 const value = player->GetPureSkillValue(Skill);
    if (!value || value >= player->GetPureMaxSkillValue(Skill))
        return false;
    for (std::size_t i = 0; i < 2; ++i)
        if (bot.professions.primary[i] == Skill && bot.professions.target[i] && value >= bot.professions.target[i])
            return false;
    uint32 const owner = GetSettings().storageReagents ? sStorageService.GetOwnerOf(player) : 0;

    // The enchantments its own recipes put on things: a piece that carries one of them got it from this bot.
    std::unordered_set<uint32> own;
    for (auto const& [spellId, state] : player->GetSpellMap())
        if (state && state->State != PLAYERSPELL_REMOVED && state->Active)
            if (uint32 const enchant = EnchantOf(sSpellMgr->GetSpellInfo(spellId)))
                own.insert(enchant);

    // Orange and yellow enchantments first, green ones only when no orange or yellow one can be made, never grey ones
    // (the owner, 2026-11-14, as in CraftForSkill), and no green one below the floor of chance.
    float const greenMin = float(sBotSettings.Get(player, Setting::ProfessionsGreenMinChance)) / 100.0f;
    uint32 bestSpell = 0, bestCount = 0, bestGrey = 0, bestYellow = 0;
    bool bestGreen = false;
    Item* bestItem = nullptr;
    float bestScore = 0.0f;
    CraftNeed blocked, blockedTop;
    float blockedScore = 0.0f, blockedTopScore = 0.0f;
    bool blockedGreen = false;
    for (auto const& [spellId, state] : player->GetSpellMap())
    {
        if (!state || state->State == PLAYERSPELL_REMOVED || !state->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!EnchantOf(info) || !HasTools(player, info))
            continue;
        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto ability = bounds.first; ability != bounds.second; ++ability)
        {
            if (ability->second->SkillLine != Skill)
                continue;
            uint32 const grey = ability->second->TrivialSkillLineRankHigh;
            uint32 const yellow = ability->second->TrivialSkillLineRankLow;
            if (value >= grey)
                continue;
            // A piece it wears that takes the enchantment and carries none, or one of its own.
            Item* target = nullptr;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END && !target; ++slot)
                if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                {
                    uint32 const carried = item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
                    if (item->IsFitToSpellRequirements(info) && item->GetTemplate()->ItemLevel >= info->BaseLevel &&
                        (!carried || own.count(carried)))
                        target = item;
                }
            if (!target)
                continue;
            float cost = 1.0f;
            for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
                if (info->Reagent[r] > 0)
                    if (ItemTemplate const* item = sObjectMgr->GetItemTemplate(uint32(info->Reagent[r])))
                        cost += float(std::max<uint32>(item->SellPrice, 1)) * float(info->ReagentCount[r]);
            float const chance = CraftChance(value, grey, yellow);
            float const score = cost / std::max(chance, 0.05f);
            bool const green = IsGreen(value, grey, yellow);
            if (green && chance < greenMin)
                continue;
            uint32 const count = CraftService::CraftableCount(player, info);
            if (count)
            {
                if (!bestSpell || (bestGreen && !green) || (bestGreen == green && score < bestScore))
                {
                    bestSpell = spellId;
                    bestCount = count;
                    bestItem = target;
                    bestScore = score;
                    bestGreen = green;
                    bestGrey = grey;
                    bestYellow = yellow;
                }
                continue;
            }
            bool const beatsBlocked = !blocked.spell || (blockedGreen && !green) || (blockedGreen == green && score < blockedScore);
            bool const beatsTop = !green && (!blockedTop.spell || score < blockedTopScore);
            if (!need || (!beatsBlocked && !beatsTop))
                continue;
            // What it lacks: dust and essences come out of storage pieces nobody wears, a vendor's material is bought.
            CraftNeed lacks;
            lacks.spell = spellId;
            bool possible = true;
            for (uint8 r = 0; r < MAX_SPELL_REAGENTS && possible; ++r)
            {
                if (info->Reagent[r] <= 0 || !info->ReagentCount[r])
                    continue;
                uint32 const entry = uint32(info->Reagent[r]);
                uint32 const have = player->GetItemCount(entry) + (owner ? sStorageService.GetAmount(owner, entry, 0) : 0);
                if (have >= info->ReagentCount[r])
                    continue;
                ItemTemplate const* item = sObjectMgr->GetItemTemplate(entry);
                if (item && Buyable(player, entry))
                {
                    if (!lacks.buyItem)
                    {
                        lacks.buyItem = entry;
                        lacks.buyCount = info->ReagentCount[r] * batch - have;
                    }
                }
                else if (item && sCraftOrders.IsDisenchantProduct(entry))
                {
                    if (!lacks.makeItem)
                    {
                        lacks.makeItem = entry;
                        lacks.makeCount = info->ReagentCount[r] * batch - have;
                    }
                    else if (!lacks.makeItem2)
                        lacks.makeItem2 = entry;
                }
                else
                    possible = false;
            }
            if (!possible)
                continue;
            if (beatsBlocked)
            {
                blocked = lacks;
                blockedScore = score;
                blockedGreen = green;
            }
            if (beatsTop)
            {
                blockedTop = lacks;
                blockedTopScore = score;
            }
        }
    }
    if (bestSpell && bestGreen && need)
    {
        // An orange or yellow enchantment whose dust, essences or vendor's material it can get comes first, and so
        // does any orange or yellow recipe of the profession (a rod it lacks, a wand: WorkOnProfessions looks at its
        // recipes next); the green enchantment is the last thing it does.
        *need = blockedTop;
        need->greenSpell = bestSpell;
        return false;
    }
    if (!bestSpell || !bestItem)
    {
        if (need)
            *need = blocked;
        return false;
    }
    EnchantJob job;
    job.spell = bestSpell;
    job.item = bestItem->GetGUID();
    job.left = CastsInColour(value, bestGrey, bestYellow, player->GetPureMaxSkillValue(Skill), bestCount, batch);
    job.skillBefore = value;
    _enchanting[player->GetGUID()] = job;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(bestSpell);
    text = Acore::StringFormat("{} x {} on its {}", job.left, info->SpellName[0], bestItem->GetTemplate()->Name1);
    return true;
}

bool Professions::EnchantScrollForGuild(Player* player, std::vector<Player*> const& wearers, std::string& text, uint32& lackVellum,
    bool start)
{
    lackVellum = 0;
    uint32 const owner = sStorageService.GetOwnerOf(player);
    if (!owner || !player->HasSkill(SKILL_ENCHANTING) || _enchanting.count(player->GetGUID()))
        return false;

    // The enchantments it can write on a vellum: a recipe of its own whose enchantment has a scroll, the rod in its bags.
    struct ScrollRecipe
    {
        SpellInfo const* info;
        uint32 enchant;
        uint32 scroll;
        uint32 vellum;
    };
    std::vector<ScrollRecipe> recipes;
    for (auto const& [spellId, state] : player->GetSpellMap())
    {
        if (!state || state->State == PLAYERSPELL_REMOVED || !state->Active || SkillOfSpell(spellId) != SKILL_ENCHANTING)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        uint32 const enchant = EnchantOf(info);
        if (!enchant || !HasTools(player, info))
            continue;
        uint32 scroll = 0;
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.IsEffect(SPELL_EFFECT_ENCHANT_ITEM) && effect.ItemType && sObjectMgr->GetItemTemplate(effect.ItemType))
                scroll = effect.ItemType;
        uint32 const vellum = scroll ? VellumFor(player, owner, info) : 0;
        // (What it cannot make now is no enchantment it has to offer: the best one it can make is what the piece gets.)
        if (!vellum || !CraftService::CraftableCount(player, info))
            continue;
        recipes.push_back({ info, enchant, scroll, vellum });
    }
    if (recipes.empty())
        return false;

    std::vector<std::pair<uint32, SpellInfo const*>> const stored = StoredScrolls(owner);
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    std::erase_if(_scrollClaims, [now](auto const& claim) { return static_cast<int32>(claim.second.second - now) <= 0; });

    struct Choice
    {
        Player* wearer = nullptr;
        Item* item = nullptr;
        uint8 slot = 0;
        ScrollRecipe const* recipe = nullptr;
        float gain = 0.0f;
        int8 role = 0;
    };
    Choice best, lacking;
    for (Player* wearer : wearers)
    {
        if (!wearer || !wearer->IsInWorld())
            continue;
        int8 const role = EnchantRoleOf(wearer);
        uint32 const minItemLevel = uint32(sBotSettings.Get(wearer, Setting::ProfessionsEnchantMinItemLevel));
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = wearer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!WorthAGuildEnchant(item, minItemLevel))
                continue;
            if (auto const claim = _scrollClaims.find({ wearer->GetGUID(), slot });
                claim != _scrollClaims.end() && claim->second.first != player->GetGUID())
                continue;
            // What it has: the enchantment on the piece, or a better scroll for it in the guild storage.
            float have = BotDriver::EnchantScore(wearer, role, item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT), item);
            for (auto const& [entry, info] : stored)
                if (TakesEnchant(item, info))
                    have = std::max(have, BotDriver::EnchantScore(wearer, role, EnchantOf(info), item));
            for (ScrollRecipe const& recipe : recipes)
            {
                if (!TakesEnchant(item, recipe.info))
                    continue;
                float const score = BotDriver::EnchantScore(wearer, role, recipe.enchant, item);
                if (!EnchantBetter(score, have))
                    continue;
                bool const hasVellum = player->HasItemCount(recipe.vellum, 1) || sStorageService.GetAmount(owner, recipe.vellum, 0);
                Choice& into = hasVellum ? best : lacking;
                if (score - have > into.gain)
                    into = { wearer, item, slot, &recipe, score - have, role };
            }
        }
    }
    if (lacking.recipe)
        lackVellum = lacking.recipe->vellum;
    if (!best.recipe)
        return false;

    ItemTemplate const* scrollProto = sObjectMgr->GetItemTemplate(best.recipe->scroll);
    uint32 const before = best.item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
    text = Acore::StringFormat("{} for {}'s {} ({} instead of {}, the best it can make for {})", scrollProto->Name1, best.wearer->GetName(),
        best.item->GetTemplate()->Name1, EnchantName(best.recipe->enchant), EnchantName(before), EnchantRoleName(best.role));
    if (!start)
        return true;

    // A vellum out of the guild storage into its bags, with a line (the storage match of round 232 found 14 Armor and 6
    // Weapon Vellums gone without one: the vellum is the spell's target, not a reagent, so no reagent line names it).
    if (!player->HasItemCount(best.recipe->vellum, 1))
    {
        std::string error;
        if (!sStorageService.Withdraw(player, best.recipe->vellum, 0, 1, error))
            return false;
        ItemTemplate const* vellumProto = sObjectMgr->GetItemTemplate(best.recipe->vellum);
        LOG_INFO(ActivityLogName, "activity: {} used 1 x {} from the guild storage: it writes {} on it for the guild", player->GetName(),
            vellumProto ? vellumProto->Name1 : "a vellum", scrollProto->Name1);
    }
    Item* vellum = player->GetItemByEntry(best.recipe->vellum);
    if (!vellum)
        return false;
    EnchantJob job;
    job.spell = best.recipe->info->Id;
    job.item = vellum->GetGUID();
    job.left = 1;
    job.skillBefore = player->GetPureSkillValue(SKILL_ENCHANTING);
    job.vellum = true;
    job.vellumEntry = best.recipe->vellum;
    job.scroll = best.recipe->scroll;
    job.enchant = best.recipe->enchant;
    job.before = before;
    job.wearer = best.wearer->GetGUID();
    job.slot = best.slot;
    job.text = text;
    _enchanting[player->GetGUID()] = job;
    _scrollClaims[{ best.wearer->GetGUID(), best.slot }] = { player->GetGUID(), now + ScrollClaimMs };
    return true;
}

bool Professions::UseEnchantScroll(Player* player, std::string& text)
{
    uint32 const owner = sStorageService.GetOwnerOf(player);
    if (!owner || !BotDriver::GetAI(player) || _enchanting.count(player->GetGUID()) || !player->IsAlive() || player->IsInCombat())
        return false;
    std::vector<std::pair<uint32, SpellInfo const*>> const stored = StoredScrolls(owner);
    if (stored.empty())
        return false;
    int8 const role = EnchantRoleOf(player);
    Item* bestItem = nullptr;
    uint32 bestScroll = 0;
    SpellInfo const* bestInfo = nullptr;
    float bestGain = 0.0f;
    uint32 const minItemLevel = uint32(sBotSettings.Get(player, Setting::ProfessionsEnchantMinItemLevel));
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!WorthAGuildEnchant(item, minItemLevel))
            continue;
        float const have = BotDriver::EnchantScore(player, role, item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT), item);
        for (auto const& [entry, info] : stored)
        {
            if (!TakesEnchant(item, info))
                continue;
            float const score = BotDriver::EnchantScore(player, role, EnchantOf(info), item);
            if (EnchantBetter(score, have) && score - have > bestGain)
            {
                bestItem = item;
                bestScroll = entry;
                bestInfo = info;
                bestGain = score - have;
            }
        }
    }
    if (!bestItem)
        return false;
    if (!player->HasItemCount(bestScroll, 1))
    {
        std::string error;
        if (!sStorageService.Withdraw(player, bestScroll, 0, 1, error))
            return false;
    }
    Item* scroll = player->GetItemByEntry(bestScroll);
    if (!scroll)
        return false;
    EnchantJob job;
    job.spell = bestInfo->Id;
    job.item = bestItem->GetGUID();
    job.left = 1;
    job.apply = true;
    job.castItem = scroll->GetGUID();
    job.scroll = bestScroll;
    job.enchant = EnchantOf(bestInfo);
    job.before = bestItem->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
    text = Acore::StringFormat("{} on its {} ({} instead of {}, for {})", scroll->GetTemplate()->Name1, bestItem->GetTemplate()->Name1,
        EnchantName(job.enchant), EnchantName(job.before), EnchantRoleName(role));
    job.text = text;
    _enchanting[player->GetGUID()] = job;
    return true;
}

std::string Professions::DescribeEnchanting(ObjectGuid character) const
{
    auto itr = _enchanting.find(character);
    if (itr == _enchanting.end())
        return {};
    if (itr->second.vellum)
        return "Enchanting a vellum for the guild: " + itr->second.text;
    if (itr->second.apply)
        return "Putting a scroll of the guild storage on its gear: " + itr->second.text;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(itr->second.spell);
    return Acore::StringFormat("Enchanting its own gear for skill: {} ({} of {})", info ? info->SpellName[0] : "an enchantment",
        itr->second.done, itr->second.done + itr->second.left);
}

bool Professions::StepEnchanting(Player* player)
{
    auto itr = _enchanting.find(player->GetGUID());
    if (itr == _enchanting.end())
        return false;
    EnchantJob& job = itr->second;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (player->IsNonMeleeSpellCast(false) || (job.nextAt && now < job.nextAt))
        return true;

    SpellInfo const* info = sSpellMgr->GetSpellInfo(job.spell);
    Item* item = player->GetItemByGuid(job.item);
    Item* scroll = job.apply ? player->GetItemByGuid(job.castItem) : nullptr;
    char const* why = nullptr;
    if (!player->IsAlive() || player->IsInCombat())
        why = "a fight";
    else if (!job.left)
        why = "done";
    else if (!info || !item || (!job.vellum && !item->IsEquipped()))
        why = "the piece is gone";
    else if (job.apply && !scroll)
        why = "the scroll left its bags before it was used";
    else if (!job.apply && !CraftService::CraftableCount(player, info))
        why = "out of materials";
    if (!why)
    {
        if (player->isMoving())
            player->StopMoving();
        // (Not from the saddle, nor in an animal form: "cannot cast Enchant Bracer ... (result 64)" is "not while
        // mounted".)
        if (player->IsMounted())
        {
            player->Dismount();
            player->RemoveAurasByType(SPELL_AURA_MOUNTED);
        }
        if (player->GetShapeshiftForm() != FORM_NONE && (player->getClass() == CLASS_DRUID || player->GetShapeshiftForm() == FORM_GHOSTWOLF))
            player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
        Spell* spell = new Spell(player, info, TRIGGERED_NONE);
        if (scroll)
        {
            // As the core's Player::CastItemUseSpell: the scroll is the cast item (no reagents, no rod), used up by the
            // cast.
            spell->m_CastItem = scroll;
            spell->m_cast_count = 1;
        }
        SpellCastTargets targets;
        targets.SetItemTarget(item);
        SpellCastResult const result = spell->prepare(&targets);
        if (result == SPELL_CAST_OK)
        {
            --job.left;
            ++job.done;
            job.nextAt = now + uint32(std::max<int32>(info->CalcCastTime(player), 0)) + 1500;
            return true;
        }
        // Refused because it was moving (its AI moved it again between the stop and the cast): held still, tried again.
        // (Healerino's scroll went back to the guild storage for that, result 51, round 232.)
        constexpr uint8 MaxMovingTries = 3;
        if (result == SPELL_FAILED_MOVING && job.movingTries < MaxMovingTries)
        {
            ++job.movingTries;
            player->StopMoving();
            BotDriver::PauseAI(player, 3 * IN_MILLISECONDS);
            job.nextAt = now + 500;
            return true;
        }
        why = "the cast failed";
        LOG_INFO(ActivityLogName, "activity: {} cannot cast {} on {} (result {})", player->GetName(), info->SpellName[0],
            job.vellum ? std::string("a vellum") : "its " + std::string(item->GetTemplate()->Name1), uint32(result));
    }
    uint32 const owner = sStorageService.GetOwnerOf(player);
    if (job.vellum)
    {
        // The scroll into the guild storage; a vellum it did not write on goes back there.
        ItemTemplate const* scrollProto = sObjectMgr->GetItemTemplate(job.scroll);
        if (uint32 const made = MoveIntoStorage(player, owner, job.scroll, 1))
            LOG_INFO(ActivityLogName, "activity: {} made {} for the guild: into the guild storage ({})", player->GetName(),
                scrollProto ? scrollProto->Name1 : "a scroll", job.text);
        else
        {
            uint32 const back = MoveIntoStorage(player, owner, job.vellumEntry, 1);
            LOG_INFO(ActivityLogName, "activity: {} did not make {} for the guild ({}){}", player->GetName(),
                scrollProto ? scrollProto->Name1 : "a scroll", why, back ? ": the vellum is back in the guild storage" : "");
        }
        _scrollClaims.erase({ job.wearer, job.slot });
    }
    else if (job.apply)
    {
        ItemTemplate const* scrollProto = sObjectMgr->GetItemTemplate(job.scroll);
        if (item && item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) == job.enchant)
            LOG_INFO(ActivityLogName, "activity: {} put {} of the guild storage on its {}: {} (was {})", player->GetName(),
                scrollProto ? scrollProto->Name1 : "a scroll", item->GetTemplate()->Name1, EnchantName(job.enchant), EnchantName(job.before));
        else
        {
            uint32 const back = MoveIntoStorage(player, owner, job.scroll, 1);
            LOG_INFO(ActivityLogName, "activity: {} did not put {} on its {} ({}){}", player->GetName(), scrollProto ? scrollProto->Name1 : "a scroll",
                item ? item->GetTemplate()->Name1 : "piece", why, back ? ": the scroll is back in the guild storage" : "");
        }
    }
    else if (job.done)
        LOG_INFO(ActivityLogName, "activity: {} put {} on its own {} {} time{} for its profession: Enchanting {} -> {} (ended: {})",
            player->GetName(), info ? info->SpellName[0] : "an enchantment", item ? item->GetTemplate()->Name1 : "gear", job.done,
            job.done == 1 ? "" : "s", job.skillBefore, player->GetPureSkillValue(SKILL_ENCHANTING), why);
    _enchanting.erase(itr);
    return false;
}

bool Professions::CraftGearFor(GuildBot const& bot, Player* player, std::vector<Player*> const& wearers, std::string& text, uint32& item,
    Player*& wearer, bool& forRole, GearStep* step, bool mayWalk, std::unordered_set<uint32> const* failed) const
{
    uint32 const store = sStorageService.GetOwnerOf(player);
    uint32 const owner = GetSettings().storageReagents ? store : 0;
    std::vector<uint32> const planned = Planned(bot);
    forRole = false;
    if (step)
        *step = GearStep();

    // What the bot can make at all (any profession it has: a blacksmith smelts with its Mining), by product.
    std::unordered_map<uint32, uint32> makers;
    for (auto const& [spellId, state] : player->GetSpellMap())
    {
        if (!state || state->State == PLAYERSPELL_REMOVED || !state->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || !CraftService::IsCraftRecipe(info))
            continue;
        if (uint32 const product = MadeBy(info))
            makers.emplace(product, spellId);
    }

    struct Choice
    {
        uint32 spell = 0, product = 0, gain = 0, level = 0;
        Player* wearer = nullptr;
        GearStep way;
    };
    // The piece somebody would level in, and the best one for somebody's dungeon set (DungeonRoles.h): made when
    // nobody would level in anything.
    Choice best, role;
    auto better = [](Choice const& a, uint32 gain, uint32 level, GearStep const& way)
    {
        if (!a.spell || gain != a.gain)
            return !a.spell || gain > a.gain;
        bool const ready = way.kind == GearStep::Kind::Piece, wasReady = a.way.kind == GearStep::Kind::Piece;
        return ready != wasReady ? ready : level > a.level;
    };
    for (auto const& [product, spellId] : makers)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        bool mine = false;
        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto ability = bounds.first; ability != bounds.second && !mine; ++ability)
            mine = std::find(planned.begin(), planned.end(), ability->second->SkillLine) != planned.end();
        ItemTemplate const* proto = mine ? sObjectMgr->GetItemTemplate(product) : nullptr;
        // Something worn for the fights (a weapon, armour).
        if (!proto || !Gear::SlotGroup(proto) || (failed && failed->count(product)))
            continue;
        // Only green or better pieces are made as upgrades (the owner, 2026-11-13: [quote left out]).
        if (proto->Quality < ITEM_QUALITY_UNCOMMON)
            continue;
        // No wearer can gain more from it than its item level.
        if (best.spell && proto->ItemLevel < best.gain)
            continue;
        // One that waits for its wearer is enough.
        if (store && sStorageService.GetAmount(store, product, 0))
            continue;
        // What it takes: nothing the bot cannot get by itself, and no walk where none is made (a town trip).
        GearStep const way = WayToCast(player, info, 1, owner, makers, 2);
        if (way.kind == GearStep::Kind::None || (way.kind == GearStep::Kind::Focus && !mayWalk))
            continue;
        for (Player* candidate : wearers)
        {
            // Not for somebody who could not put it on (mod-playerbots' rating asks the core too; this is the rule
            // in the module's own words, with the proficiency and the reputation).
            if (!CanWearCrafted(candidate, proto))
                continue;
            if (proto->Bonding == BIND_WHEN_PICKED_UP && candidate != player)
                continue;
            if (proto->RequiredLevel > candidate->GetLevel() || candidate->HasItemCount(product, 1, true))
                continue;
            // Whoever gains most from it: the item levels above what it wears there (a bare place: all of them).
            uint32 const worn = WornLevelFor(candidate, proto);
            uint32 const gain = proto->ItemLevel > worn ? proto->ItemLevel - worn : 1;
            bool const forLevelling = better(best, gain, proto->ItemLevel, way);
            bool const forSet = better(role, gain, proto->ItemLevel, way);
            if (!forLevelling && !forSet)
                continue;
            Choice choice;
            choice.spell = spellId;
            choice.product = product;
            choice.gain = gain;
            choice.level = proto->ItemLevel;
            choice.wearer = candidate;
            choice.way = way;
            if (BotDriver::IsUpgrade(candidate, product, 0))
            {
                if (forLevelling)
                    best = choice;
            }
            // Not to level in: for its dungeon role (a tank's or healer's set, rated for the role)?
            else if (forSet && sRoleService.IsRoleUpgrade(candidate, product, 0))
                role = choice;
        }
    }
    if (!best.spell && role.spell)
    {
        best = role;
        forRole = true;
    }
    if (!best.spell)
        return false;
    item = best.product;
    wearer = best.wearer;
    if (step)
    {
        *step = best.way;
        step->gain = best.gain;
    }
    std::string error;
    switch (best.way.kind)
    {
        case GearStep::Kind::Piece:
            return sCraftService.Start(player, best.spell, 1, error, text);
        case GearStep::Kind::Material:
            return sCraftService.Start(player, best.way.spell, std::max<uint32>(best.way.count, 1), error, text);
        default:
            return false; // a purchase or a walk: the caller's
    }
}

GameObject* Professions::FindNode(Player* player, float range)
{
    bool const mining = player->HasSkill(SKILL_MINING);
    bool const herbs = player->HasSkill(SKILL_HERBALISM);
    if (!mining && !herbs)
        return nullptr;

    // (The core's search by entry finds nothing for entry 0: a check of its own.)
    std::list<GameObject*> objects;
    ChestInRange check(player, range);
    Acore::GameObjectListSearcher<ChestInRange> searcher(player, objects, check);
    Cell::VisitObjects(player, searcher, range);
    GameObject* best = nullptr;
    float bestDistance = 0.0f;
    for (GameObject* go : objects)
    {
        if (go->GetGoType() != GAMEOBJECT_TYPE_CHEST || go->getLootState() != GO_READY || !go->isSpawned() ||
            !sProfessions.IsNode(go->GetEntry()))
            continue;
        LockEntry const* lock = sLockStore.LookupEntry(go->GetGOInfo()->GetLockId());
        if (!lock)
            continue;
        bool opens = false;
        for (uint8 i = 0; i < MAX_LOCK_CASE && !opens; ++i)
        {
            if (lock->Type[i] != LOCK_KEY_SKILL)
                continue;
            if (lock->Index[i] == LOCKTYPE_MINING)
                opens = mining && lock->Skill[i] <= player->GetSkillValue(SKILL_MINING);
            else if (lock->Index[i] == LOCKTYPE_HERBALISM)
                opens = herbs && lock->Skill[i] <= player->GetSkillValue(SKILL_HERBALISM);
        }
        if (!opens)
            continue;
        float const distance = player->GetDistance(go);
        if (!best || distance < bestDistance)
        {
            best = go;
            bestDistance = distance;
        }
    }
    return best;
}
