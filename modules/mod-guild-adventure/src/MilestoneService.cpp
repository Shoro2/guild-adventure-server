/*
 * MilestoneService implementation. See MilestoneService.h.
 */

#include "MilestoneService.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "GadvProtocol.h"
#include "GameTime.h"
#include "Guild.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildEvents.h"
#include "GuildMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ProgressionService.h"
#include "StorageService.h"
#include "StringFormat.h"
#include "World.h"
#include <algorithm>
#include <mutex>
#include <optional>

using namespace GuildAdventure;

namespace
{
    enum class HookType : uint8
    {
        Kill,
        Quest,
        Run
    };

    struct HookEvent
    {
        HookType type = HookType::Kill;
        ObjectGuid guid;
        uint32 a = 0;   // Kill: creature type; Run: 1 = raid
        uint32 b = 0;   // Run: instance id
        uint32 c = 0;   // Run: LFG dungeon id
    };

    std::mutex sHookLock;
    std::vector<HookEvent> sHookEvents;

    void Queue(HookEvent event)
    {
        std::lock_guard<std::mutex> lock(sHookLock);
        sHookEvents.push_back(event);
    }

    struct KindInfo
    {
        MilestoneKind kind;
        char const* token;
    };

    constexpr KindInfo Kinds[] =
    {
        { MilestoneKind::LevelClass, "level_class" },
        { MilestoneKind::LevelRace, "level_race" },
        { MilestoneKind::Skill, "skill" },
        { MilestoneKind::StorageItems, "storage_items" },
        { MilestoneKind::GuildGold, "guild_gold" },
        { MilestoneKind::Kills, "kills" },
        { MilestoneKind::Quests, "quests" },
        { MilestoneKind::Dungeons, "dungeons" },
        { MilestoneKind::Raids, "raids" },
    };

    std::optional<MilestoneKind> KindFromToken(std::string const& token)
    {
        for (KindInfo const& info : Kinds)
            if (token == info.token)
                return info.kind;
        return std::nullopt;
    }

    std::string SeriesKey(MilestoneKind kind, uint32 subject)
    {
        std::string key = ToToken(kind);
        if (subject)
            key += Acore::StringFormat("_{}", subject);
        return key;
    }

    // 1234567 -> "1,234,567"
    std::string FormatCount(uint64 value)
    {
        std::string digits = std::to_string(value);
        std::string text;
        int count = 0;
        for (auto itr = digits.rbegin(); itr != digits.rend(); ++itr)
        {
            if (count && count % 3 == 0)
                text.insert(text.begin(), ',');
            text.insert(text.begin(), *itr);
            ++count;
        }
        return text;
    }

    std::string CharacterName(ObjectGuid guid)
    {
        std::string name;
        if (!guid || !sCharacterCache->GetCharacterNameByGuid(guid, name) || name.empty())
            return {};
        return name;
    }

    std::vector<ObjectGuid> Members(AdventureGuild const& guild)
    {
        std::vector<ObjectGuid> members;
        members.reserve(guild.bots.size() + 1);
        if (guild.leader)
            members.push_back(guild.leader);
        for (auto const& [guid, bot] : guild.bots)
            members.push_back(guid);
        return members;
    }

    std::string JoinGuids(std::vector<ObjectGuid> const& guids)
    {
        std::string list;
        for (ObjectGuid guid : guids)
        {
            if (!list.empty())
                list += ',';
            list += std::to_string(guid.GetCounter());
        }
        return list;
    }

    uint32 LevelCap()
    {
        return sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    }
}

char const* GuildAdventure::ToToken(MilestoneKind kind)
{
    for (KindInfo const& info : Kinds)
        if (info.kind == kind)
            return info.token;
    return "unknown";
}

bool MilestoneSeries::IsSum() const
{
    return kind == MilestoneKind::Kills || kind == MilestoneKind::Quests || kind == MilestoneKind::Dungeons ||
        kind == MilestoneKind::Raids;
}

bool MilestoneSeries::IsLive() const
{
    return kind == MilestoneKind::StorageItems || kind == MilestoneKind::GuildGold;
}

MilestoneService& MilestoneService::Instance()
{
    static MilestoneService instance;
    return instance;
}

// ---------------------------------------------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------------------------------------------

