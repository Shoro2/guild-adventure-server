/*
 * Farming: where an item can be farmed (Farming.h, docs/orders.md "Farm orders").
 */

#include "Farming.h"
#include "BotSettings.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureUtil.h"
#include "GuildBotHolder.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "OrderService.h"
#include "Player.h"
#include "RiskModel.h"
#include "StringFormat.h"
#include "Timer.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>

using namespace GuildAdventure;

namespace
{
    // The open-world maps bots grind on.
    constexpr uint32 OpenWorldMaps[] = { 0, 1, 530, 571 };
    // Reference tables roll on reference tables; deeper than this nothing in the world database goes.
    constexpr uint8 MaxReferenceDepth = 4;
    // A ground farther than this on another land mass is not walked to (as for the profession mode's places).
    constexpr float SameLandMassDistance = 2500.0f;
    // What a bot kills at one ground before the first creatures are back: more spawns than this add nothing.
    constexpr float UsefulSpawns = 12.0f;
    // A ground this far away is worth half of one next to the bot.
    constexpr float HalfValueDistance = 3000.0f;
    // The risk model judges at most this many grounds per look (best first).
    constexpr std::size_t MaxJudged = 12;
    // Faction templates of a player of each side (human, orc), for questions without a character at hand.
    constexpr uint32 AllianceFactionTemplate = 1;
    constexpr uint32 HordeFactionTemplate = 2;

    bool IsAttackableBy(uint32 factionTemplateId, TeamId team)
    {
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(factionTemplateId);
        FactionTemplateEntry const* own = sFactionTemplateStore.LookupEntry(team == TEAM_HORDE ? HordeFactionTemplate : AllianceFactionTemplate);
        return faction && own && !faction->IsFriendlyTo(*own);
    }

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    }

    char const* MapName(uint32 mapId)
    {
        switch (mapId)
        {
            case 0:   return "the Eastern Kingdoms";
            case 1:   return "Kalimdor";
            case 530: return "Outland";
            case 571: return "Northrend";
        }
        return "another map";
    }

    int64 CellKey(float x, float y, float cell)
    {
        return (int64(std::floor(x / cell)) << 32) ^ (int64(std::floor(y / cell)) & 0xFFFFFFFFll);
    }
}

namespace
{
    // A spawn a bot may farm an item from, and the ground around it.
    struct Target
    {
        MobSpawn const* mob;
        float yield;
        float distance;        // from the bot (0 on another map)
        uint32 spawns = 0;     // targets within the radius, itself included
        float groundYield = 0.0f;
        float rank = 0.0f;
    };

    // The ground around each target: every target within the radius (a grid of that cell size, nine cells each), and
    // its rank: what a round brings (more spawns than the bot kills before the first are back add nothing), less for
    // the way. Returns the size of the biggest ground.
    uint32 RankGrounds(std::vector<Target>& targets, float radius)
    {
        std::unordered_map<int64, std::vector<uint32>> grid;
        for (uint32 i = 0; i < targets.size(); ++i)
            grid[CellKey(targets[i].mob->spawn.x, targets[i].mob->spawn.y, radius)].push_back(i);
        uint32 biggest = 0;
        for (Target& target : targets)
        {
            SpawnPoint const& spot = target.mob->spawn;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                {
                    auto cell = grid.find(CellKey(spot.x + float(dx) * radius, spot.y + float(dy) * radius, radius));
                    if (cell == grid.end())
                        continue;
                    for (uint32 index : cell->second)
                    {
                        SpawnPoint const& other = targets[index].mob->spawn;
                        if (std::hypot(other.x - spot.x, other.y - spot.y) <= radius && std::fabs(other.z - spot.z) <= radius)
                        {
                            ++target.spawns;
                            target.groundYield += targets[index].yield;
                        }
                    }
                }
            biggest = std::max(biggest, target.spawns);
            float const useful = target.groundYield * std::min(1.0f, UsefulSpawns / float(target.spawns));
            target.rank = useful / (1.0f + target.distance / HalfValueDistance);
        }
        return biggest;
    }
}

