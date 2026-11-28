/*
 * WorldIndex implementation. See WorldIndex.h.
 */

#include "WorldIndex.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "QuestInteraction.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildAdventureUtil.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "RoadMap.h"
#include "Timer.h"
#include "StringFormat.h"
#include "Trainer.h"
#include "TransportMgr.h"
#include "WaypointMgr.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <unordered_set>

using namespace GuildAdventure;

namespace
{
    // Faction templates of a human and an orc player: the reference for "friendly to the team" before any bot of
    // that team is involved (hubs).
    constexpr uint32 AllianceFactionTemplate = 1;
    constexpr uint32 HordeFactionTemplate = 2;

    // Area triggers on one map that move a player less than this are no travel (towers, halls).
    constexpr float MinPortalJump = 300.0f;

    // Hub search: quest givers within this distance of each other count as one cluster.
    constexpr float HubClusterRadius = 80.0f;

    bool IsOpenWorldMap(uint32 mapId)
    {
        MapEntry const* map = sMapStore.LookupEntry(mapId);
        return map && !map->Instanceable();
    }

    uint32 FoldToZone(uint32 areaId)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
        if (!area)
            return 0;
        return area->zone ? area->zone : area->ID;
    }

    std::vector<SpawnPoint> const EmptySpawns;
    std::vector<uint32> const EmptyIds;
}

char const* GuildAdventure::ToString(Region region)
{
    switch (region)
    {
        case Region::None:            return "none";
        case Region::EasternKingdoms: return "Eastern Kingdoms";
        case Region::Kalimdor:        return "Kalimdor";
        case Region::Teldrassil:      return "Teldrassil";
        case Region::RuttheranVillage: return "Rut'theran Village";
        case Region::SardorIsle:      return "Sardor Isle";
        case Region::AzuremystIsles:  return "Azuremyst Isles";
        case Region::EversongIsles:   return "Eversong Isles";
        case Region::Outland:         return "Outland";
        case Region::Northrend:       return "Northrend";
    }
    return "none";
}

Region GuildAdventure::GetRegionAt(uint32 mapId, float x, float y, float z)
{
    switch (mapId)
    {
        case 0:
            return Region::EasternKingdoms;
        case 1:
            // Teldrassil lies north-west of Darkshore (x above 7800); Rut'theran Village is at sea level, the top of the
            // world tree more than a thousand yards up.
            if (x > 7800.0f && y > -800.0f && y < 3500.0f)
                return z < 500.0f ? Region::RuttheranVillage : Region::Teldrassil;
            // Sardor Isle lies west of the Feralas coast, across the water the Feathermoon Ferry crosses (its docks:
            // the coast near y 2440, the island near y 3280).
            if (x > -4900.0f && x < -3800.0f && y > 2950.0f && y < 3700.0f)
                return Region::SardorIsle;
            return Region::Kalimdor;
        case 530:
            // The two Burning Crusade starting islands are far south of Outland (y below -4000): Azuremyst in the west,
            // Eversong in the east.
            if (y < -4000.0f)
                return x < 0.0f ? Region::AzuremystIsles : Region::EversongIsles;
            return Region::Outland;
        case 571:
            return Region::Northrend;
        default:
            return Region::None;
    }
}

Region GuildAdventure::GetRegion(Player const* player)
{
    return player ? GetRegionAt(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ()) :
        Region::None;
}

Region GuildAdventure::GetZoneRegion(uint32 mapId, uint32 zoneId, TeamId team)
{
    if (ZoneHub const* hub = sWorldIndex.GetHub(zoneId, team))
        return GetRegionAt(hub->mapId, hub->x, hub->y, hub->z);
    return GetRegion(mapId, zoneId);
}

Region GuildAdventure::GetRegion(uint32 mapId, uint32 zoneId)
{
    switch (mapId)
    {
        case 0:
            return Region::EasternKingdoms;
        case 1:
            // Teldrassil, Darnassus (Rut'theran Village is part of Teldrassil)
            return (zoneId == 141 || zoneId == 1657) ? Region::Teldrassil : Region::Kalimdor;
        case 530:
            switch (zoneId)
            {
                case 3524: // Azuremyst Isle
                case 3525: // Bloodmyst Isle
                case 3557: // The Exodar
                    return Region::AzuremystIsles;
                case 3430: // Eversong Woods
                case 3433: // Ghostlands
                case 3487: // Silvermoon City
                case 4080: // Isle of Quel'Danas
                    return Region::EversongIsles;
                default:
                    return Region::Outland;
            }
        case 571:
            return Region::Northrend;
        default:
            return Region::None;
    }
}

WorldIndex& WorldIndex::Instance()
{
    static WorldIndex instance;
    return instance;
}

void WorldIndex::Build()
{
    uint32 const start = getMSTime();

    GuildAdventure::QuestInteraction::ClearCache(); // its plans read the same world data
    _creatureSpawns.clear();
    _goSpawns.clear();
    _questStarters.clear();
    _questEnders.clear();
    _zoneQuests.clear();
    _questZone.clear();
    _itemSources.clear();
    _itemVendors.clear();
    _questContainers.clear();
    _itemRewardQuests.clear();
    for (auto& list : _services)
        list.clear();
    _hubs.clear();
    _mobSpawns.clear();
    _strongSpawns.clear();
    _patrolPoints.clear();
    _links.clear();
    _spawnZones.clear();
    _spawnCount = 0;

    IndexSpawns();
    IndexQuests();
    IndexItemSources();
    IndexServices();
    IndexMobs();
    IndexTransports();
    ComputeHubs();

    if (GetSettings().debug)
        for (TravelLink const& link : _links)
            LOG_INFO("module", "mod-guild-adventure: travel link {} ({} {}): map {} ({:.0f}, {:.0f}, {:.0f}) {} -> map {} ({:.0f}, {:.0f}, {:.0f}) {}",
                link.name.empty() ? "?" : link.name, link.areaTrigger ? "trigger" : "transport",
                link.areaTrigger ? link.areaTrigger : link.transportEntry, link.from.mapId, link.from.x, link.from.y, link.from.z,
                ToString(link.fromRegion), link.to.mapId, link.to.x, link.to.y, link.to.z, ToString(link.toRegion));

    std::size_t services = 0;
    for (auto const& list : _services)
        services += list.size();
    LOG_INFO("module", "mod-guild-adventure: world index: {} open-world spawns, {} zones with quests, {} quest items with sources, "
        "{} service NPCs, {} zone hubs, {} ship/zeppelin legs ({} ms)", _spawnCount, _zoneQuests.size(), _itemSources.size(),
        services, _hubs.size(), _links.size(), GetMSTimeDiffToNow(start));
}

void WorldIndex::IndexSpawns()
{
    // Spawns that exist only while a game event runs (holidays, the Scourge invasion, ...): the planner must not send
    // a bot to an NPC that is not there.
    std::unordered_set<ObjectGuid::LowType> eventCreatures;
    if (QueryResult result = WorldDatabase.Query("SELECT guid FROM game_event_creature WHERE eventEntry > 0"))
    {
        do
            eventCreatures.insert(result->Fetch()[0].Get<uint32>());
        while (result->NextRow());
    }
    _eventCreatureSpawns = eventCreatures;
    std::unordered_set<ObjectGuid::LowType> eventObjects;
    if (QueryResult result = WorldDatabase.Query("SELECT guid FROM game_event_gameobject WHERE eventEntry > 0"))
    {
        do
            eventObjects.insert(result->Fetch()[0].Get<uint32>());
        while (result->NextRow());
    }

    // Only spawns of the normal phase: bots never leave it.
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (!IsOpenWorldMap(data.mapid) || !(data.phaseMask & PHASEMASK_NORMAL) || eventCreatures.count(spawnId))
            continue;

        SpawnPoint point;
        point.spawnId = spawnId;
        point.mapId = data.mapid;
        point.x = data.posX;
        point.y = data.posY;
        point.z = data.posZ;
        for (uint32 entry : { data.id, data.id2, data.id3 })
        {
            if (!entry)
                continue;
            point.entry = entry;
            _creatureSpawns[entry].push_back(point);
            ++_spawnCount;
        }
    }

    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        if (!data.id || !IsOpenWorldMap(data.mapid) || !(data.phaseMask & PHASEMASK_NORMAL) || eventObjects.count(spawnId))
            continue;

        SpawnPoint point;
        point.spawnId = spawnId;
        point.entry = data.id;
        point.mapId = data.mapid;
        point.x = data.posX;
        point.y = data.posY;
        point.z = data.posZ;
        point.gameObject = true;
        _goSpawns[data.id].push_back(point);
        ++_spawnCount;
    }
}

