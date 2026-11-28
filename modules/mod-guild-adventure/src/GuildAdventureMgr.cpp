/*
 * GuildAdventureMgr implementation. See GuildAdventureMgr.h.
 */

#include "GuildAdventureMgr.h"
#include "BotDriver.h"
#include "GuildBudget.h"
#include "OrderService.h"
#include "RiskModel.h"
#include "ClassAbilities.h"
#include "FightProfileStore.h"
#include "SpecService.h"
#include "Squads.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GuildAdventureSettings.h"
#include "GuildAdventureUtil.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Readiness.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "World.h"
#include <algorithm>

using namespace GuildAdventure;

namespace
{
    struct OrderTokenEntry
    {
        OrderType type;
        char const* token;
        bool implemented;
        bool needsZone;
    };

    // v1 server: AUTO, IDLE, FOLLOW and LEVEL_ZONE are accepted (docs/protocol.md section 8.1).
    constexpr OrderTokenEntry OrderTokens[] =
    {
        { OrderType::Auto,      "AUTO",       true,  false },
        { OrderType::Idle,      "IDLE",       true,  false },
        { OrderType::Follow,    "FOLLOW",     true,  false },
        { OrderType::LevelZone, "LEVEL_ZONE", true,  true  },
        { OrderType::Grind,     "GRIND",      false, true  },
        { OrderType::Quest,     "QUEST",      false, true  },
        { OrderType::Gather,    "GATHER",     false, true  },
        { OrderType::Craft,     "CRAFT",      true,  false },   // the profession mode (docs/orders.md "Profession mode")
        { OrderType::Dungeon,   "DUNGEON",    true,  false },   // a dungeon run (docs/orders.md "Dungeon runs"): its
                                                               // "zone" is the dungeon's id, checked by the planner
        { OrderType::Train,     "TRAIN",      false, false },
        { OrderType::Vendor,    "VENDOR",     false, false },
        { OrderType::Farm,      "FARM",       true,  false },   // a farm order (docs/orders.md "Farm orders"): its "zone"
                                                               // is the item, checked by Farming::CheckItem
    };

    OrderTokenEntry const* FindOrder(OrderType type)
    {
        for (OrderTokenEntry const& entry : OrderTokens)
            if (entry.type == type)
                return &entry;
        return nullptr;
    }

    // FNV-1a, 31 bits so it fits the protocol's int and the addon's number type; never 0 (0 = "no cache").
    uint32 HashZones(std::vector<ZoneTemplate> const& zones)
    {
        uint32 hash = 2166136261u;
        auto mix = [&hash](std::string const& text)
        {
            for (unsigned char c : text)
            {
                hash ^= c;
                hash *= 16777619u;
            }
            hash ^= 0xFF;
            hash *= 16777619u;
        };

        for (ZoneTemplate const& zone : zones)
        {
            mix(std::to_string(zone.zoneId));
            mix(zone.name);
            mix(std::to_string(zone.minLevel) + "-" + std::to_string(zone.maxLevel));
            mix(std::string(1, zone.faction) + std::to_string(zone.continent));
            for (OrderType type : zone.orders)
                mix(ToToken(type));
        }

        hash &= 0x7FFFFFFFu;
        return hash ? hash : 1;
    }
}

char const* GuildAdventure::ToString(BotState state)
{
    switch (state)
    {
        case BotState::Active:  return "active";
        case BotState::Benched: return "benched";
    }
    return "active";
}

char const* GuildAdventure::ToToken(OrderType type)
{
    OrderTokenEntry const* entry = FindOrder(type);
    return entry ? entry->token : "AUTO";
}

std::optional<OrderType> GuildAdventure::OrderTypeFromToken(std::string const& token)
{
    for (OrderTokenEntry const& entry : OrderTokens)
        if (token == entry.token)
            return entry.type;
    return std::nullopt;
}

bool GuildAdventure::IsOrderImplemented(OrderType type)
{
    OrderTokenEntry const* entry = FindOrder(type);
    return entry && entry->implemented;
}

bool GuildAdventure::OrderNeedsZone(OrderType type)
{
    OrderTokenEntry const* entry = FindOrder(type);
    return entry && entry->needsZone;
}

char const* GuildAdventure::ToToken(UnlockKind kind)
{
    switch (kind)
    {
        case UnlockKind::Slots:   return "slots";
        case UnlockKind::Offline: return "offline";
        case UnlockKind::Feature: return "feature";
    }
    return "feature";
}

GuildBot* AdventureGuild::FindBot(ObjectGuid guid)
{
    auto itr = bots.find(guid);
    return itr != bots.end() ? &itr->second : nullptr;
}

