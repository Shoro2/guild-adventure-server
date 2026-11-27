/*
 * Dungeons: see Dungeons.h.
 */

#include "Dungeons.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "QueryResult.h"
#include "Readiness.h"
#include "StringFormat.h"
#include <unordered_map>
#include "Optional.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include <algorithm>
#include <unordered_map>

using namespace GuildAdventure;

Dungeons& Dungeons::Instance()
{
    static Dungeons instance;
    return instance;
}

void Dungeons::Load()
{
    _plans.clear();

    // How many dungeons of the list share a map.
    std::unordered_map<uint32, uint32> perMap;
    for (DungeonInfo const& dungeon : sReadiness.GetDungeons())
        if (!dungeon.raid)
            ++perMap[dungeon.mapId];

    uint32 supported = 0;
    for (DungeonInfo const& dungeon : sReadiness.GetDungeons())
    {
        DungeonPlan plan;
        plan.id = dungeon.id;
        plan.mapId = dungeon.mapId;
        plan.name = dungeon.name;
        plan.raid = dungeon.raid;
        plan.size = dungeon.size;

        // The entrance: an area trigger on an open-world map that leads onto the dungeon's map. (A map with wings has
        // one per wing: the Scarlet Monastery's four doors. Which one is this dungeon's is found with its end boss.)
        struct Entrance
        {
            uint32 outMap = 0;
            float outX = 0.0f, outY = 0.0f, outZ = 0.0f, inX = 0.0f, inY = 0.0f, inZ = 0.0f, inO = 0.0f;
            uint32 trigger = 0;
        };
        std::vector<Entrance> entrances;
        bool const wings = perMap[dungeon.mapId] > 1;
        std::string wingWhy;
        bool entrance = false;
        if (QueryResult result = WorldDatabase.Query("SELECT a.map, a.x, a.y, a.z, t.target_position_x, t.target_position_y, "
            "t.target_position_z, t.target_orientation, t.ID FROM areatrigger_teleport t JOIN areatrigger a ON a.entry = t.ID "
            "WHERE t.target_map = {} ORDER BY t.ID", dungeon.mapId))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const outMap = fields[0].Get<uint32>();
                MapEntry const* map = sMapStore.LookupEntry(outMap);
                if (!map || map->IsDungeon() || outMap == dungeon.mapId)
                    continue;
                plan.outMap = outMap;
                plan.outX = fields[1].Get<float>();
                plan.outY = fields[2].Get<float>();
                plan.outZ = fields[3].Get<float>();
                plan.inX = fields[4].Get<float>();
                plan.inY = fields[5].Get<float>();
                plan.inZ = fields[6].Get<float>();
                plan.inO = fields[7].Get<float>();
                entrances.push_back({ outMap, plan.outX, plan.outY, plan.outZ, plan.inX, plan.inY, plan.inZ, plan.inO, fields[8].Get<uint32>() });
                entrance = true;
            } while ((wings || !entrance) && result->NextRow());
        }
        // A raid whose only entrance lies inside another dungeon (Blackwing Lair's, in Upper Blackrock Spire): the way in
        // is that trigger's (the run takes its group in by teleport), the place outside is that dungeon's own entrance.
        if (!entrance && dungeon.raid)
            if (QueryResult result = WorldDatabase.Query("SELECT a.map, t.target_position_x, t.target_position_y, t.target_position_z, "
                "t.target_orientation FROM areatrigger_teleport t JOIN areatrigger a ON a.entry = t.ID WHERE t.target_map = {} ORDER BY t.ID",
                dungeon.mapId))
            {
                Field* fields = result->Fetch();
                uint32 const through = fields[0].Get<uint32>();
                if (QueryResult outside = WorldDatabase.Query("SELECT a.map, a.x, a.y, a.z FROM areatrigger_teleport t JOIN areatrigger a ON "
                    "a.entry = t.ID WHERE t.target_map = {} ORDER BY t.ID", through))
                    do
                    {
                        Field* door = outside->Fetch();
                        MapEntry const* map = sMapStore.LookupEntry(door[0].Get<uint32>());
                        if (!map || map->IsDungeon())
                            continue;
                        plan.outMap = door[0].Get<uint32>();
                        plan.outX = door[1].Get<float>();
                        plan.outY = door[2].Get<float>();
                        plan.outZ = door[3].Get<float>();
                        plan.inX = fields[1].Get<float>();
                        plan.inY = fields[2].Get<float>();
                        plan.inZ = fields[3].Get<float>();
                        plan.inO = fields[4].Get<float>();
                        entrance = true;
                    } while (!entrance && outside->NextRow());
            }

        // The bosses: the dungeon's encounters that are a creature to kill and that are spawned. (A raid's end boss that the
        // instance summons, Ragnaros: an event stop of the template brings it, and it ends the raid.)
        std::vector<uint32> endUnspawned;
        if (DungeonEncounterList const* encounters = sObjectMgr->GetDungeonEncounterList(dungeon.mapId, Difficulty(0)))
        {
            if (dungeon.raid)
                for (DungeonEncounter const* encounter : *encounters)
                    if (encounter && encounter->creditType == ENCOUNTER_CREDIT_KILL_CREATURE && encounter->lastEncounterDungeon)
                        endUnspawned.push_back(encounter->creditEntry);
            std::vector<std::pair<uint32, DungeonBoss>> found; // (encounter id, boss)
            for (DungeonEncounter const* encounter : *encounters)
            {
                if (!encounter || encounter->creditType != ENCOUNTER_CREDIT_KILL_CREATURE || !encounter->creditEntry)
                    continue;
                CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(encounter->creditEntry);
                if (!proto)
                    continue;
                for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
                {
                    if (data.mapid != dungeon.mapId || data.id != encounter->creditEntry)
                        continue;
                    DungeonBoss boss;
                    boss.entry = encounter->creditEntry;
                    boss.spawnId = spawnId;
                    boss.name = proto->Name;
                    boss.x = data.posX;
                    boss.y = data.posY;
                    boss.z = data.posZ;
                    boss.last = encounter->lastEncounterDungeon != 0;
                    boss.lastDungeon = encounter->lastEncounterDungeon;
                    found.emplace_back(encounter->dbcEntry ? encounter->dbcEntry->id : 0, std::move(boss));
                    break;
                }
            }
            std::sort(found.begin(), found.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
            for (auto& [id, boss] : found)
                if (std::none_of(plan.bosses.begin(), plan.bosses.end(), [&](DungeonBoss const& other) { return other.entry == boss.entry; }))
                    plan.bosses.push_back(std::move(boss));
            // The boss that ends the dungeon comes last, whatever its encounter id (the Stockade lists Dextren Ward
            // after Bazil Thredd, Razorfen Kraul Agathelos after Charlga).
            std::stable_partition(plan.bosses.begin(), plan.bosses.end(), [](DungeonBoss const& boss) { return !boss.last; });
        }

        // A wing of a map with several dungeons: the boss that ends this dungeon names it. Its entrance is the one whose
        // inside lies nearest to that boss, and its bosses are those that lie nearest to that entrance.
        if (wings)
        {
            auto nearest = [&](DungeonBoss const& boss)
            {
                std::size_t best = entrances.size();
                float bestDistance = 0.0f;
                for (std::size_t i = 0; i < entrances.size(); ++i)
                {
                    float const dx = entrances[i].inX - boss.x, dy = entrances[i].inY - boss.y;
                    float const distance = dx * dx + dy * dy;
                    if (best == entrances.size() || distance < bestDistance)
                    {
                        best = i;
                        bestDistance = distance;
                    }
                }
                return best;
            };
            auto end = std::find_if(plan.bosses.begin(), plan.bosses.end(), [&](DungeonBoss const& boss) { return boss.lastDungeon == dungeon.id; });
            // A hand list where that rule is wrong (ga_dungeon_wing_template: Maraudon's purple wing lies nearer to
            // the orange entrance): the bosses of the wing and the area trigger of its entrance.
            std::vector<uint32> listed;
            uint32 listedTrigger = 0;
            if (QueryResult rows = WorldDatabase.Query("SELECT boss_entry, entrance_trigger FROM ga_dungeon_wing_template WHERE dungeon_id = {}",
                    dungeon.id))
                do
                {
                    listed.push_back(rows->Fetch()[0].Get<uint32>());
                    listedTrigger = rows->Fetch()[1].Get<uint32>();
                } while (rows->NextRow());
            if (entrances.empty())
                entrance = false;
            // (A wing whose end boss has no spawn, Stratholme's Main Gate: Balnazzar comes out of Grand Crusader
            // Dathrohan. Its hand list names the entrance, and every listed boss counts for it.)
            else if (end == plan.bosses.end() && (listed.empty() || !listedTrigger))
                wingWhy = "its map holds several dungeons and no spawned boss ends this one";
            else
            {
                std::size_t own = end != plan.bosses.end() ? nearest(*end) : entrances.size();
                for (std::size_t i = 0; i < entrances.size(); ++i)
                    if (listedTrigger && entrances[i].trigger == listedTrigger)
                        own = i;
                if (own == entrances.size())
                    wingWhy = "its hand list names an entrance that its map does not have";
                else
                {
                    plan.bosses.erase(std::remove_if(plan.bosses.begin(), plan.bosses.end(), [&](DungeonBoss const& boss)
                    {
                        return listed.empty() ? nearest(boss) != own : std::find(listed.begin(), listed.end(), boss.entry) == listed.end();
                    }), plan.bosses.end());
                    for (DungeonBoss& boss : plan.bosses)
                        boss.last = boss.lastDungeon == dungeon.id;
                    std::stable_partition(plan.bosses.begin(), plan.bosses.end(), [](DungeonBoss const& boss) { return !boss.last; });
                    Entrance const& door = entrances[own];
                    plan.outMap = door.outMap;
                    plan.outX = door.outX;
                    plan.outY = door.outY;
                    plan.outZ = door.outZ;
                    plan.inX = door.inX;
                    plan.inY = door.inY;
                    plan.inZ = door.inZ;
                    plan.inO = door.inO;
                }
            }
        }

        // Where the dungeon finder puts a group in (lfg_dungeon_template), when it names a place: a run's teleports take
        // its group there too (in, and back after a wipe). Maraudon's Pristine Waters starts at Earth Song Falls in the
        // finder; the purple wing's door left the owner's group 1750 yd and every pack of the way before Landslide, 2026-11-22.
        if (entrance)
            if (QueryResult finder = WorldDatabase.Query("SELECT position_x, position_y, position_z, orientation FROM lfg_dungeon_template "
                "WHERE dungeonId = {}", dungeon.id))
            {
                Field* fields = finder->Fetch();
                float const x = fields[0].Get<float>(), y = fields[1].Get<float>(), z = fields[2].Get<float>();
                if (x != 0.0f || y != 0.0f || z != 0.0f)
                {
                    plan.inX = x;
                    plan.inY = y;
                    plan.inZ = z;
                    plan.inO = fields[3].Get<float>();
                }
            }

        // A dungeon that is no wing can have a hand list too: bosses that are left out (Ironaya in Uldaman stands
        // behind a sealed door that an event with a staff opens, and kills the tank through it).
        std::vector<uint32> listed;
        std::unordered_map<uint32, uint32> listedBefore; // a listed boss without an encounter -> the boss it comes before
        if (QueryResult rows = WorldDatabase.Query("SELECT boss_entry, before_boss FROM ga_dungeon_wing_template WHERE dungeon_id = {}",
                dungeon.id))
            do
            {
                listed.push_back(rows->Fetch()[0].Get<uint32>());
                if (uint32 const before = rows->Fetch()[1].Get<uint32>())
                    listedBefore[listed.back()] = before;
            } while (rows->NextRow());
        if (!wings && !listed.empty())
            plan.bosses.erase(std::remove_if(plan.bosses.begin(), plan.bosses.end(), [&](DungeonBoss const& boss)
            {
                return std::find(listed.begin(), listed.end(), boss.entry) == listed.end();
            }), plan.bosses.end());
        // ... and a listed boss that has no encounter of its own is added, before the boss that ends the dungeon
        // (Scarlet Commander Mograine: Whitemane comes out when he falls, and the group had gone straight to her), or
        // before the boss its row names (the six defenders on the balcony of the Sunken Temple: Jammal'an stands
        // behind a force field until they are dead).
        if (entrance)
            for (uint32 entry : listed)
            {
                if (std::any_of(plan.bosses.begin(), plan.bosses.end(), [entry](DungeonBoss const& boss) { return boss.entry == entry; }))
                    continue;
                CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(entry);
                if (!proto)
                    continue;
                for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
                {
                    if (data.mapid != dungeon.mapId || data.id != entry)
                        continue;
                    DungeonBoss boss;
                    boss.entry = entry;
                    boss.spawnId = spawnId;
                    boss.name = proto->Name;
                    boss.x = data.posX;
                    boss.y = data.posY;
                    boss.z = data.posZ;
                    auto const before = listedBefore.find(entry);
                    auto at = before == listedBefore.end() ? plan.bosses.end() :
                        std::find_if(plan.bosses.begin(), plan.bosses.end(), [&](DungeonBoss const& other) { return other.entry == before->second; });
                    if (at == plan.bosses.end())
                        at = std::find_if(plan.bosses.begin(), plan.bosses.end(), [](DungeonBoss const& other) { return other.last; });
                    if (at != plan.bosses.end())
                        boss.before = at->entry;
                    plan.bosses.insert(at, std::move(boss));
                    break;
                }
            }

        // Bosses that an event brings (Mutanus the Devourer, Grubbis): a stop at the NPC that starts the event.
        if (QueryResult events = WorldDatabase.Query("SELECT before_boss, npc_entry, gossip_option, boss_entry, follow, timeout_s, go_entries, "
                "area_trigger, x, y, z, gossip_steps FROM ga_dungeon_event_template WHERE dungeon_id = {} ORDER BY boss_entry", dungeon.id))
        {
            do
            {
                Field* fields = events->Fetch();
                uint32 const beforeBoss = fields[0].Get<uint32>();
                // No NPC to talk to: objects to use before a boss of the list can be fought, or, for a boss that has
                // no spawn, the object that brings it (a stop of its own at the object's place).
                if (!fields[1].Get<uint32>())
                {
                    std::string const objects = fields[6].Get<std::string>();
                    std::vector<uint32> entries;
                    for (std::string_view token : Acore::Tokenize(objects, ',', false))
                        if (Optional<uint32> entry = Acore::StringTo<uint32>(token))
                            entries.push_back(*entry);
                    bool listed = false;
                    for (DungeonBoss& boss : plan.bosses)
                        if (boss.entry == fields[3].Get<uint32>())
                        {
                            listed = true;
                            boss.useObjects.insert(boss.useObjects.end(), entries.begin(), entries.end());
                            if (uint32 const trigger = fields[7].Get<uint32>())
                            {
                                boss.areaTrigger = trigger;
                                boss.timeout = fields[5].Get<uint32>();
                            }
                        }
                    // A boss the instance brings by itself at a place (Majordomo Executus): a stop there, after the boss it
                    // comes before.
                    float const atX = fields[8].Get<float>(), atY = fields[9].Get<float>(), atZ = fields[10].Get<float>();
                    if (!listed && entries.empty() && (atX != 0.0f || atY != 0.0f))
                    {
                        DungeonBoss stop;
                        stop.entry = fields[3].Get<uint32>();
                        stop.summoned = true;
                        stop.timeout = fields[5].Get<uint32>();
                        stop.x = atX;
                        stop.y = atY;
                        stop.z = atZ;
                        CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(stop.entry);
                        if (!proto)
                        {
                            LOG_ERROR("module.ga", "mod-guild-adventure: ga_dungeon_event_template: dungeon {}: the boss {} is not known, it is "
                                "left out", dungeon.id, stop.entry);
                            continue;
                        }
                        stop.name = proto->Name;
                        stop.last = std::find(endUnspawned.begin(), endUnspawned.end(), stop.entry) != endUnspawned.end();
                        auto where = std::find_if(plan.bosses.begin(), plan.bosses.end(), [&](DungeonBoss const& boss) { return beforeBoss && boss.entry == beforeBoss; });
                        if (where != plan.bosses.end())
                            stop.before = where->entry;
                        plan.bosses.insert(where, std::move(stop));
                        continue;
                    }
                    if (listed || entries.empty())
                        continue;
                    DungeonBoss stop;
                    stop.summonObject = entries.front();
                    stop.entry = fields[3].Get<uint32>();
                    stop.timeout = fields[5].Get<uint32>();
                    CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(stop.entry);
                    bool placed = false;
                    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
                        if (data.mapid == dungeon.mapId && data.id == stop.summonObject)
                        {
                            stop.x = data.posX;
                            stop.y = data.posY;
                            stop.z = data.posZ;
                            placed = true;
                            break;
                        }
                    if (!proto || !placed)
                    {
                        LOG_ERROR("module.ga", "mod-guild-adventure: ga_dungeon_event_template: dungeon {}: the boss {} or a spawn of the "
                            "object {} is not known, the event is left out", dungeon.id, stop.entry, stop.summonObject);
                        continue;
                    }
                    stop.name = proto->Name;
                    auto where = std::find_if(plan.bosses.begin(), plan.bosses.end(), [&](DungeonBoss const& boss) { return beforeBoss && boss.entry == beforeBoss; });
                    if (where != plan.bosses.end())
                        stop.before = where->entry;
                    plan.bosses.insert(where, std::move(stop));
                    continue;
                }
                DungeonBoss stop;
                stop.talkEntry = fields[1].Get<uint32>();
                stop.talkOption = fields[2].Get<uint32>();
                stop.entry = fields[3].Get<uint32>();
                stop.follow = fields[4].Get<bool>();
                stop.timeout = fields[5].Get<uint32>();
                stop.talkSteps = std::max<uint8>(fields[11].Get<uint8>(), 1);
                stop.last = std::find(endUnspawned.begin(), endUnspawned.end(), stop.entry) != endUnspawned.end();
                CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(stop.entry);
                bool placed = false;
                for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
                    if (data.mapid == dungeon.mapId && data.id == stop.talkEntry)
                    {
                        stop.x = data.posX;
                        stop.y = data.posY;
                        stop.z = data.posZ;
                        placed = true;
                        break;
                    }
                // (An NPC that the instance brings, Majordomo Executus in the lair of Ragnaros: the template gives its place.)
                if (!placed && (fields[8].Get<float>() != 0.0f || fields[9].Get<float>() != 0.0f))
                {
                    stop.x = fields[8].Get<float>();
                    stop.y = fields[9].Get<float>();
                    stop.z = fields[10].Get<float>();
                    placed = true;
                }
                if (!proto || !placed)
                {
                    LOG_ERROR("module.ga", "mod-guild-adventure: ga_dungeon_event_template: dungeon {}: the boss {} or a spawn of the NPC {} "
                        "is not known, the event is left out", dungeon.id, stop.entry, stop.talkEntry);
                    continue;
                }
                stop.name = proto->Name;
                // The way it walks (SmartAI waypoints of the NPC), for a group that goes with it.
                if (stop.follow)
                    if (QueryResult points = WorldDatabase.Query("SELECT position_x, position_y, position_z FROM waypoints WHERE entry = {} "
                            "ORDER BY pointid", stop.talkEntry))
                        do
                        {
                            Field* point = points->Fetch();
                            stop.route.push_back({ point[0].Get<float>(), point[1].Get<float>(), point[2].Get<float>() });
                        } while (points->NextRow());
                auto where = std::find_if(plan.bosses.begin(), plan.bosses.end(), [&](DungeonBoss const& boss) { return beforeBoss && boss.entry == beforeBoss; });
                if (where != plan.bosses.end())
                    stop.before = where->entry;
                plan.bosses.insert(where, std::move(stop));
            } while (events->NextRow());
        }

        // What holds a boss back (DungeonBoss::clearFirst): the SmartAI conditions "no living creature X within N yards"
        // of its own events (SourceId 0), and of the objects of useObjects (SourceId 1), which then work by themselves.
        auto heldBy = [](uint32 entry, uint32 sourceId)
        {
            std::vector<std::pair<uint32, float>> held;
            if (QueryResult rows = WorldDatabase.Query("SELECT DISTINCT ConditionValue1, ConditionValue2 FROM conditions WHERE "
                    "SourceTypeOrReferenceId = 22 AND SourceEntry = {} AND SourceId = {} AND ConditionTypeOrReference = 29 AND "
                    "NegativeCondition = 1 AND ConditionValue3 = 0", entry, sourceId))
                do
                    held.emplace_back((*rows)[0].Get<uint32>(), float((*rows)[1].Get<uint32>()));
                while (rows->NextRow());
            return held;
        };
        for (DungeonBoss& boss : plan.bosses)
        {
            if (boss.IsEvent() || boss.summoned)
                continue;
            auto addClear = [&boss](float x, float y, float z, std::vector<std::pair<uint32, float>> const& held)
            {
                DungeonBoss::Clear clear;
                clear.x = x;
                clear.y = y;
                clear.z = z;
                for (auto const& [entry, yards] : held)
                {
                    clear.entries.push_back(entry);
                    clear.yards = std::max(clear.yards, yards);
                }
                boss.clearFirst.push_back(std::move(clear));
            };
            if (std::vector<std::pair<uint32, float>> const held = heldBy(boss.entry, 0); !held.empty())
                addClear(boss.x, boss.y, boss.z, held);
            std::vector<uint32> toUse;
            for (uint32 object : boss.useObjects)
            {
                std::vector<std::pair<uint32, float>> const held = heldBy(object, 1);
                if (held.empty())
                {
                    toUse.push_back(object);
                    continue;
                }
                for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
                    if (data.mapid == plan.mapId && data.id == object)
                        addClear(data.posX, data.posY, data.posZ, held);
            }
            boss.useObjects = std::move(toUse);
            // What an instance script holds a boss back with in C++ (no condition to read): Upper Blackrock Spire's script
            // opens the two doors to Pyroguard Emberseer only when the Blackhand Dreadweavers, Summoners and Veterans within
            // 15 yd of the Dragonspire Hall's seven runes are dead (instance_blackrock_spire, Dragonspireroomcheck), as a
            // player group clears the hall before it comes to the Blackrock Altar.
            if (boss.entry == 9816)
            {
                std::vector<std::pair<uint32, float>> const guards = { { 9817, 15.0f }, { 9818, 15.0f }, { 9819, 15.0f } };
                for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
                    if (data.mapid == plan.mapId && data.id >= 175194 && data.id <= 175200)
                        addClear(data.posX, data.posY, data.posZ, guards);
            }
        }

        if (!wingWhy.empty())
            plan.why = wingWhy;
        else if (!entrance)
            plan.why = "no entrance from the open world is known";
        else if (plan.bosses.empty())
            plan.why = "none of its bosses is spawned";
        else
            plan.supported = true;
        if (plan.supported)
            ++supported;
        _plans.push_back(std::move(plan));
    }
    uint32 raids = 0, raidsSupported = 0;
    for (DungeonPlan const& plan : _plans)
        if (plan.raid)
        {
            ++raids;
            raidsSupported += plan.supported ? 1 : 0;
        }
    LOG_INFO("module", "mod-guild-adventure: dungeons: {} of {} dungeons and {} of {} raids can be run on order (entrance and bosses known)",
        supported - raidsSupported, uint32(_plans.size()) - raids, raidsSupported, raids);
}