void WorldIndex::IndexQuests()
{
    auto addRelations = [](QuestRelations const* relations, bool gameObject, auto& target)
    {
        if (!relations)
            return;
        for (auto const& [entry, questId] : *relations)
            target[questId].emplace_back(entry, gameObject);
    };

    addRelations(sObjectMgr->GetCreatureQuestRelationMap(), false, _questStarters);
    addRelations(sObjectMgr->GetGOQuestRelationMap(), true, _questStarters);
    addRelations(sObjectMgr->GetCreatureQuestInvolvedRelationMap(), false, _questEnders);
    addRelations(sObjectMgr->GetGOQuestInvolvedRelationMap(), true, _questEnders);

    for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
    {
        int32 const sort = quest->GetZoneOrSort();
        if (sort <= 0)
            continue;

        uint32 const zone = FoldToZone(uint32(sort));
        if (!zone)
            continue;

        _questZone[questId] = zone;
        _zoneQuests[zone].push_back(questId);
    }

    for (auto& [zone, quests] : _zoneQuests)
        std::sort(quests.begin(), quests.end());
}

void WorldIndex::IndexItemSources()
{
    // Only items quests ask for. Loot rows flagged for the quest (QuestRequired = 1) count at any chance; ordinary loot
    // (cooking meat, "Flask of Oil", ...) from OrdinaryLootChance percent on, so the nearest source is one worth
    // killing. Reference tables are followed one level deep.
    constexpr float OrdinaryLootChance = 10.0f;

    std::unordered_set<uint32> wanted;
    for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredItemId[i])
                wanted.insert(quest->RequiredItemId[i]);

    // Items another quest hands out as a reward ("The Green Hills of Stranglethorn" asks for the four chapters).
    for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
        for (uint8 i = 0; i < QUEST_REWARDS_COUNT; ++i)
            if (quest->RewardItemId[i] && wanted.count(quest->RewardItemId[i]))
                _itemRewardQuests[quest->RewardItemId[i]].push_back(questId);

    // Items inside another item (item_loot_template: a necklace that holds the jewel, a crate): the container is
    // looked for like a wanted item, and its sources count for what it holds. The bot opens it (OrderService).
    std::unordered_map<uint32, std::vector<uint32>> containersOf; // wanted item -> containers
    std::unordered_set<uint32> lootable;                          // every item that holds something
    if (QueryResult result = WorldDatabase.Query("SELECT Entry, Item FROM item_loot_template WHERE Reference = 0"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const container = fields[0].Get<uint32>();
            uint32 const item = fields[1].Get<uint32>();
            if (!sObjectMgr->GetItemTemplate(container))
                continue;
            lootable.insert(container);
            if (!wanted.count(item))
                continue;
            containersOf[item].push_back(container);
            _questContainers.insert(container);
        } while (result->NextRow());
    }
    // ... and items whose use opens them by a spell with loot of its own (spell_loot_template: the Big-mouth Clam holds
    // the Zesty Clam Meat of "Clamlette Surprise"; LootService opens such a clam when it is looted). Thunderhoof (46) stood
    // 338 s at "the quest area" of that objective in Tanaris until NewRpg gave up, 2026-11-26: the meat had no source.
    // Their sources count for what they hold; they are not quest containers (OpenQuestContainers opens by item loot).
    std::unordered_set<uint32> spellContainers;
    {
        std::unordered_map<uint32, std::vector<uint32>> spellHolds; // spell -> wanted items its loot holds
        if (QueryResult result = WorldDatabase.Query("SELECT Entry, Item FROM spell_loot_template WHERE Reference = 0"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const item = fields[1].Get<uint32>();
                if (wanted.count(item))
                    spellHolds[fields[0].Get<uint32>()].push_back(item);
            } while (result->NextRow());
        }
        if (!spellHolds.empty())
            for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
                for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
                {
                    if (proto.Spells[i].SpellId <= 0 || proto.Spells[i].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                        continue;
                    auto const holds = spellHolds.find(uint32(proto.Spells[i].SpellId));
                    if (holds == spellHolds.end())
                        continue;
                    for (uint32 item : holds->second)
                        if (item != entry)
                            containersOf[item].push_back(entry);
                    spellContainers.insert(entry);
                }
    }
    // A container a quest hands out (its start item or a source item) holds what the quest works with: the box of
    // "Seeping Corruption" holds the vials to fill. It is opened too, unless a quest wants it back as it is.
    std::vector<uint32> handedOut;
    for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
    {
        if (uint32 const item = quest->GetSrcItemId(); item && lootable.count(item) && !wanted.count(item))
            handedOut.push_back(item);
        for (uint32 item : quest->ItemDrop)
            if (item && lootable.count(item) && !wanted.count(item))
                handedOut.push_back(item);
    }
    for (uint32 container : _questContainers)
        wanted.insert(container);
    for (uint32 container : spellContainers)
        wanted.insert(container);
    // The materials of profession recipes that are trade goods (cloth, meat, ...): a bot in the profession mode hunts
    // for what its recipes lack and creatures drop (OrderService::PlanMaterialHunt). After the containers: what holds
    // a material is not opened for it.
    for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
    {
        SkillLineAbilityEntry const* ability = sSkillLineAbilityStore.LookupEntry(i);
        SkillLineEntry const* line = ability ? sSkillLineStore.LookupEntry(ability->SkillLine) : nullptr;
        if (!line || (line->categoryId != SKILL_CATEGORY_PROFESSION && line->categoryId != SKILL_CATEGORY_SECONDARY))
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(ability->Spell);
        if (!info)
            continue;
        for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
            if (info->Reagent[r] > 0)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(uint32(info->Reagent[r])))
                    if (proto->Class == ITEM_CLASS_TRADE_GOODS && IsHuntMaterial(proto))
                        wanted.insert(proto->ItemId);
    }
    _questContainers.insert(handedOut.begin(), handedOut.end());

    std::unordered_map<uint32, std::vector<uint32>> creatureByLoot;
    for (auto const& [entry, cinfo] : *sObjectMgr->GetCreatureTemplates())
        if (cinfo.lootid)
            creatureByLoot[cinfo.lootid].push_back(entry);

    std::unordered_map<uint32, std::vector<uint32>> goByLoot;
    for (auto const& [entry, goinfo] : *sObjectMgr->GetGameObjectTemplates())
        if (uint32 const lootId = goinfo.GetLootId())
            goByLoot[lootId].push_back(entry);

    // (item, entry, is GO) seen so far, so an item listed twice for a creature gives one source.
    std::unordered_set<uint64> seen;
    // `chance`: the loot row's percent. A row with chance 0 shares what its group leaves with the group's other such
    // rows, and a row behind a reference shares the reference with what else it holds: both counted as a rough share.
    constexpr float GroupShare = 25.0f;
    constexpr float ReferenceShare = 20.0f;
    auto add = [&](uint32 item, std::vector<uint32> const& entries, bool gameObject, float chance)
    {
        for (uint32 entry : entries)
        {
            uint64 const key = (uint64(item) << 32) | (uint64(entry) << 1) | (gameObject ? 1 : 0);
            if (seen.insert(key).second)
                _itemSources[item].emplace_back(entry, gameObject);
            float& known = _itemSourceChance[key];
            known = std::max(known, chance);
        }
    };

    // Reference tables that hold a wanted item: reference -> items.
    std::unordered_map<uint32, std::vector<uint32>> referenceItems;
    if (QueryResult result = WorldDatabase.Query("SELECT Entry, Item, QuestRequired, Chance FROM reference_loot_template WHERE Reference = 0"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const item = fields[1].Get<uint32>();
            // Chance 0 in a reference table is an equal share of its group: only the quest-flagged rows are taken then.
            if (wanted.count(item) && (fields[2].Get<bool>() || fields[3].Get<float>() >= OrdinaryLootChance))
                referenceItems[fields[0].Get<uint32>()].push_back(item);
        } while (result->NextRow());
    }

    auto indexLoot = [&](char const* table, std::unordered_map<uint32, std::vector<uint32>> const& byLoot, bool gameObject)
    {
        QueryResult result = WorldDatabase.Query("SELECT Entry, Item, Reference, QuestRequired, Chance FROM {} "
            "WHERE QuestRequired = 1 OR Reference > 0 OR Chance >= {}", table, OrdinaryLootChance);
        if (!result)
            return;
        do
        {
            Field* fields = result->Fetch();
            auto owners = byLoot.find(fields[0].Get<uint32>());
            if (owners == byLoot.end())
                continue;
            // Signed: the world database has a stray negative reference (creature_loot_template, -34377).
            int32 const reference = fields[2].Get<int32>();
            bool const questRow = fields[3].Get<bool>();
            float const chance = fields[4].Get<float>();
            if (reference < 0)
                continue;
            if (!reference)
            {
                uint32 const item = fields[1].Get<uint32>();
                // Quest-flagged rows as before, for any item; ordinary rows only for items quests ask for.
                if (questRow || wanted.count(item))
                    add(item, owners->second, gameObject, chance > 0.0f ? chance : GroupShare);
                continue;
            }
            auto items = referenceItems.find(uint32(reference));
            if (items == referenceItems.end() || (!questRow && chance < OrdinaryLootChance))
                continue;
            for (uint32 item : items->second)
                add(item, owners->second, gameObject, std::min(chance > 0.0f ? chance : GroupShare, ReferenceShare));
        } while (result->NextRow());
    };
    indexLoot("creature_loot_template", creatureByLoot, false);
    indexLoot("gameobject_loot_template", goByLoot, true);

    // What drops a container is a source of what the container holds.
    for (auto const& [item, containers] : containersOf)
        for (uint32 container : containers)
        {
            auto sources = _itemSources.find(container);
            if (sources == _itemSources.end())
                continue;
            std::vector<std::pair<uint32, bool>> const copy = sources->second; // _itemSources[item] may rehash the map
            for (auto const& [entry, gameObject] : copy)
            {
                // (As often as its container.)
                uint64 const key = (uint64(item) << 32) | (uint64(entry) << 1) | (gameObject ? 1 : 0);
                if (seen.insert(key).second)
                    _itemSources[item].emplace_back(entry, gameObject);
                auto const held = _itemSourceChance.find((uint64(container) << 32) | (uint64(entry) << 1) | (gameObject ? 1 : 0));
                float& known = _itemSourceChance[key];
                known = std::max(known, held != _itemSourceChance.end() ? held->second : 0.0f);
            }
        }

    // How many items have a source that is left out for its chance (GetItemSources).
    uint32 thinned = 0;
    for (auto const& [item, sources] : _itemSources)
    {
        float best = 0.0f, worst = 100.0f;
        for (auto const& [entry, gameObject] : sources)
        {
            auto const known = _itemSourceChance.find((uint64(item) << 32) | (uint64(entry) << 1) | (gameObject ? 1 : 0));
            float const chance = known != _itemSourceChance.end() ? known->second : 0.0f;
            if (chance <= 0.0f)
                continue;
            best = std::max(best, chance);
            worst = std::min(worst, chance);
        }
        if (best > 0.0f && worst * 5.0f < best)
            ++thinned;
    }
    LOG_INFO("module", "mod-guild-adventure: world index: {} items have a source that is left out: another one has the item "
        "five times as often", thinned);

    // Vendors that sell a wanted item (for money, not for tokens).
    for (auto const& [entry, cinfo] : *sObjectMgr->GetCreatureTemplates())
    {
        if (!(cinfo.npcflag & UNIT_NPC_FLAG_VENDOR))
            continue;
        VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(entry);
        if (!items)
            continue;
        for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
        {
            VendorItem const* item = items->GetItem(slot);
            if (item && !item->ExtendedCost && wanted.count(item->item))
                _itemVendors[item->item].emplace_back(entry, false);
        }
    }
}