GuildBot const* AdventureGuild::FindBot(ObjectGuid guid) const
{
    auto itr = bots.find(guid);
    return itr != bots.end() ? &itr->second : nullptr;
}

GuildBot* AdventureGuild::FindPlanned(ObjectGuid guid)
{
    if (guid == leader)
        return leaderPlanned ? &leaderPlan : nullptr;
    return FindBot(guid);
}

GuildBot const* AdventureGuild::FindPlanned(ObjectGuid guid) const
{
    if (guid == leader)
        return leaderPlanned ? &leaderPlan : nullptr;
    return FindBot(guid);
}

GuildBot* AdventureGuild::FindMember(ObjectGuid guid)
{
    if (!leader.IsEmpty() && guid == leader)
        return &leaderPlan;
    return FindBot(guid);
}

GuildBot const* AdventureGuild::FindMember(ObjectGuid guid) const
{
    if (!leader.IsEmpty() && guid == leader)
        return &leaderPlan;
    return FindBot(guid);
}

bool ZoneTemplate::AllowsTeam(TeamId team) const
{
    if (faction == 'C')
        return true;
    return (faction == 'A' && team == TEAM_ALLIANCE) || (faction == 'H' && team == TEAM_HORDE);
}

bool ZoneTemplate::AllowsOrder(OrderType type) const
{
    return std::find(orders.begin(), orders.end(), type) != orders.end();
}

GuildAdventureMgr& GuildAdventureMgr::Instance()
{
    static GuildAdventureMgr instance;
    return instance;
}

// ---------------------------------------------------------------------------------------------------------------
// Templates (world database)
// ---------------------------------------------------------------------------------------------------------------

void GuildAdventureMgr::LoadTemplates()
{
    _zones.clear();
    _zoneIndex.clear();
    // Zones that reach beyond the server's level cap are left out (GuildAdventure.Zones.WithinLevelCap): with
    // MaxPlayerLevel = 60 the guild plays classic content only, and no bot is sent through the Dark Portal.
    uint8 const levelCap = uint8(std::min<uint32>(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL), 255));
    bool const withinCap = GuildAdventure::GetSettings().zonesWithinLevelCap;
    uint32 beyondCap = 0;
    if (QueryResult result = WorldDatabase.Query("SELECT zone_id, name, min_level, max_level, faction, continent, orders "
        "FROM ga_zone_template ORDER BY min_level, max_level, zone_id"))
    {
        do
        {
            Field* fields = result->Fetch();
            ZoneTemplate zone;
            zone.zoneId = fields[0].Get<uint32>();
            zone.name = fields[1].Get<std::string>();
            zone.minLevel = fields[2].Get<uint8>();
            zone.maxLevel = fields[3].Get<uint8>();
            if (withinCap && zone.maxLevel > levelCap)
            {
                ++beyondCap;
                continue;
            }
            std::string const faction = fields[4].Get<std::string>();
            zone.faction = (faction == "A" || faction == "H") ? faction[0] : 'C';
            zone.continent = fields[5].Get<uint16>();
            std::string const orders = fields[6].Get<std::string>();
            for (std::string_view token : Acore::Tokenize(orders, ',', false))
            {
                if (std::optional<OrderType> type = OrderTypeFromToken(std::string(token)))
                    zone.orders.push_back(*type);
                else
                    LOG_ERROR("module", "mod-guild-adventure: ga_zone_template {} lists unknown order '{}'", zone.zoneId, token);
            }
            _zoneIndex[zone.zoneId] = _zones.size();
            _zones.push_back(std::move(zone));
        } while (result->NextRow());
    }
    _zoneVersion = HashZones(_zones);
    if (beyondCap)
        LOG_INFO("module", "mod-guild-adventure: {} zones reach beyond the level cap {} and are not offered "
            "(GuildAdventure.Zones.WithinLevelCap)", beyondCap, uint32(levelCap));

    _unlocks.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT id, renown_level, kind, value, gold_cost, description "
        "FROM ga_unlock_template ORDER BY renown_level, id"))
    {
        do
        {
            Field* fields = result->Fetch();
            UnlockTemplate unlock;
            unlock.id = fields[0].Get<uint32>();
            unlock.renownLevel = std::max<uint8>(1, fields[1].Get<uint8>());
            std::string const kind = fields[2].Get<std::string>();
            unlock.value = fields[3].Get<std::string>();
            unlock.goldCost = fields[4].Get<uint64>();
            unlock.description = fields[5].Get<std::string>();
            if (kind == "slots")
                unlock.kind = UnlockKind::Slots;
            else if (kind == "offline")
                unlock.kind = UnlockKind::Offline;
            else if (kind == "feature")
                unlock.kind = UnlockKind::Feature;
            else
            {
                LOG_ERROR("module", "mod-guild-adventure: ga_unlock_template {} has unknown kind '{}', skipped", unlock.id, kind);
                continue;
            }
            _unlocks.push_back(std::move(unlock));
        } while (result->NextRow());
    }

    _renownToNext.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT level, points FROM ga_renown_level ORDER BY level"))
    {
        do
        {
            Field* fields = result->Fetch();
            _renownToNext[fields[0].Get<uint8>()] = fields[1].Get<uint32>();
        } while (result->NextRow());
    }

    _renownSources.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT event_type, points FROM ga_renown_source"))
    {
        do
        {
            Field* fields = result->Fetch();
            _renownSources[fields[0].Get<std::string>()] = int32(fields[1].Get<uint32>());
        } while (result->NextRow());
    }

    LOG_INFO("module", "mod-guild-adventure: loaded {} zones (version {}), {} unlocks, {} renown levels, {} renown sources",
        _zones.size(), _zoneVersion, _unlocks.size(), _renownToNext.size() + 1, _renownSources.size());

    // What a class only gets from its class quests (ga_class_ability).
    ClassAbilities::Load();
    // Talent templates per class, the guilds' own and what each bot plays by (ga_spec_template, ga_bot_spec).
    sSpecService.Load();
    // The stat weights items are judged by, from the wowbot335 research (ga_stat_weight; mod-playerbots patch 0002).
    BotDriver::LoadStatWeights();
    // The guilds' fight profiles, the bots' assignments and their own option values (ga_fight_*).
    sFightProfileStore.Load();
    // Where guild characters died, so those places stay set aside across a restart (ga_bot_death_place).
    Risk::Load();
    // What each character has spent of its gear allowance (ga_bot_budget).
    GuildBudget::Load();
    // Where and on which quest guild characters died in the last hours (ga_guild_death).
    OrderService::LoadGuildDeaths();
    // Crossings of gaps in the path finder's ground (ga_path_bridge).
    OrderService::LoadPathBridges();
    // What each character waits with since before the restart (ga_bot_wait).
    OrderService::LoadBotWaits();
}