void MilestoneService::LoadTemplates()
{
    _series.clear();
    _seriesByKey.clear();

    QueryResult result = WorldDatabase.Query("SELECT id, kind, subject, tier, threshold, points, requires_level, comment "
        "FROM ga_milestone_template ORDER BY kind, subject, tier");
    if (!result)
    {
        LOG_WARN("module", "mod-guild-adventure: ga_milestone_template is empty or missing: no tiered guild achievements");
        return;
    }

    uint32 tiers = 0;
    do
    {
        Field* fields = result->Fetch();
        std::optional<MilestoneKind> const kind = KindFromToken(fields[1].Get<std::string>());
        if (!kind)
        {
            LOG_ERROR("module", "mod-guild-adventure: ga_milestone_template {} has the unknown kind '{}'", fields[0].Get<uint32>(),
                fields[1].Get<std::string>());
            continue;
        }

        uint32 const subject = fields[2].Get<uint32>();
        std::string const key = SeriesKey(*kind, subject);
        auto found = _seriesByKey.find(key);
        if (found == _seriesByKey.end())
        {
            MilestoneSeries series;
            series.kind = *kind;
            series.subject = subject;
            series.key = key;
            series.name = fields[7].Get<std::string>();
            found = _seriesByKey.emplace(key, _series.size()).first;
            _series.push_back(std::move(series));
        }

        MilestoneTier tier;
        tier.id = fields[0].Get<uint32>();
        tier.tier = fields[3].Get<uint8>();
        tier.threshold = fields[4].Get<uint64>();
        tier.points = fields[5].Get<uint32>();
        tier.requiresLevel = fields[6].Get<uint8>();
        _series[found->second].tiers.push_back(tier);
        ++tiers;
    } while (result->NextRow());

    // The query sorted by tier inside a series already; keep the series in the order of the kinds, then subjects.
    std::stable_sort(_series.begin(), _series.end(), [](MilestoneSeries const& a, MilestoneSeries const& b)
    {
        return a.kind != b.kind ? uint8(a.kind) < uint8(b.kind) : a.subject < b.subject;
    });
    _seriesByKey.clear();
    for (std::size_t i = 0; i < _series.size(); ++i)
        _seriesByKey[_series[i].key] = i;

    LOG_INFO("module", "mod-guild-adventure: loaded {} tiered guild achievement series ({} tiers, {} active at level cap {})",
        _series.size(), tiers, GetActiveCount(), LevelCap());
}

void MilestoneService::Load()
{
    LoadTemplates();
    _guilds.clear();

    uint32 earned = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT owner_account, milestone_id, earned_by, earned_at, points, renown "
        "FROM ga_guild_milestone"))
    {
        do
        {
            Field* fields = result->Fetch();
            EarnedMilestone milestone;
            milestone.earnedBy = fields[2].Get<uint32>() ? ObjectGuid::Create<HighGuid::Player>(fields[2].Get<uint32>()) : ObjectGuid::Empty;
            milestone.earnedAt = fields[3].Get<uint32>();
            milestone.points = fields[4].Get<uint32>();
            milestone.renown = fields[5].Get<int32>();
            _guilds[fields[0].Get<uint32>()].earned[fields[1].Get<uint32>()] = milestone;
            ++earned;
        } while (result->NextRow());
    }

    if (QueryResult result = CharacterDatabase.Query("SELECT owner_account, counter_key, value FROM ga_guild_counter"))
    {
        do
        {
            Field* fields = result->Fetch();
            _guilds[fields[0].Get<uint32>()].counters[fields[1].Get<std::string>()] = fields[2].Get<uint64>();
        } while (result->NextRow());
    }

    // The members' levels, skills and money from the database (also the ones offline), and a first quest count.
    for (auto& [owner, guild] : sGuildAdventureMgr.GetGuilds())
        State(owner);

    // The first scan runs on the first world update.
    _scanTimer = GetSettings().milestonesScanSeconds * IN_MILLISECONDS;
    _loaded = true;
    LOG_INFO("module", "mod-guild-adventure: loaded {} reached guild achievement tiers of {} guilds", earned, _guilds.size());
}

MilestoneService::GuildState& MilestoneService::State(uint32 ownerAccount)
{
    GuildState& state = _guilds[ownerAccount];
    if (!state.loaded)
    {
        state.loaded = true;
        if (AdventureGuild* guild = sGuildAdventureMgr.FindGuild(ownerAccount))
            LoadGuild(*guild, state);
    }
    return state;
}