void WorldIndex::IndexServices()
{
    for (auto const& [entry, spawns] : _creatureSpawns)
    {
        CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(entry);
        if (!cinfo)
            continue;

        // Script helpers are no services: "[DND] ..." / "[PH] ..." names, triggers, unselectable creatures.
        if (cinfo->Name.empty() || cinfo->Name[0] == '[' || cinfo->HasFlagsExtra(CREATURE_FLAG_EXTRA_TRIGGER) ||
            (cinfo->unit_flags & UNIT_FLAG_NOT_SELECTABLE))
            continue;

        uint8 trainerClass = 0;
        if (cinfo->npcflag & UNIT_NPC_FLAG_TRAINER_CLASS)
            if (Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(entry))
                if (trainer->GetTrainerType() == Trainer::Type::Class)
                    trainerClass = uint8(trainer->GetTrainerRequirement());

        for (SpawnPoint const& spawn : spawns)
        {
            CreatureData const* data = sObjectMgr->GetCreatureData(spawn.spawnId);
            uint32 const flags = (data && data->npcflag) ? data->npcflag : cinfo->npcflag;

            ServicePoint point;
            point.spawn = spawn;
            point.npcFlags = flags;
            point.factionTemplate = cinfo->faction;
            point.name = cinfo->Name;

            if (flags & UNIT_NPC_FLAG_VENDOR_MASK || flags & UNIT_NPC_FLAG_VENDOR_FOOD)
                _services[uint8(ServiceType::Vendor)].push_back(point);
            if (flags & UNIT_NPC_FLAG_REPAIR)
                _services[uint8(ServiceType::Repair)].push_back(point);
            if (flags & UNIT_NPC_FLAG_INNKEEPER)
                _services[uint8(ServiceType::Innkeeper)].push_back(point);
            if (flags & UNIT_NPC_FLAG_FLIGHTMASTER)
                _services[uint8(ServiceType::FlightMaster)].push_back(point);
            if (flags & UNIT_NPC_FLAG_VENDOR_FOOD)
                _services[uint8(ServiceType::FoodVendor)].push_back(point);
            if (flags & UNIT_NPC_FLAG_SPIRITHEALER)
                _services[uint8(ServiceType::SpiritHealer)].push_back(point);
            if (trainerClass)
            {
                point.trainerClass = trainerClass;
                _services[uint8(ServiceType::ClassTrainer)].push_back(point);
            }
        }
    }
}