ZoneTemplate const* GuildAdventureMgr::GetZone(uint32 zoneId) const
{
    auto itr = _zoneIndex.find(zoneId);
    return itr != _zoneIndex.end() ? &_zones[itr->second] : nullptr;
}

UnlockTemplate const* GuildAdventureMgr::GetUnlock(uint32 id) const
{
    for (UnlockTemplate const& unlock : _unlocks)
        if (unlock.id == id)
            return &unlock;
    return nullptr;
}

uint32 GuildAdventureMgr::GetRenownToNext(uint8 level) const
{
    auto itr = _renownToNext.find(level);
    return itr != _renownToNext.end() ? itr->second : 0;
}

uint8 GuildAdventureMgr::GetMaxRenownLevel() const
{
    // Levels are consecutive from 1; the highest level is the one after the last row.
    uint8 level = 1;
    while (_renownToNext.find(level) != _renownToNext.end())
        ++level;
    return level;
}

int32 GuildAdventureMgr::GetRenownPoints(std::string const& eventType) const
{
    auto itr = _renownSources.find(eventType);
    return itr != _renownSources.end() ? ScaleRenown(itr->second) : 0;
}

// ---------------------------------------------------------------------------------------------------------------
// State (characters database)
// ---------------------------------------------------------------------------------------------------------------

void GuildAdventureMgr::LoadState()
{
    _guilds.clear();
    _botOwner.clear();
    _botAccounts.clear();

    if (QueryResult result = CharacterDatabase.Query("SELECT owner_account, guild_id, leader_guid, team, unlocked_slots, "
        "renown_total, renown_level, offline_window_min, owner_logout_at, created_at FROM ga_guild"))
    {
        do
        {
            Field* fields = result->Fetch();
            AdventureGuild guild;
            guild.ownerAccount = fields[0].Get<uint32>();
            guild.guildId = fields[1].Get<uint32>();
            guild.leader = ObjectGuid::Create<HighGuid::Player>(fields[2].Get<uint32>());
            guild.team = fields[3].Get<uint8>() == TEAM_HORDE ? TEAM_HORDE : TEAM_ALLIANCE;
            guild.unlockedSlots = fields[4].Get<uint8>();
            guild.renownTotal = fields[5].Get<uint32>();
            guild.renownLevel = std::max<uint8>(1, fields[6].Get<uint8>());
            guild.offlineWindowMin = fields[7].Get<uint16>();
            guild.ownerLogoutAt = fields[8].Get<uint32>();
            guild.createdAt = fields[9].Get<uint32>();
            _guilds.emplace(guild.ownerAccount, std::move(guild));
        } while (result->NextRow());
    }

    LoadBotAccounts();
    LoadBots();
    LoadBotSnapshots();
    LoadLeaders();
    LoadOrders();
    LoadProfessions();
    LoadQuestBlocks();
    LoadUnlocks();
    LoadEvents();
    // Squads of guild bots (ga_bot_squad).
    sSquadService.Load();

    std::size_t bots = 0;
    for (auto const& [owner, guild] : _guilds)
        bots += guild.bots.size();
    LOG_INFO("module", "mod-guild-adventure: loaded {} adventure guilds, {} guild bots, {} bot accounts",
        _guilds.size(), bots, _botAccounts.size());
}