MilestoneService::GuildState const* MilestoneService::FindState(uint32 ownerAccount) const
{
    auto itr = _guilds.find(ownerAccount);
    return itr != _guilds.end() ? &itr->second : nullptr;
}

void MilestoneService::LoadGuild(AdventureGuild const& guild, GuildState& state)
{
    std::vector<ObjectGuid> const members = Members(guild);
    if (members.empty())
        return;
    std::string const list = JoinGuids(members);

    if (QueryResult result = CharacterDatabase.Query("SELECT guid, money, level, race, class FROM characters WHERE guid IN ({})", list))
    {
        do
        {
            Field* fields = result->Fetch();
            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>());
            state.money[guid] = fields[1].Get<uint32>();
            uint8 const level = fields[2].Get<uint8>();
            RaiseMax(state, SeriesKey(MilestoneKind::LevelClass, fields[4].Get<uint8>()), level, guid);
            RaiseMax(state, SeriesKey(MilestoneKind::LevelRace, fields[3].Get<uint8>()), level, guid);
        } while (result->NextRow());
    }

    std::string skills;
    for (MilestoneSeries const& series : _series)
        if (series.kind == MilestoneKind::Skill)
            skills += (skills.empty() ? "" : ",") + std::to_string(series.subject);
    if (!skills.empty())
        if (QueryResult result = CharacterDatabase.Query("SELECT guid, skill, value FROM character_skills WHERE guid IN ({}) AND skill IN ({})",
            list, skills))
        {
            do
            {
                Field* fields = result->Fetch();
                RaiseMax(state, SeriesKey(MilestoneKind::Skill, fields[1].Get<uint16>()), fields[2].Get<uint16>(),
                    ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>()));
            } while (result->NextRow());
        }

    // A guild without a quest count yet: the quests its characters completed before (the same quest once per character).
    std::string const quests = SeriesKey(MilestoneKind::Quests, 0);
    if (!state.counters.count(quests))
    {
        uint64 done = 0;
        if (QueryResult result = CharacterDatabase.Query("SELECT COUNT(*) FROM character_queststatus_rewarded WHERE guid IN ({})", list))
            done = result->Fetch()[0].Get<uint64>();
        state.counters[quests] = done;
        state.dirty.insert(quests);
    }

    uint64 copper = 0;
    if (Guild* wowGuild = guild.guildId ? sGuildMgr->GetGuildById(guild.guildId) : nullptr)
        copper = wowGuild->GetTotalBankMoney();
    for (auto const& [guid, money] : state.money)
        copper += money;
    state.goldValue = copper / GOLD;
}

// ---------------------------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------------------------

bool MilestoneService::CountsAsKill(uint32 creatureType)
{
    switch (creatureType)
    {
        case CREATURE_TYPE_BEAST:
        case CREATURE_TYPE_DRAGONKIN:
        case CREATURE_TYPE_DEMON:
        case CREATURE_TYPE_ELEMENTAL:
        case CREATURE_TYPE_GIANT:
        case CREATURE_TYPE_UNDEAD:
        case CREATURE_TYPE_HUMANOID:
        case CREATURE_TYPE_MECHANICAL:
            return true;
        default:
            return false;
    }
}

void MilestoneService::QueueKill(ObjectGuid credited, uint32 creatureType)
{
    if (CountsAsKill(creatureType))
        Queue({ HookType::Kill, credited, creatureType });
}

void MilestoneService::QueueQuest(ObjectGuid character)
{
    Queue({ HookType::Quest, character });
}

void MilestoneService::QueueRun(ObjectGuid character, bool raid, uint32 instanceId, uint32 dungeonId)
{
    Queue({ HookType::Run, character, raid ? 1u : 0u, instanceId, dungeonId });
}

// ---------------------------------------------------------------------------------------------------------------
// Counting
// ---------------------------------------------------------------------------------------------------------------

void MilestoneService::RaiseMax(GuildState& state, std::string const& key, uint64 value, ObjectGuid holder)
{
    uint64& current = state.counters[key];
    if (value < current)
        return;
    if (value == current)
    {
        // The value came from ga_guild_counter: the first member seen at it is the holder.
        if (value && !state.holders.count(key))
            state.holders[key] = holder;
        return;
    }
    current = value;
    state.holders[key] = holder;
    state.dirty.insert(key);
}