Farming& Farming::Instance()
{
    static Farming instance;
    return instance;
}

void Farming::Load()
{
    uint32 const started = getMSTime();
    _creatureLoot.clear();
    _referenceLoot.clear();
    _lootsOfItem.clear();
    _refsOfItem.clear();
    _lootsOfRef.clear();
    _refsOfRef.clear();
    _creaturesOfLoot.clear();
    _grindKinds.clear();
    _yields.clear();
    _droppedItems.clear();

    // The loot tables as the core rolls them in the open world (loot mode 1). A stray negative reference is skipped.
    auto load = [&](char const* table, std::unordered_map<uint32, LootTable>& store, bool creature)
    {
        uint32 rows = 0;
        QueryResult result = WorldDatabase.Query("SELECT Entry, Item, Reference, Chance, QuestRequired, GroupId, MinCount, MaxCount "
            "FROM {} WHERE (LootMode & 1) <> 0", table);
        if (!result)
            return rows;
        do
        {
            Field* fields = result->Fetch();
            uint32 const id = fields[0].Get<uint32>();
            int32 const reference = fields[2].Get<int32>();
            if (reference < 0)
                continue;
            LootRow row;
            row.item = fields[1].Get<uint32>();
            row.reference = uint32(reference);
            row.chance = std::fabs(fields[3].Get<float>());
            row.quest = fields[4].Get<bool>();
            row.group = fields[5].Get<uint8>();
            row.minCount = fields[6].Get<uint8>();
            row.maxCount = fields[7].Get<uint8>();
            store[id].push_back(row);
            ++rows;
            if (row.reference)
                (creature ? _lootsOfRef : _refsOfRef)[row.reference].push_back(id);
            else if (!row.quest)
            {
                (creature ? _lootsOfItem : _refsOfItem)[row.item].push_back(id);
                _droppedItems.insert(row.item);
            }
        } while (result->NextRow());
        return rows;
    };
    uint32 const creatureRows = load("creature_loot_template", _creatureLoot, true);
    uint32 const referenceRows = load("reference_loot_template", _referenceLoot, false);

    for (auto const& [entry, cinfo] : *sObjectMgr->GetCreatureTemplates())
        if (cinfo.lootid)
            _creaturesOfLoot[cinfo.lootid].push_back(entry);

    // What the planner grinds: the mob spawns of the world index (no elites, no NPCs, with loot), by kind.
    for (uint32 mapId : OpenWorldMaps)
        for (MobSpawn const& mob : sWorldIndex.GetMobSpawns(mapId))
        {
            auto [itr, added] = _grindKinds.try_emplace(mob.spawn.entry);
            if (!added)
                continue;
            itr->second.minLevel = mob.minLevel;
            itr->second.maxLevel = mob.maxLevel;
            itr->second.factionTemplate = mob.factionTemplate;
            if (CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(mob.spawn.entry))
                itr->second.name = cinfo->Name;
        }

    LOG_INFO("module", "mod-guild-adventure: farming: {} creature loot rows, {} reference loot rows, {} items in them, {} kinds of "
        "creature bots grind ({} ms)", creatureRows, referenceRows, _droppedItems.size(), _grindKinds.size(), GetMSTimeDiffToNow(started));
}

float Farming::GroupShare(LootTable const& table, LootRow const& row)
{
    // A group gives one of its rows: the ones with a chance of their own first, the rest share what is left equally.
    float explicitSum = 0.0f;
    uint32 equal = 0;
    for (LootRow const& other : table)
    {
        if (other.group != row.group)
            continue;
        if (other.chance > 0.0f)
            explicitSum += other.chance;
        else
            ++equal;
    }
    if (row.chance > 0.0f)
        return std::min(row.chance, 100.0f) / 100.0f;
    return equal ? std::max(0.0f, 100.0f - explicitSum) / 100.0f / float(equal) : 0.0f;
}