void GuildAdventureMgr::LoadBotAccounts()
{
    if (QueryResult result = CharacterDatabase.Query("SELECT account_id, owner_account, pool_index, created_at FROM ga_bot_account"))
    {
        do
        {
            Field* fields = result->Fetch();
            BotAccount account;
            account.accountId = fields[0].Get<uint32>();
            account.ownerAccount = fields[1].Get<uint32>();
            account.poolIndex = fields[2].Get<uint8>();
            account.createdAt = fields[3].Get<uint32>();
            _botAccounts.push_back(account);
        } while (result->NextRow());
    }
}

void GuildAdventureMgr::LoadBots()
{
    QueryResult result = CharacterDatabase.Query("SELECT b.bot_guid, b.owner_account, b.bot_account, b.slot, b.state, "
        "b.auto_advance, b.recruited_at, b.kills, b.deaths, b.quests, c.name, c.race, c.class, c.gender, c.level, c.xp, "
        "c.zone, c.map, c.money FROM ga_bot b LEFT JOIN characters c ON c.guid = b.bot_guid");
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        uint32 const lowGuid = fields[0].Get<uint32>();
        uint32 const owner = fields[1].Get<uint32>();

        auto guildItr = _guilds.find(owner);
        if (guildItr == _guilds.end())
        {
            LOG_ERROR("module", "mod-guild-adventure: ga_bot {} belongs to owner account {} without an adventure guild, skipped", lowGuid, owner);
            continue;
        }

        if (fields[10].IsNull())
        {
            LOG_ERROR("module", "mod-guild-adventure: ga_bot {} has no character any more, skipped (remove the row)", lowGuid);
            continue;
        }

        GuildBot bot;
        bot.guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
        bot.botAccount = fields[2].Get<uint32>();
        bot.slot = fields[3].Get<uint8>();
        bot.state = fields[4].Get<uint8>() == uint8(BotState::Benched) ? BotState::Benched : BotState::Active;
        bot.autoAdvance = fields[5].Get<bool>();
        bot.recruitedAt = fields[6].Get<uint32>();
        bot.kills = fields[7].Get<uint32>();
        bot.deaths = fields[8].Get<uint32>();
        bot.quests = fields[9].Get<uint32>();
        bot.name = fields[10].Get<std::string>();
        bot.race = fields[11].Get<uint8>();
        bot.classId = fields[12].Get<uint8>();
        bot.gender = fields[13].Get<uint8>();
        bot.snapshot.level = fields[14].Get<uint8>();
        bot.snapshot.xp = fields[15].Get<uint32>();
        bot.snapshot.zoneId = fields[16].Get<uint16>();
        bot.snapshot.mapId = fields[17].Get<uint16>();
        bot.snapshot.money = fields[18].Get<uint32>();

        _botOwner[bot.guid] = owner;
        guildItr->second.bots.emplace(bot.guid, std::move(bot));
    } while (result->NextRow());
}