DungeonPlan const* Dungeons::GetPlan(uint32 id) const
{
    for (DungeonPlan const& plan : _plans)
        if (plan.id == id)
            return &plan;
    return nullptr;
}

std::vector<std::string> Dungeons::Describe(uint32 id) const
{
    std::vector<std::string> lines;
    if (!id)
    {
        for (DungeonPlan const& plan : _plans)
            lines.push_back(Acore::StringFormat("{} {}{} (map {}): {}", plan.id, plan.name, plan.raid ? " (raid)" : "", plan.mapId, plan.supported ?
                Acore::StringFormat("{} bosses", plan.bosses.size()) : "not yet: " + plan.why));
        return lines;
    }
    DungeonPlan const* plan = GetPlan(id);
    if (!plan)
    {
        lines.push_back(Acore::StringFormat("No dungeon {} in the list (.ga admin dungeon lists them).", id));
        return lines;
    }
    lines.push_back(Acore::StringFormat("{} {} (map {}): {}", plan->id, plan->name, plan->mapId, plan->supported ? "can be run" :
        "not yet: " + plan->why));
    lines.push_back(Acore::StringFormat("  entrance at ({:.0f}, {:.0f}, {:.0f}) map {}, leads to ({:.0f}, {:.0f}, {:.0f})", plan->outX,
        plan->outY, plan->outZ, plan->outMap, plan->inX, plan->inY, plan->inZ));
    for (DungeonBoss const& boss : plan->bosses)
        lines.push_back(Acore::StringFormat("  {} {} ({}) at ({:.0f}, {:.0f}, {:.0f}){}", boss.talkEntry ? "event of" : boss.summonObject ?
            "object for" : boss.summoned ? "summoned boss" : "boss", boss.name, boss.entry, boss.x, boss.y, boss.z, boss.last ? ", the last" : ""));
    return lines;
}