float Farming::TableYield(std::unordered_map<uint32, LootTable> const& store, uint32 id, uint32 item, uint8 depth) const
{
    auto itr = store.find(id);
    if (itr == store.end() || depth > MaxReferenceDepth)
        return 0.0f;
    float total = 0.0f;
    for (LootRow const& row : itr->second)
    {
        if (row.quest || (!row.reference && row.item != item))
            continue;
        float const share = row.group ? GroupShare(itr->second, row) : std::min(row.chance, 100.0f) / 100.0f;
        if (share <= 0.0f)
            continue;
        if (row.reference)
        {
            float const inner = TableYield(_referenceLoot, row.reference, item, uint8(depth + 1));
            if (inner > 0.0f)
                total += share * float(std::max<uint8>(row.maxCount, 1)) * inner;
        }
        else
            total += share * (float(row.minCount) + float(row.maxCount)) / 2.0f;
    }
    return total;
}

std::unordered_map<uint32, float> const& Farming::Yields(uint32 item)
{
    auto cached = _yields.find(item);
    if (cached != _yields.end())
        return cached->second;
    std::unordered_map<uint32, float>& yields = _yields[item];

    // The reference tables that hold the item, directly or through other reference tables.
    std::unordered_set<uint32> references;
    std::vector<uint32> open;
    if (auto direct = _refsOfItem.find(item); direct != _refsOfItem.end())
        for (uint32 reference : direct->second)
            if (references.insert(reference).second)
                open.push_back(reference);
    for (uint8 depth = 0; depth < MaxReferenceDepth && !open.empty(); ++depth)
    {
        std::vector<uint32> next;
        for (uint32 reference : open)
            if (auto parents = _refsOfRef.find(reference); parents != _refsOfRef.end())
                for (uint32 parent : parents->second)
                    if (references.insert(parent).second)
                        next.push_back(parent);
        open.swap(next);
    }

    // The creature loot tables that give it: with a row of their own, or by a roll on one of those reference tables.
    std::unordered_set<uint32> loots;
    if (auto direct = _lootsOfItem.find(item); direct != _lootsOfItem.end())
        loots.insert(direct->second.begin(), direct->second.end());
    for (uint32 reference : references)
        if (auto rolling = _lootsOfRef.find(reference); rolling != _lootsOfRef.end())
            loots.insert(rolling->second.begin(), rolling->second.end());

    for (uint32 loot : loots)
    {
        auto creatures = _creaturesOfLoot.find(loot);
        if (creatures == _creaturesOfLoot.end())
            continue;
        float const yield = TableYield(_creatureLoot, loot, item, 0);
        if (yield <= 0.0f)
            continue;
        for (uint32 entry : creatures->second)
            yields[entry] = yield;
    }
    return yields;
}

float Farming::YieldOf(uint32 creatureEntry, uint32 item)
{
    std::unordered_map<uint32, float> const& yields = Yields(item);
    auto itr = yields.find(creatureEntry);
    return itr != yields.end() ? itr->second : 0.0f;
}

bool Farming::CheckItem(uint32 item, TeamId team, std::string& error)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    if (!proto)
    {
        error = "That item is unknown";
        return false;
    }
    std::unordered_map<uint32, float> const& yields = Yields(item);
    if (yields.empty())
    {
        error = Acore::StringFormat("No creature drops {} (a farm order hunts creatures; it does not gather, fish, skin, buy or craft)",
            proto->Name1);
        return false;
    }
    for (auto const& [entry, yield] : yields)
        if (auto kind = _grindKinds.find(entry); kind != _grindKinds.end() && IsAttackableBy(kind->second.factionTemplate, team))
            return true;
    error = Acore::StringFormat("Only elites, dungeon creatures or creatures your side cannot attack drop {}", proto->Name1);
    return false;
}