void GuildAdventureMgr::LoadBotSnapshots()
{
    // The roster's gear level and professions of the bots that are not in the world yet (RosterService refreshes
    // both while a bot is in the world). Two reads at startup, no table of the module's own.
    auto findBot = [this](uint32 lowGuid) -> GuildBot*
    {
        ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
        auto owner = _botOwner.find(guid);
        if (owner == _botOwner.end())
            return nullptr;
        auto guild = _guilds.find(owner->second);
        return guild == _guilds.end() ? nullptr : guild->second.FindBot(guid);
    };

    std::string skillList;
    for (uint16 skill : PrimaryProfessionSkills)
        skillList += (skillList.empty() ? "" : ",") + std::to_string(skill);
    for (uint16 skill : SecondaryProfessionSkills)
        skillList += "," + std::to_string(skill);

    std::unordered_map<uint32, std::unordered_map<uint16, std::pair<uint32, uint32>>> skills;
    if (QueryResult result = CharacterDatabase.Query("SELECT s.guid, s.skill, s.value, s.max FROM character_skills s "
        "JOIN ga_bot b ON b.bot_guid = s.guid WHERE s.skill IN ({})", skillList))
    {
        do
        {
            Field* fields = result->Fetch();
            skills[fields[0].Get<uint32>()][fields[1].Get<uint16>()] = { fields[2].Get<uint32>(), fields[3].Get<uint32>() };
        } while (result->NextRow());
    }
    for (auto const& [lowGuid, known] : skills)
    {
        GuildBot* bot = findBot(lowGuid);
        if (!bot)
            continue;
        std::string summary;
        auto add = [&](uint16 skill)
        {
            auto itr = known.find(skill);
            if (itr == known.end())
                return;
            summary += (summary.empty() ? "" : ",") + Acore::StringFormat("{}:{}:{}", skill, itr->second.first, itr->second.second);
        };
        for (uint16 skill : PrimaryProfessionSkills)
            add(skill);
        for (uint16 skill : SecondaryProfessionSkills)
            add(skill);
        bot->snapshot.skills = summary;
    }

    std::unordered_map<uint32, std::unordered_map<uint8, uint32>> equipped; // item entry per equipment slot
    if (QueryResult result = CharacterDatabase.Query("SELECT ci.guid, ci.slot, ii.itemEntry FROM character_inventory ci "
        "JOIN item_instance ii ON ii.guid = ci.item JOIN ga_bot b ON b.bot_guid = ci.guid "
        "WHERE ci.bag = 0 AND ci.slot < {}", uint32(EQUIPMENT_SLOT_END)))
    {
        do
        {
            Field* fields = result->Fetch();
            equipped[fields[0].Get<uint32>()][fields[1].Get<uint8>()] = fields[2].Get<uint32>();
        } while (result->NextRow());
    }
    for (auto const& [lowGuid, slots] : equipped)
    {
        GuildBot* bot = findBot(lowGuid);
        if (!bot)
            continue;
        bot->snapshot.gearLevel = Readiness::GearLevelOf(bot->classId, bot->snapshot.level, [&slots](uint8 slot)
        {
            auto itr = slots.find(slot);
            ItemTemplate const* item = itr == slots.end() ? nullptr : sObjectMgr->GetItemTemplate(itr->second);
            return item ? item->ItemLevel : 0u;
        });
    }
}

void GuildAdventureMgr::LoadLeaders()
{
    QueryResult result = CharacterDatabase.Query("SELECT g.owner_account, c.name, c.race, c.class, c.gender, c.level, c.xp, "
        "c.zone, c.map, c.money FROM ga_guild g JOIN characters c ON c.guid = g.leader_guid");
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        auto itr = _guilds.find(fields[0].Get<uint32>());
        if (itr == _guilds.end())
            continue;

        GuildBot& plan = itr->second.leaderPlan;
        plan.guid = itr->second.leader;
        plan.name = fields[1].Get<std::string>();
        plan.race = fields[2].Get<uint8>();
        plan.classId = fields[3].Get<uint8>();
        plan.gender = fields[4].Get<uint8>();
        plan.snapshot.level = fields[5].Get<uint8>();
        plan.snapshot.xp = fields[6].Get<uint32>();
        plan.snapshot.zoneId = fields[7].Get<uint16>();
        plan.snapshot.mapId = fields[8].Get<uint16>();
        plan.snapshot.money = fields[9].Get<uint32>();
    } while (result->NextRow());
}

void GuildAdventureMgr::LoadOrders()
{
    QueryResult result = CharacterDatabase.Query("SELECT bot_guid, order_type, zone_id, level_cap, continent, no_dungeons, "
        "gold_cap, set_at FROM ga_bot_order WHERE queue_pos = 0");
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>());
        AdventureGuild* guild = FindGuildOfMember(guid);
        if (guild && guid == guild->leader)
            guild->leaderPlanned = true; // the leader has an order row: the planner plays it
        GuildBot* bot = guild ? guild->FindPlanned(guid) : nullptr;
        if (!bot)
            continue;

        std::string const token = fields[1].Get<std::string>();
        std::optional<OrderType> type = OrderTypeFromToken(token);
        if (!type)
        {
            LOG_ERROR("module", "mod-guild-adventure: bot {} has unknown order '{}', using AUTO", bot->name, token);
            type = OrderType::Auto;
        }

        bot->order.type = *type;
        bot->order.zoneId = fields[2].Get<uint32>();
        bot->order.levelCap = fields[3].Get<uint8>();
        bot->order.continent = fields[4].Get<int16>();
        bot->order.noDungeons = fields[5].Get<bool>();
        bot->order.goldCap = fields[6].Get<uint64>();
        bot->order.setAt = fields[7].Get<uint32>();
    } while (result->NextRow());

    // The target stock of farm orders (ga_bot_farm).
    if (QueryResult farm = CharacterDatabase.Query("SELECT bot_guid, target_stock FROM ga_bot_farm"))
    {
        do
        {
            Field* fields = farm->Fetch();
            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>());
            AdventureGuild* guild = FindGuildOfMember(guid);
            GuildBot* bot = guild ? guild->FindPlanned(guid) : nullptr;
            if (bot && bot->order.type == OrderType::Farm)
                bot->order.farmTarget = fields[1].Get<uint32>();
        } while (farm->NextRow());
    }
}