void WorldIndex::IndexMobs()
{
    // Creatures that run for help when they are nearly dead: their SmartAI script says so (action 25).
    _fleeingEntries.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT entryorguid FROM smart_scripts WHERE source_type = 0 AND "
        "action_type = 25 AND entryorguid > 0"))
    {
        do
            _fleeingEntries.insert(uint32(result->Fetch()[0].Get<int32>()));
        while (result->NextRow());
    }
    // Creatures that call every creature of their side for help in a fight (action 39, its first parameter the
    // radius). Gorruk (55, warrior) pulled one Deadwood furbolg in Felwood ("1 expected"); 15 s in a Den Watcher
    // called at half its health, a Den Watcher, an Avenger and Chieftain Bloodmaw came, and it died (2026-11-27).
    _callsForHelp.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT entryorguid, MAX(action_param1) FROM smart_scripts WHERE source_type = 0 AND "
        "action_type = 39 AND entryorguid > 0 AND event_type IN (0, 2, 4) GROUP BY entryorguid"))
    {
        do
        {
            Field const* fields = result->Fetch();
            if (uint32 const radius = fields[1].Get<uint32>())
                _callsForHelp[uint32(fields[0].Get<int32>())] = std::min(float(radius), 60.0f);
        } while (result->NextRow());
    }
    _earlyFleeingEntries.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT entryorguid FROM smart_scripts WHERE source_type = 0 AND "
        "action_type = 25 AND entryorguid > 0 AND (event_type = 0 OR (event_type = 2 AND event_param2 >= 50))"))
    {
        do
            _earlyFleeingEntries.insert(uint32(result->Fetch()[0].Get<int32>()));
        while (result->NextRow());
    }
    // Creatures that stun their foe every few seconds: an in-combat timer (event 0) of at most 12 s that casts (action
    // 11) a spell with a stun aura.
    _stunningEntries.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT entryorguid, action_param1 FROM smart_scripts WHERE source_type = 0 AND "
        "entryorguid > 0 AND event_type = 0 AND action_type = 11 AND event_param4 BETWEEN 1 AND 12000"))
    {
        do
        {
            Field const* fields = result->Fetch();
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(fields[1].Get<uint32>()); info && info->HasAura(SPELL_AURA_MOD_STUN))
                _stunningEntries.insert(uint32(fields[0].Get<int32>()));
        } while (result->NextRow());
    }

    // Creatures that knock their foe back: any cast (action 11) of their SmartAI script of a spell with a knock back
    // effect. A Stitched Horror's Mighty Blow threw bots of level 60 off the plateau above the Infectis Scar into the
    // hollow below it, three times in four days (round 272).
    _knockbackEntries.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT entryorguid, action_param1 FROM smart_scripts WHERE source_type = 0 AND "
        "entryorguid > 0 AND action_type = 11"))
    {
        do
        {
            Field const* fields = result->Fetch();
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(fields[1].Get<uint32>());
                info && (info->HasEffect(SPELL_EFFECT_KNOCK_BACK) || info->HasEffect(SPELL_EFFECT_KNOCK_BACK_DEST)))
                _knockbackEntries.insert(uint32(fields[0].Get<int32>()));
        } while (result->NextRow());
    }

    // Creatures a run does not get away from: an in-combat timer (event 0) of at most 8 s that casts a damage spell of
    // 20 yd's reach or more, or any in-combat cast of a spell that roots. Mojowind (50, shaman) left three Bloodsail
    // of level 40 at 87 % health and died on the run 36 s later: two Bloodsail Mages cast Fireball at its back every
    // four to six seconds, a Raider's Net held it (round 259).
    _runHolders.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT entryorguid, action_param1, event_param4 FROM smart_scripts WHERE "
        "source_type = 0 AND entryorguid > 0 AND event_type = 0 AND action_type = 11"))
    {
        do
        {
            Field const* fields = result->Fetch();
            SpellInfo const* info = sSpellMgr->GetSpellInfo(fields[1].Get<uint32>());
            if (!info)
                continue;
            uint32 const repeatMax = fields[2].Get<uint32>();
            if (info->HasAura(SPELL_AURA_MOD_ROOT) ||
                (repeatMax >= 1 && repeatMax <= 8000 && info->HasEffect(SPELL_EFFECT_SCHOOL_DAMAGE) && info->GetMaxRange(false) >= 20.0f))
                _runHolders.insert(uint32(fields[0].Get<int32>()));
        } while (result->NextRow());
    }

    // Creatures their SmartAI scripts summon: a spell cast with a summon effect (action 11) or a summon (action 12),
    // in a fight or around one (in combat, out of combat, at a health, on aggro, in range, at a respawn or a reset):
    // not for a quest's item, a talk, a death or an event step (those are the quest's, or come after the fight).
    _minions.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT entryorguid, action_type, action_param1 FROM smart_scripts WHERE "
        "source_type = 0 AND entryorguid > 0 AND action_type IN (11, 12) AND event_type IN (0, 1, 2, 4, 9, 11, 25)"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const summoner = uint32(fields[0].Get<int32>());
            uint32 const param = fields[2].Get<uint32>();
            uint32 minion = 0;
            if (fields[1].Get<uint8>() == 12)
                minion = param;
            else if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(param))
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS && !minion; ++i)
                    // ("Summon Voidwalker" 12746, the Wastewander Shadow Mages' and nine more casters' spell, is a summon of a
                    // pet: a creature casting it gets a guardian. Two guild characters died in Tanaris to such minions.)
                    if ((spell->Effects[i].Effect == SPELL_EFFECT_SUMMON || spell->Effects[i].Effect == SPELL_EFFECT_SUMMON_PET) &&
                        spell->Effects[i].MiscValue > 0)
                        minion = uint32(spell->Effects[i].MiscValue);
            CreatureTemplate const* info = minion && minion != summoner ? sObjectMgr->GetCreatureTemplate(minion) : nullptr;
            if (!info || info->type == CREATURE_TYPE_CRITTER || info->type == CREATURE_TYPE_TOTEM ||
                info->type == CREATURE_TYPE_NOT_SPECIFIED)
                continue;
            _minions.emplace(summoner, minion);
        } while (result->NextRow());
    }

    // ... and what their scripts summon when they die (SMART_EVENT_DEATH), when it is hostile to players: such a
    // creature is two fights, the second out of its corpse. Catdudu (60) ground Tortured Druids and Sentinels in
    // Silithus, each of them brings a Hive'Ashi Drone at its death (spell 21327); two kills in one fight left it with
    // two drones at 31 % health, and it died on the run, round 231.
    _deathSummons.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT entryorguid, action_type, action_param1 FROM smart_scripts WHERE "
        "source_type = 0 AND entryorguid > 0 AND action_type IN (11, 12) AND event_type = 6"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const summoner = uint32(fields[0].Get<int32>());
            uint32 const param = fields[2].Get<uint32>();
            uint32 summon = 0;
            uint32 count = 1;
            if (fields[1].Get<uint8>() == 12)
                summon = param;
            else if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(param))
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS && !summon; ++i)
                    if (spell->Effects[i].Effect == SPELL_EFFECT_SUMMON && spell->Effects[i].MiscValue > 0)
                    {
                        summon = uint32(spell->Effects[i].MiscValue);
                        // (As the core's count of a summon effect: its base points and the highest roll of its dice.)
                        count = uint32(std::max<int32>(1, spell->Effects[i].BasePoints + spell->Effects[i].DieSides));
                    }
            CreatureTemplate const* info = summon && summon != summoner ? sObjectMgr->GetCreatureTemplate(summon) : nullptr;
            if (!info || info->type == CREATURE_TYPE_CRITTER || info->type == CREATURE_TYPE_TOTEM ||
                info->type == CREATURE_TYPE_NOT_SPECIFIED || info->HasFlagsExtra(CREATURE_FLAG_EXTRA_TRIGGER) ||
                (info->unit_flags & (UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_IMMUNE_TO_PC)))
                continue;
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->faction);
            if (!faction || !faction->IsHostileToPlayers())
                continue;
            auto& entry = _deathSummons[summoner];
            if (!entry.first)
                entry.first = summon;
            if (entry.first == summon)
                entry.second = uint8(std::min<uint32>(entry.second + count, 10));
        } while (result->NextRow());
    }

    // ... and what they summon in a fight, when it is hostile to players, whatever its type: such a creature is a
    // fight of two or more from its first seconds. Ashmire (48, warlock) pulled one Centipaar Swarmer in Tanaris
    // ("1 expected"); the Swarmer cast Silithid Swarm (spell 6589, summons of no type with one health point) every
    // one to three seconds, four were on the bot after 11 s, and it died on the run (2026-11-27). A summon on a
    // timer of five seconds or less counts twice (the next one comes before the bot is done with the last).
    _combatSummons.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT entryorguid, action_type, action_param1, event_type, event_param4 FROM "
        "smart_scripts WHERE source_type = 0 AND entryorguid > 0 AND action_type IN (11, 12) AND event_type IN (0, 2, 4)"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const summoner = uint32(fields[0].Get<int32>());
            uint32 const param = fields[2].Get<uint32>();
            uint32 summon = 0;
            uint32 count = 1;
            if (fields[1].Get<uint8>() == 12)
                summon = param;
            else if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(param))
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS && !summon; ++i)
                    if (spell->Effects[i].Effect == SPELL_EFFECT_SUMMON && spell->Effects[i].MiscValue > 0)
                    {
                        summon = uint32(spell->Effects[i].MiscValue);
                        count = uint32(std::max<int32>(1, spell->Effects[i].BasePoints + spell->Effects[i].DieSides));
                    }
            uint32 const repeatMax = fields[4].Get<uint32>();
            if (fields[3].Get<uint8>() == 0 && repeatMax >= 1 && repeatMax <= 5000)
                count = std::max<uint32>(count, 2);
            CreatureTemplate const* info = summon && summon != summoner ? sObjectMgr->GetCreatureTemplate(summon) : nullptr;
            if (!info || info->type == CREATURE_TYPE_CRITTER || info->type == CREATURE_TYPE_TOTEM ||
                info->HasFlagsExtra(CREATURE_FLAG_EXTRA_TRIGGER) ||
                (info->unit_flags & (UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_IMMUNE_TO_PC)))
                continue;
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->faction);
            if (!faction || !faction->IsHostileToPlayers())
                continue;
            auto& entry = _combatSummons[summoner];
            if (!entry.first)
                entry.first = summon;
            if (entry.first == summon)
                entry.second = uint8(std::min<uint32>(std::max<uint32>(entry.second, count), 4));
        } while (result->NextRow());
    }

    uint32 patrolPoints = 0;
    for (auto const& [entry, spawns] : _creatureSpawns)
    {
        CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(entry);
        if (!cinfo)
            continue;
        if (cinfo->type == CREATURE_TYPE_CRITTER || cinfo->HasFlagsExtra(CREATURE_FLAG_EXTRA_TRIGGER))
            continue;
        if (cinfo->unit_flags & (UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_IMMUNE_TO_PC))
            continue;
        // Elites, rares and bosses are nobody's grind target, but they stand in camps (GetStrongSpawns). So do
        // creatures without loot: the guards of the other side's towns and camps have none (Horde Guard, Splintertree
        // Guard, Tarren Mill Deathguard, Hammerfall Guardian: level 40 to 50, and the risk model did not know them).
        // And creatures with a service (a gossip, a quest, a vendor's list): a city's guards answer questions, to
        // their own side (Orgrimmar Grunt, Stormwind City Guard). Of those two kinds only what is hostile to a side
        // of players is kept, and no civilians (they attack nobody). Whom a creature attacks is decided where it
        // is counted, by faction.
        bool const strong = cinfo->rank != CREATURE_ELITE_NORMAL;
        bool const service = cinfo->npcflag != 0;
        bool const dangerOnly = strong || service || !cinfo->lootid;
        if (service || (!strong && !cinfo->lootid))
        {
            if (cinfo->HasFlagsExtra(CREATURE_FLAG_EXTRA_CIVILIAN))
                continue;
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(cinfo->faction);
            if (!faction)
                continue;
            bool hostile = (faction->hostileMask & (FACTION_MASK_PLAYER | FACTION_MASK_ALLIANCE | FACTION_MASK_HORDE)) != 0;
            for (uint32 enemy : faction->enemyFaction)
                hostile = hostile || enemy != 0;
            if (!hostile)
                continue;
        }

        for (SpawnPoint const& spawn : spawns)
        {
            MobSpawn mob;
            mob.spawn = spawn;
            mob.minLevel = cinfo->minlevel;
            mob.maxLevel = cinfo->maxlevel;
            mob.factionTemplate = cinfo->faction;
            mob.elite = strong && cinfo->rank != CREATURE_ELITE_RARE;
            // (A rare elite is no rare that is seldom there: it is one spawn, up until somebody kills it, and nobody does
            // on this server; King Mosh, counted "seldom there", killed a level-60 shaman at the plants of its quest.)
            mob.rare = cinfo->rank == CREATURE_ELITE_RARE;
            mob.flees = _fleeingEntries.count(entry) != 0;
            (dangerOnly ? _strongSpawns : _mobSpawns)[spawn.mapId].push_back(mob);

            // A creature on patrol: a point every PatrolStep yards of its path (two Horde Scouts of level 28 walk
            // through the Warsong lumber camp; the lumber piles a level-25 mage was sent to stand 40 and more yards
            // from every spawn, and on their way).
            constexpr float PatrolStep = 12.0f;
            CreatureData const* data = sObjectMgr->GetCreatureData(spawn.spawnId);
            if (!data || data->movementType != WAYPOINT_MOTION_TYPE)
                continue;
            CreatureAddon const* addon = sObjectMgr->GetCreatureAddon(spawn.spawnId);
            WaypointPath const* path = addon && addon->path_id ? sWaypointMgr->GetPath(addon->path_id) : nullptr;
            if (!path || path->Nodes.size() < 2)
                continue;
            float round = 0.0f;
            for (std::size_t i = 1; i < path->Nodes.size(); ++i)
                round += std::hypot(path->Nodes[i].X - path->Nodes[i - 1].X, path->Nodes[i].Y - path->Nodes[i - 1].Y);
            mob.patrol = true;
            mob.round = round;
            float lastX = spawn.x, lastY = spawn.y, lastZ = spawn.z;
            for (WaypointNode const& node : path->Nodes)
            {
                float const segment = std::hypot(node.X - lastX, node.Y - lastY);
                uint32 const steps = std::max<uint32>(1, uint32(segment / PatrolStep));
                for (uint32 step = 1; step <= steps; ++step)
                {
                    mob.spawn.x = lastX + (node.X - lastX) * float(step) / float(steps);
                    mob.spawn.y = lastY + (node.Y - lastY) * float(step) / float(steps);
                    mob.spawn.z = lastZ + (node.Z - lastZ) * float(step) / float(steps);
                    _patrolPoints[spawn.mapId].push_back(mob);
                    ++patrolPoints;
                }
                lastX = node.X;
                lastY = node.Y;
                lastZ = node.Z;
            }
        }
    }
    LOG_INFO("module", "mod-guild-adventure: world index: {} points of patrol paths for the risk model", patrolPoints);
    LOG_INFO("module", "mod-guild-adventure: world index: {} kinds of creature summon a minion or add by their script (counted "
        "beside them by the risk model), {} bring a hostile creature at their death, {} summon one in a fight", _minions.size(),
        _deathSummons.size(), _combatSummons.size());
}