FarmSearch Farming::FindGrounds(Player const* player, BotRuntime const& rt, uint32 item, std::size_t limit)
{
    FarmSearch result;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    if (!proto)
    {
        result.problem = FarmProblem::UnknownItem;
        result.reason = Acore::StringFormat("item {} is unknown", item);
        return result;
    }
    std::unordered_map<uint32, float> const& yields = Yields(item);
    result.kinds = uint32(yields.size());
    if (yields.empty())
    {
        result.problem = FarmProblem::NoCreatures;
        result.reason = Acore::StringFormat("no creature drops {} (a farm order hunts creatures; it does not gather, fish, skin, buy or craft)",
            proto->Name1);
        return result;
    }

    uint8 const level = player->GetLevel();
    int32 const margin = sBotSettings.Get(player, Setting::FarmLevelsBelow);
    float const radius = std::max(10.0f, sBotSettings.GetFloat(player, Setting::FarmGroundRadius));
    uint32 const minSpawns = uint32(std::max(1, sBotSettings.Get(player, Setting::FarmMinSpawns)));
    float const maxDistance = sBotSettings.GetFloat(player, Setting::FarmMaxDistance);
    int32 const levelLimit = int32(level) - margin;
    result.levelLimit = uint8(std::clamp(levelLimit, 0, 255));

    // The kinds of creature this bot may farm it from: what the planner grinds, what its side may attack, far enough
    // below it.
    std::unordered_map<uint32, float> valid;
    uint8 lowest = 255;
    for (auto const& [entry, yield] : yields)
    {
        auto kind = _grindKinds.find(entry);
        if (kind == _grindKinds.end() || !WorldIndex::IsAttackableFaction(kind->second.factionTemplate, player))
            continue;
        ++result.grindKinds;
        if (kind->second.maxLevel < lowest)
        {
            lowest = kind->second.maxLevel;
            result.lowestName = kind->second.name;
        }
        if (int32(kind->second.maxLevel) > levelLimit)
            continue;
        ++result.validKinds;
        valid.emplace(entry, yield);
    }
    result.lowestLevel = lowest == 255 ? 0 : lowest;
    if (!result.grindKinds)
    {
        result.problem = FarmProblem::NotGrindable;
        result.reason = Acore::StringFormat("only elites, dungeon creatures or creatures its side cannot attack drop {}", proto->Name1);
        return result;
    }
    if (valid.empty())
    {
        result.problem = FarmProblem::TooHigh;
        result.reason = Acore::StringFormat("every creature that drops {} is above level {} ({} below its own level {}); the lowest is {} "
            "(level {}), so from level {} on", proto->Name1, std::max(levelLimit, 0), margin, uint32(level), result.lowestName,
            uint32(result.lowestLevel), uint32(result.lowestLevel) + uint32(margin));
        return result;
    }

    // Their spawns on the bot's map, without the places it keeps away from and the grounds it has emptied.
    uint32 const now = getMSTime();
    uint32 const mapId = player->GetMapId();
    Region const here = GetRegion(player);
    std::vector<Target> targets;
    uint32 setAside = 0, emptied = 0, tooFar = 0;
    uint32 firstBack = 0;
    for (MobSpawn const& mob : sWorldIndex.GetMobSpawns(mapId))
    {
        auto yield = valid.find(mob.spawn.entry);
        if (yield == valid.end())
            continue;
        ++result.validSpawns;
        float const distance = player->GetExactDist2d(mob.spawn.x, mob.spawn.y);
        // On this land mass (an island off the coast is another one: the bot would have to take a ship).
        if (distance > SameLandMassDistance && GetRegionAt(mob.spawn.mapId, mob.spawn.x, mob.spawn.y, mob.spawn.z) != here)
            continue;
        ++result.localSpawns;
        if (maxDistance > 0.0f && distance > maxDistance)
        {
            ++tooFar;
            continue;
        }
        if (rt.IsAvoided(mob.spawn) || rt.IsInAvoidedArea(mob.spawn.mapId, mob.spawn.x, mob.spawn.y, level))
        {
            ++setAside;
            continue;
        }
        bool wait = false;
        for (BotRuntime::Farm::Emptied const& ground : rt.farm.emptied)
            if (ground.mapId == mapId && now < ground.until &&
                std::hypot(ground.x - mob.spawn.x, ground.y - mob.spawn.y) <= radius)
            {
                wait = true;
                uint32 const left = ground.until - now;
                firstBack = firstBack ? std::min(firstBack, left) : left;
                break;
            }
        if (wait)
        {
            ++emptied;
            continue;
        }
        targets.push_back({ &mob, yield->second, distance });
    }

    // Nothing on this land mass: on another one? The bot goes there when it must (the owner, 2026-11-13: [quote left out]), by ship, zeppelin or portal as any travel goes; the ground is picked anew when
    // it has arrived.
    if (!result.localSpawns)
    {
        float bestValue = 0.0f;
        for (uint32 otherMap : OpenWorldMaps)
        {
            std::vector<Target> away;
            for (MobSpawn const& mob : sWorldIndex.GetMobSpawns(otherMap))
                if (auto yield = valid.find(mob.spawn.entry); yield != valid.end())
                    away.push_back({ &mob, yield->second, 0.0f });
            if (away.empty())
                continue;
            uint32 const needed = std::min(minSpawns, std::max<uint32>(RankGrounds(away, radius), 1));
            // The best ground of each land mass of that map.
            std::unordered_map<uint8, Target const*> bestOf;
            for (Target const& target : away)
            {
                if (target.spawns < needed)
                    continue;
                SpawnPoint const& spot = target.mob->spawn;
                Region const region = GetRegionAt(spot.mapId, spot.x, spot.y, spot.z);
                if (region == Region::None || region == here)
                    continue;
                Target const*& best = bestOf[uint8(region)];
                if (!best || target.rank > best->rank)
                    best = &target;
            }
            for (auto const& [region, target] : bestOf)
            {
                if (result.elsewhere.empty())
                    result.elsewhere = ToString(Region(region));
                int32 const legs = sWorldIndex.CountTravelLegs(here, Region(region), player->GetTeamId());
                if (legs <= 0)
                    continue;
                float const value = target->rank / float(legs);
                if (value <= bestValue)
                    continue;
                bestValue = value;
                result.voyage = true;
                result.voyageSpot = target->mob->spawn;
                result.voyageRegion = Region(region);
                result.voyageSpawns = target->spawns;
                result.voyageLegs = uint32(legs);
                result.elsewhere = ToString(Region(region));
                if (auto kind = _grindKinds.find(target->mob->spawn.entry); kind != _grindKinds.end())
                    result.voyageName = kind->second.name;
            }
        }
        if (result.elsewhere.empty())
        {
            result.problem = FarmProblem::NotGrindable;
            result.reason = Acore::StringFormat("the creatures that drop {} have no place in the open world", proto->Name1);
            return result;
        }
        result.problem = FarmProblem::Elsewhere;
        // (A reach set by the leader keeps the bot on its side of the sea.)
        if (maxDistance > 0.0f)
            result.voyage = false;
        result.reason = result.voyage ?
            Acore::StringFormat("the creatures it can farm {} from live in {}: it travels there", proto->Name1, result.elsewhere) :
            maxDistance > 0.0f ?
            Acore::StringFormat("the creatures it could farm {} from live in {}, farther than its farm orders reach ({:.0f} yd)",
                proto->Name1, result.elsewhere, maxDistance) :
            Acore::StringFormat("the creatures it could farm {} from live in {}, and no ship, zeppelin or portal for its side leads "
                "there", proto->Name1, result.elsewhere);
        return result;
    }

    // The ground around each target: every target within the radius.
    uint32 const biggest = RankGrounds(targets, radius);
    // Groups first; single creatures only when no place has a group (an item few creatures drop).
    uint32 const needed = std::min(minSpawns, std::max<uint32>(biggest, 1));
    std::vector<uint32> order;
    for (uint32 i = 0; i < targets.size(); ++i)
        if (targets[i].spawns >= needed)
            order.push_back(i);
    std::sort(order.begin(), order.end(), [&](uint32 a, uint32 b) { return targets[a].rank > targets[b].rank; });

    std::vector<SpawnPoint const*> looked;
    uint32 refused = 0;
    bool found = false;
    for (uint32 index : order)
    {
        // Up to MaxJudged places are judged; with a usable one in hand the search ends once `limit` are listed.
        if (looked.size() >= MaxJudged || (found && result.grounds.size() >= limit))
            break;
        Target const& target = targets[index];
        SpawnPoint const& spot = target.mob->spawn;
        // One ground per place.
        bool again = false;
        for (SpawnPoint const* other : looked)
            if (std::hypot(other->x - spot.x, other->y - spot.y) <= radius)
            {
                again = true;
                break;
            }
        if (again)
            continue;
        looked.push_back(&spot);
        // Not in a zone that begins above the bot, and where the risk model lets it work.
        ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(sWorldIndex.GetSpawnZone(spot));
        Risk::SpotVerdict const verdict = Risk::JudgeSpot(player, rt, spot);
        bool const usable = verdict.usable && !(zone && zone->minLevel > level);

        FarmGround ground;
        ground.spot = spot;
        ground.spawns = target.spawns;
        ground.yield = target.groundYield;
        ground.distance = target.distance;
        ground.usable = usable;
        ground.verdict = usable ? "" : !verdict.usable ? verdict.reason : "its zone begins above the bot's level";
        // What stands there: kinds, levels, the kind most of them are.
        std::map<uint32, uint32> kinds;
        ground.minLevel = 255;
        for (Target const& other : targets)
            if (std::hypot(other.mob->spawn.x - spot.x, other.mob->spawn.y - spot.y) <= radius &&
                std::fabs(other.mob->spawn.z - spot.z) <= radius)
            {
                ++kinds[other.mob->spawn.entry];
                ground.minLevel = std::min(ground.minLevel, other.mob->minLevel);
                ground.maxLevel = std::max(ground.maxLevel, other.mob->maxLevel);
            }
        ground.kinds = uint32(kinds.size());
        uint32 most = 0;
        for (auto const& [entry, count] : kinds)
            if (count > most)
            {
                most = count;
                if (auto kind = _grindKinds.find(entry); kind != _grindKinds.end())
                    ground.name = kind->second.name;
            }
        if (!usable)
            ++refused;
        // The first usable ground goes to the front: it is the one the planner takes.
        if (usable && !found)
            result.grounds.insert(result.grounds.begin(), ground);
        else
            result.grounds.push_back(ground);
        found = found || usable;
    }
    if (result.grounds.size() > limit)
        result.grounds.resize(limit);

    if (!result.grounds.empty() && result.grounds.front().usable)
    {
        result.problem = FarmProblem::None;
        return result;
    }
    if (emptied && !refused && !setAside)
    {
        result.problem = FarmProblem::Waiting;
        result.waitMs = firstBack;
        result.reason = Acore::StringFormat("it has emptied the hunting grounds for {} in its reach and waits for the creatures to come "
            "back", proto->Name1);
        return result;
    }
    result.problem = FarmProblem::Refused;
    if (tooFar && !refused && !setAside)
        result.reason = Acore::StringFormat("the creatures it could farm {} from are farther away than its farm orders reach ({:.0f} yd)",
            proto->Name1, maxDistance);
    else
        result.reason = Acore::StringFormat("the risk rules keep it from every place where it could farm {} right now ({} looked at, {} "
            "spawns at places it has set aside)", proto->Name1, refused, setAside);
    return result;
}