void GuildAdventureMgr::LoadProfessions()
{
    QueryResult result = CharacterDatabase.Query("SELECT bot_guid, skill_id, plan_slot, target_skill FROM ga_bot_profession_plan "
        "ORDER BY bot_guid, plan_slot, skill_id");
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>());
        AdventureGuild* guild = FindGuildOfMember(guid);
        GuildBot* bot = guild ? guild->FindMember(guid) : nullptr;
        if (!bot)
            continue;

        uint16 const skill = fields[1].Get<uint16>();
        uint8 const slot = fields[2].Get<uint8>();
        if (slot == 1 || slot == 2)
        {
            bot->professions.primary[slot - 1] = skill;
            bot->professions.target[slot - 1] = fields[3].Get<uint16>();
        }
        else
            bot->professions.secondary.push_back(skill);
    } while (result->NextRow());
}

void GuildAdventureMgr::LoadQuestBlocks()
{
    QueryResult result = CharacterDatabase.Query("SELECT bot_guid, quest_id, failures, blocked, reason, updated_at FROM ga_bot_quest_block");
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>());
        AdventureGuild* guild = FindGuildOfMember(guid);
        GuildBot* bot = guild ? guild->FindPlanned(guid) : nullptr;
        if (!bot)
            continue;

        QuestBlock& block = bot->questBlocks[fields[1].Get<uint32>()];
        block.failures = fields[2].Get<uint8>();
        block.blocked = fields[3].Get<bool>();
        block.reason = fields[4].Get<std::string>();
        block.updatedAt = fields[5].Get<uint32>();
    } while (result->NextRow());
}

void GuildAdventureMgr::LoadUnlocks()
{
    QueryResult result = CharacterDatabase.Query("SELECT owner_account, unlock_id FROM ga_guild_unlock");
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        if (AdventureGuild* guild = FindGuild(fields[0].Get<uint32>()))
            guild->ownedUnlocks.insert(fields[1].Get<uint32>());
    } while (result->NextRow());
}

void GuildAdventureMgr::LoadEvents()
{
    if (QueryResult result = CharacterDatabase.Query("SELECT MAX(id) FROM ga_event_log"))
        if (!result->Fetch()[0].IsNull())
            _nextEventId = result->Fetch()[0].Get<uint32>() + 1;

    for (auto& [owner, guild] : _guilds)
    {
        QueryResult result = CharacterDatabase.Query("SELECT id, bot_guid, event_type, message, level, zone_id, points, created_at "
            "FROM ga_event_log WHERE owner_account = {} ORDER BY id DESC LIMIT {}", owner, uint32(RecentEventCount));
        if (!result)
            continue;

        do
        {
            Field* fields = result->Fetch();
            GuildEvent event;
            event.id = fields[0].Get<uint32>();
            uint32 const bot = fields[1].Get<uint32>();
            event.bot = bot ? ObjectGuid::Create<HighGuid::Player>(bot) : ObjectGuid::Empty;
            event.type = fields[2].Get<std::string>();
            event.message = fields[3].Get<std::string>();
            event.level = fields[4].Get<uint8>();
            event.zoneId = fields[5].Get<uint32>();
            event.points = fields[6].Get<int32>();
            event.time = fields[7].Get<uint32>();
            guild.recentEvents.push_front(std::move(event)); // query is newest first; keep oldest first
        } while (result->NextRow());
    }
}

AdventureGuild* GuildAdventureMgr::FindGuild(uint32 ownerAccount)
{
    auto itr = _guilds.find(ownerAccount);
    return itr != _guilds.end() ? &itr->second : nullptr;
}

AdventureGuild* GuildAdventureMgr::FindGuildOfBot(ObjectGuid botGuid)
{
    auto itr = _botOwner.find(botGuid);
    return itr != _botOwner.end() ? FindGuild(itr->second) : nullptr;
}

AdventureGuild* GuildAdventureMgr::FindGuildOfMember(ObjectGuid guid)
{
    if (AdventureGuild* guild = FindGuildOfBot(guid))
        return guild;
    return FindGuildOfLeader(guid);
}

AdventureGuild* GuildAdventureMgr::FindGuildOfLeader(ObjectGuid leaderGuid)
{
    for (auto& [owner, guild] : _guilds)
        if (guild.leader == leaderGuid)
            return &guild;
    return nullptr;
}