void WorldIndex::IndexTransports()
{
    QueryResult result = WorldDatabase.Query("SELECT entry, faction, name FROM ga_transport_route");
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        uint32 const entry = fields[0].Get<uint32>();
        std::string const faction = fields[1].Get<std::string>();
        TransportTemplate const* tmpl = sTransportMgr->GetTransportTemplate(entry);
        if (!tmpl)
        {
            LOG_ERROR("module", "mod-guild-adventure: ga_transport_route {} is not a transport, skipped", entry);
            continue;
        }

        // The docks in path order; the path is a loop, so the last dock leads back to the first.
        std::vector<TransportStop> stops;
        for (KeyFrame const& frame : tmpl->keyFrames)
        {
            if (!frame.IsStopFrame())
                continue;
            TransportStop stop;
            stop.mapId = uint16(frame.Node->mapid);
            stop.x = frame.Node->x;
            stop.y = frame.Node->y;
            stop.z = frame.Node->z;
            stops.push_back(stop);
        }

        for (std::size_t i = 0; i < stops.size() && stops.size() > 1; ++i)
        {
            TravelLink link;
            link.transportEntry = entry;
            link.faction = (faction == "A" || faction == "H") ? faction[0] : 'C';
            link.name = fields[2].Get<std::string>();
            link.from = stops[i];
            link.to = stops[(i + 1) % stops.size()];
            _links.push_back(std::move(link));
        }
    } while (result->NextRow());

    // Portals: area triggers that teleport from an open-world map to another open-world spot (the Dark Portal and its
    // way back). Triggers inside one region (a city's wizard tower) fall out once the regions are known.
    for (auto const& [triggerId, teleport] : sObjectMgr->GetAllAreaTriggerTeleports())
    {
        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(triggerId);
        if (!trigger || !IsOpenWorldMap(trigger->map) || !IsOpenWorldMap(teleport.target_mapId))
            continue;
        // On the same map only long jumps (Darnassus down to Rut'theran Village), not the step into a tower.
        if (trigger->map == teleport.target_mapId)
        {
            float const dx = trigger->x - teleport.target_X;
            float const dy = trigger->y - teleport.target_Y;
            if (dx * dx + dy * dy < MinPortalJump * MinPortalJump)
                continue;
        }

        TravelLink link;
        link.areaTrigger = triggerId;
        link.triggerRadius = trigger->radius > 0.0f ? trigger->radius : std::max(trigger->length, trigger->width) / 2.0f;
        link.faction = 'C';
        link.from.mapId = uint16(trigger->map);
        link.from.x = trigger->x;
        link.from.y = trigger->y;
        link.from.z = trigger->z;
        link.to.mapId = uint16(teleport.target_mapId);
        link.to.x = teleport.target_X;
        link.to.y = teleport.target_Y;
        link.to.z = teleport.target_Z;
        _links.push_back(std::move(link));
    }

    // Teleporters players click: a game object whose spell teleports, at once or through the spell's DB script (the Orb
    // of Translocation between Silvermoon and the Undercity casts Translocate, whose spell_scripts row casts the teleport),
    // from an open-world map to a spot 300 yd or more away. Without them the Eversong Isles had no way out: a blood elf
    // levels in Eversong Woods and Ghostlands to 20, and the zones of its next levels lie behind the Silvermoon orb.
    auto teleportOf = [](uint32 spellId, SpellTargetPosition const*& target)
    {
        for (int depth = 0; depth < 3 && spellId; ++depth)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info)
                return false;
            uint32 next = 0;
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                if (info->Effects[i].Effect == SPELL_EFFECT_TELEPORT_UNITS)
                    if (SpellTargetPosition const* position = sSpellMgr->GetSpellTargetPosition(spellId, SpellEffIndex(i)))
                    {
                        target = position;
                        return true;
                    }
                if (info->Effects[i].Effect == SPELL_EFFECT_SCRIPT_EFFECT && !next)
                {
                    auto const scripts = sSpellScripts.find(spellId | (uint32(i) << 24));
                    if (scripts != sSpellScripts.end())
                        for (auto const& [delay, script] : scripts->second)
                            if (script.command == SCRIPT_COMMAND_CAST_SPELL)
                            {
                                next = script.CastSpell.SpellID;
                                break;
                            }
                }
            }
            spellId = next;
        }
        return false;
    };
    std::size_t orbs = 0;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        if (!IsOpenWorldMap(data.mapid) || !(data.phaseMask & PHASEMASK_NORMAL))
            continue;
        GameObjectTemplate const* tmpl = sObjectMgr->GetGameObjectTemplate(data.id);
        if (!tmpl)
            continue;
        uint32 const spellId = tmpl->type == GAMEOBJECT_TYPE_GOOBER ? tmpl->goober.spellId :
            tmpl->type == GAMEOBJECT_TYPE_SPELLCASTER ? tmpl->spellcaster.spellId : 0;
        if (!spellId || (tmpl->type == GAMEOBJECT_TYPE_GOOBER && (tmpl->goober.lockId || tmpl->goober.questId > 0)))
            continue;
        SpellTargetPosition const* target = nullptr;
        if (!teleportOf(spellId, target) || !IsOpenWorldMap(target->target_mapId))
            continue;
        if (target->target_mapId == data.mapid)
        {
            float const dx = data.posX - target->target_X;
            float const dy = data.posY - target->target_Y;
            if (dx * dx + dy * dy < MinPortalJump * MinPortalJump)
                continue;
        }
        // Its side by its faction: the Portals to Blasted Lands of the Horde's capitals (faction 1735) serve the Horde,
        // those of the Alliance's (1732) the Alliance. All were 'C', and an Alliance death knight on its way from Feralas
        // to the Burning Steppes walked to the one in Thunder Bluff and died twice to its Bluffwatchers, round 242.
        char side = 'C';
        if (GameObjectTemplateAddon const* addon = sObjectMgr->GetGameObjectTemplateAddon(data.id); addon && addon->faction)
        {
            bool const forAlliance = IsFriendlyFaction(addon->faction, TEAM_ALLIANCE);
            bool const forHorde = IsFriendlyFaction(addon->faction, TEAM_HORDE);
            if (!forAlliance && !forHorde)
                continue;
            side = forAlliance == forHorde ? 'C' : (forAlliance ? 'A' : 'H');
        }
        TravelLink link;
        link.gameObject = data.id;
        link.triggerRadius = 3.0f;
        link.faction = side;
        link.name = tmpl->name;
        link.from.mapId = uint16(data.mapid);
        link.from.x = data.posX;
        link.from.y = data.posY;
        link.from.z = data.posZ;
        link.to.mapId = uint16(target->target_mapId);
        link.to.x = target->target_X;
        link.to.y = target->target_Y;
        link.to.z = target->target_Z;
        _links.push_back(std::move(link));
        ++orbs;
    }
    if (orbs)
        LOG_INFO("module", "mod-guild-adventure: {} teleporters players click (an Orb of Translocation ...) are travel links", orbs);

    ResolveLinkRegions();
}