std::vector<FarmItem> Farming::Search(std::string const& text, TeamId team, std::size_t limit)
{
    std::vector<FarmItem> found;
    std::string const needle = Lower(text);
    if (needle.empty())
        return found;
    bool const number = std::all_of(needle.begin(), needle.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    uint32 const wantedId = number ? uint32(std::strtoul(needle.c_str(), nullptr, 10)) : 0;

    auto consider = [&](ItemTemplate const& proto) -> bool
    {
        std::unordered_map<uint32, float> const& yields = Yields(proto.ItemId);
        FarmItem entry;
        entry.lowestLevel = 255;
        for (auto const& [creature, yield] : yields)
        {
            auto kind = _grindKinds.find(creature);
            if (kind == _grindKinds.end() || !IsAttackableBy(kind->second.factionTemplate, team))
                continue;
            ++entry.kinds;
            entry.lowestLevel = std::min(entry.lowestLevel, kind->second.maxLevel);
        }
        if (!entry.kinds)
            return false;
        entry.item = proto.ItemId;
        entry.name = proto.Name1;
        entry.quality = uint8(proto.Quality);
        entry.itemLevel = uint16(proto.ItemLevel);
        entry.itemClass = uint8(proto.Class);
        found.push_back(entry);
        return true;
    };

    if (number)
    {
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(wantedId))
            consider(*proto);
        return found;
    }

    // Names that begin with the text first, then the ones that hold it, each by name. (Only items some loot table
    // holds are looked at: a few thousand of the item templates.)
    struct Match
    {
        ItemTemplate const* proto;
        bool prefix;
        std::string name;
    };
    std::vector<Match> matches;
    for (uint32 item : _droppedItems)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
        if (!proto)
            continue;
        std::string name = Lower(proto->Name1);
        std::size_t const at = name.find(needle);
        if (at == std::string::npos)
            continue;
        matches.push_back({ proto, at == 0, std::move(name) });
    }
    std::sort(matches.begin(), matches.end(), [](Match const& a, Match const& b)
    {
        if (a.prefix != b.prefix)
            return a.prefix;
        if (a.name != b.name)
            return a.name < b.name;
        return a.proto->ItemId < b.proto->ItemId;
    });
    for (Match const& match : matches)
    {
        if (found.size() >= limit)
            break;
        consider(*match.proto);
    }
    return found;
}