void MilestoneService::AddSum(GuildState& state, std::string const& key, uint64 amount)
{
    if (!amount)
        return;
    state.counters[key] += amount;
    state.dirty.insert(key);
}

void MilestoneService::WriteCounters(uint32 ownerAccount, GuildState& state, bool direct)
{
    if (state.dirty.empty())
        return;
    uint32 const now = uint32(GameTime::GetGameTime().count());
    for (std::string const& key : state.dirty)
    {
        // The keys are the module's own ("kills_7", "skill_164"): nothing to escape.
        std::string const sql = Acore::StringFormat("REPLACE INTO ga_guild_counter (owner_account, counter_key, value, updated_at) "
            "VALUES ({}, '{}', {}, {})", ownerAccount, key, state.counters[key], now);
        if (direct)
            CharacterDatabase.DirectExecute(sql);
        else
            CharacterDatabase.Execute(sql);
    }
    state.dirty.clear();
}

void MilestoneService::Flush()
{
    for (auto& [owner, state] : _guilds)
        WriteCounters(owner, state, true);
}

void MilestoneService::Update(uint32 diff)
{
    if (!_loaded || !GetSettings().milestonesEnable)
        return;

    ProcessQueue();

    _scanTimer += diff;
    if (_scanTimer >= GetSettings().milestonesScanSeconds * IN_MILLISECONDS)
    {
        _scanTimer = 0;
        ScanAll();
    }
}

void MilestoneService::ProcessQueue()
{
    std::vector<HookEvent> events;
    {
        std::lock_guard<std::mutex> lock(sHookLock);
        if (sHookEvents.empty())
            return;
        events.swap(sHookEvents);
    }

    std::map<uint32, std::vector<Award>> awards;
    for (HookEvent const& event : events)
    {
        AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(event.guid);
        if (!guild)
            continue;
        GuildState& state = State(guild->ownerAccount);

        std::string key;
        switch (event.type)
        {
            case HookType::Kill:
                key = SeriesKey(MilestoneKind::Kills, event.a);
                break;
            case HookType::Quest:
                key = SeriesKey(MilestoneKind::Quests, 0);
                break;
            case HookType::Run:
                // A last boss killed twice in one instance (a respawn, a second credit) is still one run.
                if (!state.runs.insert({ event.b, event.c }).second)
                    continue;
                key = SeriesKey(event.a ? MilestoneKind::Raids : MilestoneKind::Dungeons, 0);
                break;
        }

        AddSum(state, key, 1);
        if (MilestoneSeries const* series = FindSeries(key))
            Evaluate(*guild, state, *series, event.guid, awards[guild->ownerAccount]);
    }

    for (auto& [owner, list] : awards)
        if (!list.empty())
            if (AdventureGuild* guild = sGuildAdventureMgr.FindGuild(owner))
                Announce(*guild, list);
}

void MilestoneService::ScanMembers(AdventureGuild const& guild, GuildState& state)
{
    for (ObjectGuid guid : Members(guild))
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!player || !player->IsInWorld())
            continue;

        uint8 const level = player->GetLevel();
        RaiseMax(state, SeriesKey(MilestoneKind::LevelClass, player->getClass()), level, guid);
        RaiseMax(state, SeriesKey(MilestoneKind::LevelRace, player->getRace(true)), level, guid);
        for (MilestoneSeries const& series : _series)
            if (series.kind == MilestoneKind::Skill)
                if (uint16 const value = player->GetPureSkillValue(series.subject))
                    RaiseMax(state, series.key, value, guid);
        state.money[guid] = player->GetMoney();
    }

    uint64 copper = 0;
    if (Guild* wowGuild = guild.guildId ? sGuildMgr->GetGuildById(guild.guildId) : nullptr)
        copper = wowGuild->GetTotalBankMoney();
    for (ObjectGuid guid : Members(guild))
    {
        auto itr = state.money.find(guid);
        if (itr != state.money.end())
            copper += itr->second;
    }
    state.goldValue = copper / GOLD;
}

void MilestoneService::ScanAll()
{
    for (auto& [owner, guild] : sGuildAdventureMgr.GetGuilds())
        ScanGuild(guild);
}

void MilestoneService::ScanGuild(AdventureGuild& guild)
{
    GuildState& state = State(guild.ownerAccount);
    ScanMembers(guild, state);
    std::vector<Award> awards;
    EvaluateAll(guild, state, awards);
    Announce(guild, awards);
    WriteCounters(guild.ownerAccount, state, false);
}