void WorldIndex::ResolveLinkRegions() const
{
    auto resolve = [](TransportStop const& stop, Region& region, std::string& name)
    {
        if (region != Region::None)
            return;
        Map* map = sMapMgr->FindBaseNonInstanceMap(stop.mapId);
        if (!map)
            return;
        uint32 zone = 0, area = 0;
        map->GetZoneAndAreaId(PHASEMASK_NORMAL, zone, area, stop.x, stop.y, stop.z);
        region = GetRegionAt(stop.mapId, stop.x, stop.y, stop.z);
        // The zone, not the area: a docked ship lies in the bay ("The Long Wash"), the zone is what players know. Rut'theran
        // Village is named itself (its zone is Teldrassil, which also names the top of the tree).
        std::string const zoneName = GetAreaName(zone);
        name = (zoneName.empty() || region == Region::RuttheranVillage) ? ToString(region) : zoneName;
    };

    for (TravelLink& link : _links)
    {
        resolve(link.from, link.fromRegion, link.fromName);
        resolve(link.to, link.toRegion, link.toName);
        if (link.areaTrigger && link.name.empty() && link.fromRegion != Region::None && link.toRegion != Region::None)
            link.name = Acore::StringFormat("the portal to {}", link.toName);
        // (Once: the regions are resolved again after the maps load.)
        if (link.gameObject && link.fromRegion != Region::None && link.toRegion != Region::None &&
            link.name.rfind("the ", 0) != 0)
            link.name = Acore::StringFormat("the {} to {}", link.name.empty() ? std::string("teleporter") : link.name, link.toName);
    }
}

namespace
{
    // Whether a ship, zeppelin, portal or teleporter serves a side: its faction, and neither end in a town of the other
    // side (RoadMap::InHostileTown: a portal of faction 0 stands in Orgrimmar, another leads into Stormwind).
    bool ServesTeam(TravelLink const& link, TeamId team)
    {
        if ((link.faction == 'A' && team != TEAM_ALLIANCE) || (link.faction == 'H' && team != TEAM_HORDE))
            return false;
        return !sRoadMap.InHostileTown(link.from.mapId, link.from.x, link.from.y, team) &&
            !sRoadMap.InHostileTown(link.to.mapId, link.to.x, link.to.y, team);
    }
}

int32 WorldIndex::CountTravelLegs(Region from, Region to, TeamId team) const
{
    if (from == to)
        return 0;

    ResolveLinkRegions();

    // Breadth-first over the regions; the graph has a handful of nodes.
    std::unordered_map<uint8, int32> legs;
    std::deque<Region> queue;
    legs[uint8(from)] = 0;
    queue.push_back(from);
    while (!queue.empty())
    {
        Region const here = queue.front();
        queue.pop_front();
        for (TravelLink const& link : _links)
        {
            if (link.fromRegion != here || link.toRegion == Region::None || link.fromRegion == link.toRegion)
                continue;
            if (!ServesTeam(link, team))
                continue;
            if (legs.count(uint8(link.toRegion)))
                continue;
            if (link.toRegion != to && IsRegionBeyondCap(link.toRegion))
                continue;
            legs[uint8(link.toRegion)] = legs[uint8(here)] + 1;
            if (link.toRegion == to)
                return legs[uint8(to)];
            queue.push_back(link.toRegion);
        }
    }
    return -1;
}

bool WorldIndex::IsRegionBeyondCap(Region region) const
{
    // Only the zones of the level cap are loaded (GuildAdventure.Zones.WithinLevelCap). A level-16 orc on its way to
    // Ghostlands took The Mighty Wind to the Borean Tundra: the way over Northrend and Dalaran's portal to Silvermoon has
    // as many legs as the zeppelin to Tirisfal and Undercity's orb, and its zeppelin tower was nearer.
    if (region != Region::Outland && region != Region::Northrend)
        return false;
    for (GuildAdventure::ZoneTemplate const& zone : sGuildAdventureMgr.GetZones())
        if (GetRegion(zone.continent, zone.zoneId) == region)
            return false;
    return true;
}

TravelLink const* WorldIndex::NextTravelLink(Region from, Region to, TeamId team, uint32 mapId, float x, float y,
    uint32 destMap, float destX, float destY) const
{
    int32 const total = CountTravelLegs(from, to, team);
    if (total <= 0)
        return nullptr;

    // The first leg is one after which the rest of the way is one leg shorter; of those, the one with the shortest
    // walk to the dock plus, for the last leg, from the arrival dock to the destination.
    TravelLink const* best = nullptr;
    float bestDistance = 0.0f;
    for (TravelLink const& link : _links)
    {
        if (link.fromRegion != from || link.toRegion == Region::None || link.toRegion == from)
            continue;
        if (!ServesTeam(link, team))
            continue;
        if (link.toRegion != to && IsRegionBeyondCap(link.toRegion))
            continue;
        if (CountTravelLegs(link.toRegion, to, team) != total - 1)
            continue;
        float const dx = link.from.x - x;
        float const dy = link.from.y - y;
        float distance = link.from.mapId == mapId ? std::sqrt(dx * dx + dy * dy) : 1e6f;
        if (total == 1)
        {
            float const ax = link.to.x - destX;
            float const ay = link.to.y - destY;
            distance += link.to.mapId == destMap ? std::sqrt(ax * ax + ay * ay) : 1e6f;
        }
        else
        {
            // The walk from its arrival dock to the dock of the next leg counts too (Ghostlands from the Barrens: the ship
            // from Ratchet lands in Booty Bay, the other end of the continent from Undercity's orb; the zeppelin from
            // Orgrimmar lands beside it).
            float next = 1e6f;
            for (TravelLink const& onward : _links)
            {
                if (onward.fromRegion != link.toRegion || onward.toRegion == Region::None || onward.toRegion == link.toRegion)
                    continue;
                if (!ServesTeam(onward, team))
                    continue;
                if (onward.toRegion != to && IsRegionBeyondCap(onward.toRegion))
                    continue;
                if (onward.from.mapId != link.to.mapId || CountTravelLegs(onward.toRegion, to, team) != total - 2)
                    continue;
                float const nx = onward.from.x - link.to.x;
                float const ny = onward.from.y - link.to.y;
                next = std::min(next, std::sqrt(nx * nx + ny * ny));
            }
            distance += next;
        }
        if (!best || distance < bestDistance)
        {
            best = &link;
            bestDistance = distance;
        }
    }
    return best;
}