std::vector<std::string> Farming::Describe(Player const* player, BotRuntime const& rt, uint32 item)
{
    std::vector<std::string> lines;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    if (!proto)
    {
        lines.push_back(Acore::StringFormat("item {} is unknown", item));
        return lines;
    }
    FarmSearch const search = FindGrounds(player, rt, item, 5);
    lines.push_back(Acore::StringFormat("{} ({}) for {} (level {}): {} kinds of creature drop it, {} of them a bot of its side can grind, "
        "{} at or below level {} with {} spawns on its map", proto->Name1, item, player->GetName(), uint32(player->GetLevel()),
        search.kinds, search.grindKinds, search.validKinds, uint32(search.levelLimit), search.validSpawns));

    // The best kinds by what a kill brings.
    std::vector<std::pair<float, uint32>> best;
    for (auto const& [entry, yield] : Yields(item))
        if (auto kind = _grindKinds.find(entry); kind != _grindKinds.end() &&
            WorldIndex::IsAttackableFaction(kind->second.factionTemplate, player) && kind->second.maxLevel <= search.levelLimit)
            best.emplace_back(yield, entry);
    std::sort(best.begin(), best.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
    for (std::size_t i = 0; i < best.size() && i < 5; ++i)
    {
        GrindKind const& kind = _grindKinds[best[i].second];
        lines.push_back(Acore::StringFormat("  kind: {} ({}, level {}-{}): {:.3f} per kill, {} spawns", kind.name, best[i].second,
            uint32(kind.minLevel), uint32(kind.maxLevel), best[i].first, sWorldIndex.GetCreatureSpawns(best[i].second).size()));
    }
    if (search.problem != FarmProblem::None)
        lines.push_back("no hunting ground: " + search.reason);
    for (FarmGround const& ground : search.grounds)
        lines.push_back(Acore::StringFormat("  ground: {} at ({:.0f}, {:.0f}, {:.0f}) in {}, {:.0f} yd away: {} spawns of {} kind{} (level "
            "{}-{}), {:.2f} per round: {}", ground.name, ground.spot.x, ground.spot.y, ground.spot.z,
            GetAreaName(sWorldIndex.GetSpawnZone(ground.spot)), ground.distance, ground.spawns, ground.kinds, ground.kinds == 1 ? "" : "s",
            uint32(ground.minLevel), uint32(ground.maxLevel), ground.yield, ground.usable ? "usable" : "not usable (" + ground.verdict + ")"));
    return lines;
}

bool Farming::IsOrderItem(ObjectGuid character, uint32 item)
{
    AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(character);
    GuildBot const* bot = guild ? guild->FindPlanned(character) : nullptr;
    return bot && bot->order.type == OrderType::Farm && bot->order.zoneId == item;
}