AdventureGuild& GuildAdventureMgr::CreateGuild(uint32 ownerAccount, Player* leader, uint8 slots, uint16 offlineWindowMin)
{
    AdventureGuild guild;
    guild.ownerAccount = ownerAccount;
    guild.leader = leader->GetGUID();
    guild.team = leader->GetTeamId();
    guild.unlockedSlots = slots;
    guild.offlineWindowMin = offlineWindowMin;
    guild.createdAt = uint32(GameTime::GetGameTime().count());
    guild.leaderPlan.guid = guild.leader;
    guild.leaderPlan.name = leader->GetName();
    guild.leaderPlan.race = leader->getRace();
    guild.leaderPlan.classId = leader->getClass();
    guild.leaderPlan.gender = leader->getGender();
    guild.leaderPlan.snapshot.level = leader->GetLevel();

    AdventureGuild& stored = _guilds[ownerAccount] = std::move(guild);
    SaveGuild(stored);
    return stored;
}

void GuildAdventureMgr::SaveGuild(AdventureGuild const& guild) const
{
    CharacterDatabase.Execute("REPLACE INTO ga_guild (owner_account, guild_id, leader_guid, team, unlocked_slots, renown_total, "
        "renown_level, offline_window_min, owner_logout_at, created_at) VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
        guild.ownerAccount, guild.guildId, guild.leader.GetCounter(), uint32(guild.team), uint32(guild.unlockedSlots),
        guild.renownTotal, uint32(guild.renownLevel), uint32(guild.offlineWindowMin), guild.ownerLogoutAt, guild.createdAt);
}

void GuildAdventureMgr::AddBot(AdventureGuild& guild, GuildBot bot)
{
    _botOwner[bot.guid] = guild.ownerAccount;
    GuildBot& stored = guild.bots[bot.guid] = std::move(bot);
    SaveBot(guild, stored);
    SaveOrder(stored);
    SaveProfessions(stored);
}

void GuildAdventureMgr::SaveBot(AdventureGuild const& guild, GuildBot const& bot) const
{
    CharacterDatabase.Execute("REPLACE INTO ga_bot (bot_guid, owner_account, bot_account, slot, state, auto_advance, recruited_at, "
        "kills, deaths, quests) VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
        bot.guid.GetCounter(), guild.ownerAccount, bot.botAccount, uint32(bot.slot), uint32(bot.state), uint32(bot.autoAdvance ? 1 : 0),
        bot.recruitedAt, bot.kills, bot.deaths, bot.quests);
}

void GuildAdventureMgr::SaveOrder(GuildBot const& bot) const
{
    CharacterDatabase.Execute("REPLACE INTO ga_bot_order (bot_guid, queue_pos, order_type, zone_id, level_cap, continent, "
        "no_dungeons, gold_cap, set_at) VALUES ({}, 0, '{}', {}, {}, {}, {}, {}, {})",
        bot.guid.GetCounter(), ToToken(bot.order.type), bot.order.zoneId, uint32(bot.order.levelCap), int32(bot.order.continent),
        uint32(bot.order.noDungeons ? 1 : 0), bot.order.goldCap, bot.order.setAt);
    // The target stock of a farm order has a table of its own.
    if (bot.order.type == OrderType::Farm && bot.order.farmTarget)
        CharacterDatabase.Execute("REPLACE INTO ga_bot_farm (bot_guid, target_stock, set_at) VALUES ({}, {}, {})",
            bot.guid.GetCounter(), bot.order.farmTarget, bot.order.setAt);
    else
        CharacterDatabase.Execute("DELETE FROM ga_bot_farm WHERE bot_guid = {}", bot.guid.GetCounter());
}

void GuildAdventureMgr::ClearLeaderOrder(AdventureGuild& guild) const
{
    guild.leaderPlanned = false;
    guild.leaderPlan.order = BotOrder();
    guild.leaderPlan.questBlocks.clear();
    CharacterDatabase.Execute("DELETE FROM ga_bot_order WHERE bot_guid = {}", guild.leader.GetCounter());
    CharacterDatabase.Execute("DELETE FROM ga_bot_farm WHERE bot_guid = {}", guild.leader.GetCounter());
    CharacterDatabase.Execute("DELETE FROM ga_bot_quest_block WHERE bot_guid = {}", guild.leader.GetCounter());
}