void WorldIndex::ComputeHubs()
{
    // A hub lies inside its zone: the travel there ends on entering the zone, and a hub outside it would send the bot
    // there again and again. Zone lookups need the continents' base maps (their terrain loads on demand).
    for (GuildAdventure::ZoneTemplate const& zone : sGuildAdventureMgr.GetZones())
        sMapMgr->CreateBaseMap(zone.continent);

    uint32 fallbacks = 0;
    std::vector<std::string> missing;
    for (GuildAdventure::ZoneTemplate const& zone : sGuildAdventureMgr.GetZones())
    {
        for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
        {
            if (!zone.AllowsTeam(team))
                continue;

            // Every spawn inside the zone of a quest giver of this zone's quests that the team can talk to. Givers in a
            // neighbour zone do not count (the Alterac Mountains' quests start in Hillsbrad, Azshara's at the pier in
            // Tanaris): the planner reaches them from inside the zone anyway (pickups are not limited to the zone).
            std::vector<std::pair<SpawnPoint, std::string>> givers;
            std::unordered_set<ObjectGuid::LowType> seen;
            for (uint32 questId : GetZoneQuests(zone.zoneId))
            {
                auto itr = _questStarters.find(questId);
                if (itr == _questStarters.end())
                    continue;
                for (auto const& [entry, gameObject] : itr->second)
                {
                    if (gameObject)
                        continue;
                    CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(entry);
                    if (!cinfo || !IsFriendlyFaction(cinfo->faction, team))
                        continue;
                    for (SpawnPoint const& spawn : GetCreatureSpawns(entry))
                        if (spawn.mapId == zone.continent && GetSpawnZone(spawn) == zone.zoneId && seen.insert(spawn.spawnId).second)
                            givers.emplace_back(spawn, cinfo->Name);
                }
            }

            // No giver inside the zone: the zone's own mobs mark it (the densest cluster, like a grind spot). Only mobs in
            // the zone's map rectangle are looked up, the rest of the continent is skipped.
            if (givers.empty())
            {
                float x1 = 0.0f, y1 = 0.0f, x2 = 100.0f, y2 = 100.0f;
                Zone2MapCoordinates(x1, y1, zone.zoneId);
                Zone2MapCoordinates(x2, y2, zone.zoneId);
                float const minX = std::min(x1, x2), maxX = std::max(x1, x2), minY = std::min(y1, y2), maxY = std::max(y1, y2);
                for (MobSpawn const& mob : GetMobSpawns(zone.continent))
                    if (mob.spawn.x >= minX && mob.spawn.x <= maxX && mob.spawn.y >= minY && mob.spawn.y <= maxY &&
                        GetSpawnZone(mob.spawn) == zone.zoneId)
                        if (CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(mob.spawn.entry))
                            givers.emplace_back(mob.spawn, cinfo->Name);
                if (!givers.empty())
                    ++fallbacks;
            }

            if (givers.empty())
            {
                missing.push_back(Acore::StringFormat("{} ({})", zone.name, team == TEAM_HORDE ? "Horde" : "Alliance"));
                continue;
            }

            // The giver with the most other givers around it marks the hub (a walkable NPC spot, not a centroid
            // that could lie inside a wall). Ties go to the lowest spawn id, so the hub is stable across restarts.
            std::size_t best = 0;
            uint32 bestCount = 0;
            for (std::size_t i = 0; i < givers.size(); ++i)
            {
                uint32 count = 0;
                for (std::size_t j = 0; j < givers.size(); ++j)
                {
                    float const dx = givers[i].first.x - givers[j].first.x;
                    float const dy = givers[i].first.y - givers[j].first.y;
                    if (dx * dx + dy * dy <= HubClusterRadius * HubClusterRadius)
                        ++count;
                }
                if (count > bestCount || (count == bestCount && givers[i].first.spawnId < givers[best].first.spawnId))
                {
                    best = i;
                    bestCount = count;
                }
            }

            ZoneHub hub;
            hub.mapId = givers[best].first.mapId;
            hub.x = givers[best].first.x;
            hub.y = givers[best].first.y;
            hub.z = givers[best].first.z;
            hub.name = givers[best].second;
            _hubs[(uint64(zone.zoneId) << 1) | uint64(team == TEAM_HORDE ? 1 : 0)] = hub;
        }
    }

    if (fallbacks)
        LOG_INFO("module", "mod-guild-adventure: {} zone hubs marked by the zone's mobs (no quest giver inside the zone)", fallbacks);
    for (std::string const& zone : missing)
        LOG_WARN("module", "mod-guild-adventure: no hub for {}: no quest giver and no mob inside the zone", zone);
}

std::vector<SpawnPoint> const& WorldIndex::GetCreatureSpawns(uint32 entry) const
{
    auto itr = _creatureSpawns.find(entry);
    return itr != _creatureSpawns.end() ? itr->second : EmptySpawns;
}

std::vector<SpawnPoint> const& WorldIndex::GetGameObjectSpawns(uint32 entry) const
{
    auto itr = _goSpawns.find(entry);
    return itr != _goSpawns.end() ? itr->second : EmptySpawns;
}

std::vector<uint32> const& WorldIndex::GetZoneQuests(uint32 zoneId) const
{
    auto itr = _zoneQuests.find(zoneId);
    return itr != _zoneQuests.end() ? itr->second : EmptyIds;
}

uint32 WorldIndex::GetQuestZone(uint32 questId) const
{
    auto itr = _questZone.find(questId);
    return itr != _questZone.end() ? itr->second : 0;
}

std::vector<SpawnPoint> WorldIndex::SpawnsOf(std::vector<std::pair<uint32, bool>> const& entries) const
{
    std::vector<SpawnPoint> result;
    for (auto const& [entry, gameObject] : entries)
    {
        std::vector<SpawnPoint> const& spawns = gameObject ? GetGameObjectSpawns(entry) : GetCreatureSpawns(entry);
        result.insert(result.end(), spawns.begin(), spawns.end());
    }
    return result;
}

std::vector<SpawnPoint> WorldIndex::GetQuestStarters(uint32 questId) const
{
    auto itr = _questStarters.find(questId);
    return itr != _questStarters.end() ? SpawnsOf(itr->second) : std::vector<SpawnPoint>();
}

std::vector<SpawnPoint> WorldIndex::GetQuestEnders(uint32 questId) const
{
    auto itr = _questEnders.find(questId);
    return itr != _questEnders.end() ? SpawnsOf(itr->second) : std::vector<SpawnPoint>();
}

bool WorldIndex::HasCreatureSources(uint32 itemId) const
{
    auto itr = _itemSources.find(itemId);
    return itr != _itemSources.end() &&
        std::any_of(itr->second.begin(), itr->second.end(), [](std::pair<uint32, bool> const& source) { return !source.second; });
}

namespace
{
    uint64 SourceKey(uint32 item, uint32 entry, bool gameObject)
    {
        return (uint64(item) << 32) | (uint64(entry) << 1) | (gameObject ? 1 : 0);
    }

    // A source with the item less than a fifth as often as the best one is no source.
    constexpr float LowSourceFactor = 5.0f;
}

std::vector<uint32> WorldIndex::GetItemSourceCreatures(uint32 itemId) const
{
    std::vector<uint32> result;
    auto itr = _itemSources.find(itemId);
    if (itr == _itemSources.end())
        return result;
    float best = 0.0f;
    for (auto const& [entry, gameObject] : itr->second)
        best = std::max(best, GetItemSourceChance(itemId, entry, gameObject));
    for (auto const& [entry, gameObject] : itr->second)
    {
        float const chance = GetItemSourceChance(itemId, entry, gameObject);
        if (!gameObject && (best <= 0.0f || chance <= 0.0f || chance * LowSourceFactor >= best))
            result.push_back(entry);
    }
    return result;
}

float WorldIndex::GetItemSourceChance(uint32 itemId, uint32 entry, bool gameObject) const
{
    auto itr = _itemSourceChance.find(SourceKey(itemId, entry, gameObject));
    return itr != _itemSourceChance.end() ? itr->second : 0.0f;
}

std::vector<SpawnPoint> WorldIndex::GetItemSources(uint32 itemId) const
{
    auto itr = _itemSources.find(itemId);
    if (itr == _itemSources.end())
        return {};
    // Not from a source that hardly ever has the item while another has it for certain. The Wyrmtail of "Catch a
    // Dragon by the Tail" lies on the ground of the Wyrmbog (71 objects, 100 %), and the whelps and daggermaws there
    // carry one in a hundred kills: the planner took the nearest source, and a paladin killed 25 of them for nothing.
    float best = 0.0f;
    for (auto const& [entry, gameObject] : itr->second)
        best = std::max(best, GetItemSourceChance(itemId, entry, gameObject));
    if (best <= 0.0f)
        return SpawnsOf(itr->second);
    std::vector<std::pair<uint32, bool>> kept;
    for (auto const& source : itr->second)
    {
        float const chance = GetItemSourceChance(itemId, source.first, source.second);
        if (chance <= 0.0f || chance * LowSourceFactor >= best)
            kept.push_back(source);
    }
    return SpawnsOf(kept);
}

bool WorldIndex::FindLocalPortal(uint32 mapId, float x, float y, float z, float range, float& targetX, float& targetY,
    float& targetZ, float& targetO) const
{
    float best = range * range;
    bool found = false;
    for (auto const& [triggerId, teleport] : sObjectMgr->GetAllAreaTriggerTeleports())
    {
        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(triggerId);
        if (!trigger || trigger->map != mapId || teleport.target_mapId != mapId)
            continue;
        float const dx = trigger->x - x, dy = trigger->y - y, dz = trigger->z - z;
        float const distance = dx * dx + dy * dy + dz * dz;
        if (distance > best)
            continue;
        best = distance;
        found = true;
        targetX = teleport.target_X;
        targetY = teleport.target_Y;
        targetZ = teleport.target_Z;
        targetO = teleport.target_Orientation;
    }
    return found;
}