void MilestoneService::AddRun(AdventureGuild& guild, bool raid, ObjectGuid by)
{
    AddToCounter(guild, SeriesKey(raid ? MilestoneKind::Raids : MilestoneKind::Dungeons, 0), 1, by);
}

bool MilestoneService::AddToCounter(AdventureGuild& guild, std::string const& key, uint64 amount, ObjectGuid by)
{
    MilestoneSeries const* series = FindSeries(key);
    if (!series || series->IsLive())
        return false;

    GuildState& state = State(guild.ownerAccount);
    if (series->IsSum())
        AddSum(state, key, amount);
    else
        RaiseMax(state, key, amount, by);

    std::vector<Award> awards;
    Evaluate(guild, state, *series, by, awards);
    Announce(guild, awards);
    WriteCounters(guild.ownerAccount, state, false);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Tiers
// ---------------------------------------------------------------------------------------------------------------

void MilestoneService::Evaluate(AdventureGuild const& guild, GuildState& state, MilestoneSeries const& series, ObjectGuid by,
    std::vector<Award>& awards)
{
    if (!GetSettings().milestonesEnable)
        return;

    uint64 const value = GetValue(guild, series);
    uint8 const active = GetActiveTiers(series);
    if (!series.IsSum() && !series.IsLive())
    {
        auto holder = state.holders.find(series.key);
        by = holder != state.holders.end() ? holder->second : ObjectGuid::Empty;
    }
    else if (series.IsLive())
        by = ObjectGuid::Empty;

    for (std::size_t i = 0; i < series.tiers.size() && i < active; ++i)
    {
        MilestoneTier const& tier = series.tiers[i];
        if (value < tier.threshold || state.earned.count(tier.id))
            continue;

        EarnedMilestone earned;
        earned.earnedBy = by;
        earned.earnedAt = uint32(GameTime::GetGameTime().count());
        earned.points = GetTierPoints(series, i);
        earned.renown = ScaleRenown(int64(earned.points) * GetSettings().achievementsRenownPerPoint);
        state.earned[tier.id] = earned;

        CharacterDatabase.Execute("INSERT IGNORE INTO ga_guild_milestone (owner_account, milestone_id, earned_by, earned_at, points, renown) "
            "VALUES ({}, {}, {}, {}, {}, {})", guild.ownerAccount, tier.id, by.GetCounter(), earned.earnedAt, earned.points, earned.renown);
        awards.push_back({ &series, &tier, by, earned.points });
    }
}

void MilestoneService::EvaluateAll(AdventureGuild const& guild, GuildState& state, std::vector<Award>& awards)
{
    for (MilestoneSeries const& series : _series)
        Evaluate(guild, state, series, ObjectGuid::Empty, awards);
}

void MilestoneService::Announce(AdventureGuild& guild, std::vector<Award> const& awards)
{
    if (awards.empty())
        return;

    uint32 points = 0;
    for (Award const& award : awards)
        points += award.points;
    int32 const renown = ScaleRenown(int64(points) * GetSettings().achievementsRenownPerPoint);

    auto eventBot = [&guild](ObjectGuid by) { return guild.FindBot(by) ? by : ObjectGuid::Empty; };

    if (awards.size() <= 3)
    {
        for (Award const& award : awards)
        {
            std::string const tier = DescribeTier(*award.series, *award.tier);
            std::string const name = CharacterName(award.by);
            int32 const tierRenown = ScaleRenown(int64(award.points) * GetSettings().achievementsRenownPerPoint);
            RecordEvent(guild, "MILESTONE", eventBot(award.by), name.empty() ?
                Acore::StringFormat("The guild reached \"{}\" (+{} guild points).", tier, award.points) :
                Acore::StringFormat("{} reached \"{}\" for the guild (+{} guild points).", name, tier, award.points),
                uint8(award.tier->tier), 0, tierRenown);
        }
    }
    else
    {
        std::string names;
        for (std::size_t i = 0; i < awards.size() && i < 3; ++i)
            names += (i ? ", \"" : "\"") + DescribeTier(*awards[i].series, *awards[i].tier) + "\"";
        RecordEvent(guild, "MILESTONE", ObjectGuid::Empty, Acore::StringFormat("The guild reached {} tiered achievements (+{} guild "
            "points): {} and {} more.", awards.size(), points, names, awards.size() - 3), 0, 0, renown);
    }

    LOG_INFO("module", "mod-guild-adventure: owner {} reached {} guild achievement tiers, +{} guild points, +{} renown",
        guild.ownerAccount, awards.size(), points, renown);

    // AddRenown pushes the STATE record (renown, guild points); without renown the push is ours.
    if (renown)
        sProgressionService.AddRenown(guild, renown);
    else
        PushGuildState(guild);
    sGadvProtocol.PushMilestones(guild);
}

// ---------------------------------------------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------------------------------------------

MilestoneSeries const* MilestoneService::FindSeries(std::string const& key) const
{
    auto itr = _seriesByKey.find(key);
    return itr != _seriesByKey.end() ? &_series[itr->second] : nullptr;
}

uint64 MilestoneService::GetValue(AdventureGuild const& guild, MilestoneSeries const& series) const
{
    if (series.kind == MilestoneKind::StorageItems)
        return sStorageService.GetTotals(guild.ownerAccount).second;

    GuildState const* state = FindState(guild.ownerAccount);
    if (!state)
        return 0;
    if (series.kind == MilestoneKind::GuildGold)
        return state->goldValue;
    auto itr = state->counters.find(series.key);
    return itr != state->counters.end() ? itr->second : 0;
}

bool MilestoneService::HasEarned(AdventureGuild const& guild, uint32 milestoneId) const
{
    GuildState const* state = FindState(guild.ownerAccount);
    return state && state->earned.count(milestoneId);
}

uint8 MilestoneService::GetActiveTiers(MilestoneSeries const& series) const
{
    uint32 const cap = LevelCap();
    uint8 active = 0;
    for (MilestoneTier const& tier : series.tiers)
    {
        if (tier.requiresLevel > cap)
            break;
        ++active;
    }
    return active;
}

uint32 MilestoneService::GetTierPoints(MilestoneSeries const& series, std::size_t index) const
{
    if (index >= series.tiers.size())
        return 0;
    uint32 const base = series.tiers[index].points;
    uint8 const active = GetActiveTiers(series);
    return active && index + 1 == active ? base * GetSettings().milestonesLastTierFactor : base;
}

std::string MilestoneService::DescribeTier(MilestoneSeries const& series, MilestoneTier const& tier) const
{
    std::string const count = FormatCount(tier.threshold);
    switch (series.kind)
    {
        case MilestoneKind::LevelClass:
        case MilestoneKind::LevelRace:
            return Acore::StringFormat("{} level {}", series.name, tier.threshold);
        case MilestoneKind::Skill:
            return Acore::StringFormat("{} {}", series.name, tier.threshold);
        case MilestoneKind::StorageItems:
            return Acore::StringFormat("{} items in the guild storage", count);
        case MilestoneKind::GuildGold:
            return Acore::StringFormat("{} gold in the guild", count);
        case MilestoneKind::Kills:
            return Acore::StringFormat("{} {} slain", count, series.name);
        case MilestoneKind::Quests:
            return Acore::StringFormat("{} quests completed", count);
        case MilestoneKind::Dungeons:
            return Acore::StringFormat("{} dungeon run{}", count, tier.threshold == 1 ? "" : "s");
        case MilestoneKind::Raids:
            return Acore::StringFormat("{} raid run{}", count, tier.threshold == 1 ? "" : "s");
    }
    return count;
}

uint32 MilestoneService::GetEarnedCount(AdventureGuild const& guild) const
{
    GuildState const* state = FindState(guild.ownerAccount);
    return state ? uint32(state->earned.size()) : 0;
}

uint32 MilestoneService::GetActiveCount() const
{
    uint32 count = 0;
    for (MilestoneSeries const& series : _series)
        count += GetActiveTiers(series);
    return count;
}

uint32 MilestoneService::GetPoints(AdventureGuild const& guild) const
{
    GuildState const* state = FindState(guild.ownerAccount);
    uint32 points = 0;
    if (state)
        for (auto const& [id, earned] : state->earned)
            points += earned.points;
    return points;
}

int32 MilestoneService::GetRenown(AdventureGuild const& guild) const
{
    GuildState const* state = FindState(guild.ownerAccount);
    int32 renown = 0;
    if (state)
        for (auto const& [id, earned] : state->earned)
            renown += earned.renown;
    return renown;
}