void GuildAdventureMgr::SaveProfessions(GuildBot const& bot) const
{
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    trans->Append("DELETE FROM ga_bot_profession_plan WHERE bot_guid = {}", bot.guid.GetCounter());
    for (uint8 i = 0; i < 2; ++i)
        if (bot.professions.primary[i])
            trans->Append("INSERT INTO ga_bot_profession_plan (bot_guid, skill_id, plan_slot, target_skill) VALUES ({}, {}, {}, {})",
                bot.guid.GetCounter(), uint32(bot.professions.primary[i]), uint32(i + 1), uint32(bot.professions.target[i]));
    for (uint16 skill : bot.professions.secondary)
        trans->Append("INSERT INTO ga_bot_profession_plan (bot_guid, skill_id, plan_slot, target_skill) VALUES ({}, {}, 0, 0)",
            bot.guid.GetCounter(), uint32(skill));
    CharacterDatabase.CommitTransaction(trans);
}

void GuildAdventureMgr::SaveQuestBlock(GuildBot const& bot, uint32 questId) const
{
    auto itr = bot.questBlocks.find(questId);
    if (itr == bot.questBlocks.end())
    {
        CharacterDatabase.Execute("DELETE FROM ga_bot_quest_block WHERE bot_guid = {} AND quest_id = {}", bot.guid.GetCounter(), questId);
        return;
    }

    std::string reason = itr->second.reason;
    CharacterDatabase.EscapeString(reason);
    CharacterDatabase.Execute("REPLACE INTO ga_bot_quest_block (bot_guid, quest_id, failures, blocked, reason, updated_at) "
        "VALUES ({}, {}, {}, {}, '{}', {})", bot.guid.GetCounter(), questId, uint32(itr->second.failures),
        uint32(itr->second.blocked ? 1 : 0), reason, itr->second.updatedAt);
}

void GuildAdventureMgr::SaveUnlock(AdventureGuild const& guild, uint32 unlockId) const
{
    CharacterDatabase.Execute("INSERT IGNORE INTO ga_guild_unlock (owner_account, unlock_id, acquired_at) VALUES ({}, {}, {})",
        guild.ownerAccount, unlockId, uint32(GameTime::GetGameTime().count()));
}

std::vector<BotAccount> GuildAdventureMgr::GetBotAccounts(uint32 ownerAccount) const
{
    std::vector<BotAccount> result;
    for (BotAccount const& account : _botAccounts)
        if (account.ownerAccount == ownerAccount)
            result.push_back(account);
    std::sort(result.begin(), result.end(), [](BotAccount const& a, BotAccount const& b) { return a.poolIndex < b.poolIndex; });
    return result;
}

void GuildAdventureMgr::AddBotAccount(BotAccount const& account)
{
    _botAccounts.push_back(account);
    CharacterDatabase.Execute("INSERT INTO ga_bot_account (account_id, owner_account, pool_index, created_at) VALUES ({}, {}, {}, {})",
        account.accountId, account.ownerAccount, uint32(account.poolIndex), account.createdAt);
}

uint32 GuildAdventureMgr::CountBotsOnAccount(uint32 accountId) const
{
    uint32 count = 0;
    for (auto const& [owner, guild] : _guilds)
        for (auto const& [guid, bot] : guild.bots)
            if (bot.botAccount == accountId)
                ++count;
    return count;
}

void GuildAdventureMgr::PruneEventLog(uint32 keepDays) const
{
    if (!keepDays)
        return;

    uint32 const now = uint32(GameTime::GetGameTime().count());
    uint32 const age = keepDays * DAY;
    if (now <= age)
        return;

    // The window function keeps the derived table from being merged into the DELETE (MySQL refuses to delete from a
    // table it reads in a merged subquery).
    CharacterDatabase.Execute("DELETE e FROM ga_event_log e JOIN (SELECT id FROM (SELECT id, ROW_NUMBER() OVER "
        "(PARTITION BY owner_account ORDER BY id DESC) AS position FROM ga_event_log) ranked WHERE position > {}) old "
        "ON old.id = e.id WHERE e.created_at < {}", uint32(RecentEventCount), now - age);
}

GuildEvent const& GuildAdventureMgr::AddEvent(AdventureGuild& guild, GuildEvent event)
{
    event.id = _nextEventId++;
    if (!event.time)
        event.time = uint32(GameTime::GetGameTime().count());
    if (event.message.size() > 255)
        event.message.resize(255);

    std::string message = event.message;
    CharacterDatabase.EscapeString(message);
    std::string type = event.type;
    CharacterDatabase.EscapeString(type);
    CharacterDatabase.Execute("INSERT INTO ga_event_log (id, owner_account, bot_guid, event_type, message, level, zone_id, points, "
        "created_at) VALUES ({}, {}, {}, '{}', '{}', {}, {}, {}, {})",
        event.id, guild.ownerAccount, event.bot.GetCounter(), type, message, uint32(event.level), event.zoneId, event.points, event.time);

    guild.recentEvents.push_back(std::move(event));
    while (guild.recentEvents.size() > RecentEventCount)
        guild.recentEvents.pop_front();
    return guild.recentEvents.back();
}