bool WorldIndex::FindLocalPortalInto(uint32 mapId, float x, float y, float z, float range, float height, LocalPortal& portal) const
{
    constexpr float MinJump = 30.0f;   // a teleporter moves its user at least this far
    constexpr float MaxJump = 300.0f;  // and less than this (the longer ones are travel links)
    float best = range * range;
    bool found = false;
    for (auto const& [triggerId, teleport] : sObjectMgr->GetAllAreaTriggerTeleports())
    {
        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(triggerId);
        if (!trigger || trigger->map != mapId || teleport.target_mapId != mapId || std::fabs(teleport.target_Z - z) > height)
            continue;
        float const dx = teleport.target_X - x, dy = teleport.target_Y - y;
        float const distance = dx * dx + dy * dy;
        if (distance > best)
            continue;
        float const jx = teleport.target_X - trigger->x, jy = teleport.target_Y - trigger->y, jz = teleport.target_Z - trigger->z;
        float const jump = std::sqrt(jx * jx + jy * jy + jz * jz);
        if (jump < MinJump || jump > MaxJump)
            continue;
        best = distance;
        found = true;
        portal.x = trigger->x;
        portal.y = trigger->y;
        portal.z = trigger->z;
        portal.radius = std::max(2.0f, trigger->radius > 0.0f ? trigger->radius : std::min(trigger->length, trigger->width) * 0.5f);
        portal.targetX = teleport.target_X;
        portal.targetY = teleport.target_Y;
        portal.targetZ = teleport.target_Z;
        portal.targetO = teleport.target_Orientation;
    }
    return found;
}

bool WorldIndex::FindLocalPortalFrom(uint32 mapId, float x, float y, float z, float range, LocalPortal& portal) const
{
    constexpr float MinJump = 30.0f;
    constexpr float MaxJump = 300.0f;
    float best = range * range;
    bool found = false;
    for (auto const& [triggerId, teleport] : sObjectMgr->GetAllAreaTriggerTeleports())
    {
        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(triggerId);
        if (!trigger || trigger->map != mapId || teleport.target_mapId != mapId)
            continue;
        float const dx = trigger->x - x, dy = trigger->y - y, dz = trigger->z - z;
        float const distance = dx * dx + dy * dy + dz * dz;
        if (distance > best)
            continue;
        float const jx = teleport.target_X - trigger->x, jy = teleport.target_Y - trigger->y, jz = teleport.target_Z - trigger->z;
        float const jump = std::sqrt(jx * jx + jy * jy + jz * jz);
        if (jump < MinJump || jump > MaxJump)
            continue;
        best = distance;
        found = true;
        portal.x = trigger->x;
        portal.y = trigger->y;
        portal.z = trigger->z;
        portal.radius = std::max(2.0f, trigger->radius > 0.0f ? trigger->radius : std::min(trigger->length, trigger->width) * 0.5f);
        portal.targetX = teleport.target_X;
        portal.targetY = teleport.target_Y;
        portal.targetZ = teleport.target_Z;
        portal.targetO = teleport.target_Orientation;
    }
    return found;
}

std::vector<uint32> const& WorldIndex::GetQuestsRewarding(uint32 itemId) const
{
    auto itr = _itemRewardQuests.find(itemId);
    return itr != _itemRewardQuests.end() ? itr->second : EmptyIds;
}

std::vector<SpawnPoint> WorldIndex::GetItemVendors(uint32 itemId) const
{
    auto itr = _itemVendors.find(itemId);
    return itr != _itemVendors.end() ? SpawnsOf(itr->second) : std::vector<SpawnPoint>();
}

std::vector<ServicePoint> const& WorldIndex::GetServices(ServiceType type) const
{
    std::size_t const index = std::min<std::size_t>(uint8(type), ServiceTypeCount - 1);
    return _services[index];
}

ZoneHub const* WorldIndex::GetHub(uint32 zoneId, TeamId team) const
{
    auto itr = _hubs.find((uint64(zoneId) << 1) | uint64(team == TEAM_HORDE ? 1 : 0));
    return itr != _hubs.end() ? &itr->second : nullptr;
}

std::vector<MobSpawn> const& WorldIndex::GetMobSpawns(uint32 mapId) const
{
    static std::vector<MobSpawn> const empty;
    auto itr = _mobSpawns.find(mapId);
    return itr != _mobSpawns.end() ? itr->second : empty;
}

std::vector<MobSpawn> const& WorldIndex::GetStrongSpawns(uint32 mapId) const
{
    static std::vector<MobSpawn> const empty;
    auto itr = _strongSpawns.find(mapId);
    return itr != _strongSpawns.end() ? itr->second : empty;
}

std::vector<MobSpawn> const& WorldIndex::GetPatrolPoints(uint32 mapId) const
{
    static std::vector<MobSpawn> const empty;
    auto itr = _patrolPoints.find(mapId);
    return itr != _patrolPoints.end() ? itr->second : empty;
}

void WorldIndex::LookupSpawn(SpawnPoint const& spawn, uint32& zone, uint32& area) const
{
    zone = area = 0;
    uint64 const key = (uint64(spawn.spawnId) << 1) | uint64(spawn.gameObject ? 1 : 0);
    auto itr = _spawnZones.find(key);
    if (itr != _spawnZones.end())
    {
        zone = itr->second.first;
        area = itr->second.second;
        return;
    }

    // Only maps that exist already: the planner asks for spawns on the bot's own map, which is loaded.
    Map* map = sMapMgr->FindBaseNonInstanceMap(spawn.mapId);
    if (!map)
        return;

    map->GetZoneAndAreaId(PHASEMASK_NORMAL, zone, area, spawn.x, spawn.y, spawn.z);
    _spawnZones[key] = { zone, area };
}

uint32 WorldIndex::CountHostileSpawns(Player const* player, uint32 mapId, float x, float y, float z, float radius) const
{
    FactionTemplateEntry const* own = player ? player->GetFactionTemplateEntry() : nullptr;
    if (!own)
        return 0;
    uint32 count = 0;
    for (std::vector<MobSpawn> const* spawns : { &GetMobSpawns(mapId), &GetStrongSpawns(mapId) })
        for (MobSpawn const& mob : *spawns)
        {
            float const dx = mob.spawn.x - x, dy = mob.spawn.y - y;
            if (dx * dx + dy * dy > radius * radius || std::fabs(mob.spawn.z - z) > 30.0f)
                continue;
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(mob.factionTemplate);
            if (faction && faction->IsHostileTo(*own))
                ++count;
        }
    return count;
}

uint32 WorldIndex::GetSpawnZone(SpawnPoint const& spawn) const
{
    uint32 zone, area;
    LookupSpawn(spawn, zone, area);
    return zone;
}

uint32 WorldIndex::GetSpawnArea(SpawnPoint const& spawn) const
{
    uint32 zone, area;
    LookupSpawn(spawn, zone, area);
    return area;
}

Region WorldIndex::GetSpawnRegion(SpawnPoint const& spawn) const
{
    return GetRegionAt(spawn.mapId, spawn.x, spawn.y, spawn.z);
}

bool WorldIndex::IsFriendlyFaction(uint32 factionTemplateId, Player const* player)
{
    FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(factionTemplateId);
    FactionTemplateEntry const* own = player ? player->GetFactionTemplateEntry() : nullptr;
    if (!faction || !own)
        return false;
    return !faction->IsHostileTo(*own);
}

bool WorldIndex::IsFriendlyFaction(uint32 factionTemplateId, TeamId team)
{
    FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(factionTemplateId);
    FactionTemplateEntry const* own = sFactionTemplateStore.LookupEntry(team == TEAM_HORDE ? HordeFactionTemplate : AllianceFactionTemplate);
    if (!faction || !own)
        return false;
    return !faction->IsHostileTo(*own);
}

bool WorldIndex::IsHuntMaterial(ItemTemplate const* proto)
{
    if (!proto || proto->Class != ITEM_CLASS_TRADE_GOODS)
        return false;
    switch (proto->SubClass)
    {
        // What a gathering profession brings (herbs, ore and stone, leather and hides), what disenchanting gives and
        // gems: the world database has stray loot rows for them (Briarthorn on four "Defias Drones" at 21 %).
        case ITEM_SUBCLASS_HERB:
        case ITEM_SUBCLASS_METAL_STONE:
        case ITEM_SUBCLASS_LEATHER:
        case ITEM_SUBCLASS_ENCHANTING:
        case ITEM_SUBCLASS_JEWELCRAFTING:
            return false;
        default:
            return true;
    }
}

bool WorldIndex::IsAttackableFaction(uint32 factionTemplateId, Player const* player)
{
    FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(factionTemplateId);
    FactionTemplateEntry const* own = player ? player->GetFactionTemplateEntry() : nullptr;
    if (!faction || !own)
        return false;
    return !faction->IsFriendlyTo(*own);
}

uint32 WorldIndex::MinionOf(uint32 creatureEntry) const
{
    auto itr = _minions.find(creatureEntry);
    return itr == _minions.end() ? 0 : itr->second;
}

std::pair<uint32, uint8> WorldIndex::DeathSummonOf(uint32 creatureEntry) const
{
    auto itr = _deathSummons.find(creatureEntry);
    return itr == _deathSummons.end() ? std::pair<uint32, uint8>{ 0, 0 } : itr->second;
}

std::pair<uint32, uint8> WorldIndex::CombatSummonOf(uint32 creatureEntry) const
{
    auto itr = _combatSummons.find(creatureEntry);
    return itr == _combatSummons.end() ? std::pair<uint32, uint8>{ 0, 0 } : itr->second;
}
