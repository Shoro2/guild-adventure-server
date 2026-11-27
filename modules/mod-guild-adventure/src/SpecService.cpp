/*
 * SpecService implementation. See SpecService.h.
 */

#include "SpecService.h"
#include "BotDriver.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GuildAdventureSettings.h"
#include "Log.h"
#include "Optional.h"
#include "Player.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include <algorithm>
#include <map>

using namespace GuildAdventure;

namespace
{
    constexpr uint32 MaxGuildTemplates = 40;
    constexpr std::size_t MaxNameLength = 40;

    // (class, tab, row, column) -> talent, built on first use.
    std::unordered_map<uint32, TalentEntry const*> talentIndex;
    // The talents of each class, by tree.
    std::unordered_map<uint8, std::vector<std::pair<uint8, TalentEntry const*>>> classTalents;

    uint32 TalentKey(uint8 classId, uint8 tab, uint8 row, uint8 col)
    {
        return (uint32(classId) << 24) | (uint32(tab) << 16) | (uint32(row) << 8) | col;
    }

    void BuildTalentIndex()
    {
        if (!talentIndex.empty())
            return;
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* talent = sTalentStore.LookupEntry(i);
            TalentTabEntry const* tab = talent ? sTalentTabStore.LookupEntry(talent->TalentTab) : nullptr;
            if (!tab || !tab->ClassMask || tab->tabpage > 2)
                continue;
            for (uint8 classId = 1; classId < MAX_CLASSES; ++classId)
                if (tab->ClassMask & (1u << (classId - 1)))
                {
                    talentIndex[TalentKey(classId, uint8(tab->tabpage), uint8(talent->Row), uint8(talent->Col))] = talent;
                    classTalents[classId].emplace_back(uint8(tab->tabpage), talent);
                }
        }
    }

    TalentEntry const* FindTalent(uint8 classId, SpecPoint const& point)
    {
        BuildTalentIndex();
        auto itr = talentIndex.find(TalentKey(classId, point.tab, point.row, point.col));
        return itr != talentIndex.end() ? itr->second : nullptr;
    }

    uint8 RankOf(Player const* player, TalentEntry const* talent)
    {
        for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
            if (talent->RankID[rank] && player->HasTalent(talent->RankID[rank], player->GetActiveSpec()))
                return uint8(rank + 1);
        return 0;
    }

    std::array<uint32, 3> PointsPerTab(Player const* player)
    {
        BuildTalentIndex();
        std::array<uint32, 3> points = { 0, 0, 0 };
        for (auto const& [tab, talent] : classTalents[player->getClass()])
            points[tab] += RankOf(player, talent);
        return points;
    }

    uint8 TopTab(std::array<uint32, 3> const& points)
    {
        return uint8(std::max_element(points.begin(), points.end()) - points.begin());
    }

    // True if every talent the character has fits into the template with the points it has (so its free points can
    // simply go on along the template).
    bool FitsTemplate(Player const* player, SpecTemplate const& spec)
    {
        std::array<uint32, 3> const have = PointsPerTab(player);
        uint32 const total = have[0] + have[1] + have[2] + player->GetFreeTalentPoints();
        std::unordered_map<uint32, uint8> wanted;
        for (std::size_t i = 0; i < spec.points.size() && i < total; ++i)
            if (TalentEntry const* talent = FindTalent(spec.classId, spec.points[i]))
                ++wanted[talent->TalentID];
        for (auto const& [tab, talent] : classTalents[player->getClass()])
            if (uint8 const rank = RankOf(player, talent))
            {
                auto itr = wanted.find(talent->TalentID);
                if (itr == wanted.end() || rank > itr->second)
                    return false;
            }
        return true;
    }

    std::vector<SpecPoint> ParsePoints(std::string const& text)
    {
        std::vector<SpecPoint> points;
        for (std::string_view token : Acore::Tokenize(text, ' ', false))
        {
            std::vector<std::string_view> const parts = Acore::Tokenize(token, '.', true);
            if (parts.size() != 3)
                continue;
            Optional<uint8> const tab = Acore::StringTo<uint8>(parts[0]);
            Optional<uint8> const row = Acore::StringTo<uint8>(parts[1]);
            Optional<uint8> const col = Acore::StringTo<uint8>(parts[2]);
            if (tab && row && col && *tab <= 2)
                points.push_back({ *tab, *row, *col });
        }
        return points;
    }

    std::string FormatPoints(std::vector<SpecPoint> const& points)
    {
        std::string text;
        for (SpecPoint const& point : points)
            text += Acore::StringFormat("{}{}.{}.{}", text.empty() ? "" : " ", uint32(point.tab), uint32(point.row), uint32(point.col));
        return text;
    }

    std::string Summary(std::array<uint32, 3> const& points)
    {
        return Acore::StringFormat("{}/{}/{}", points[0], points[1], points[2]);
    }

    void StoreAssignment(ObjectGuid::LowType bot, uint32 templateId, bool resetPending)
    {
        CharacterDatabase.Execute("REPLACE INTO ga_bot_spec (bot_guid, template_id, reset_pending, set_at) VALUES ({}, {}, {}, {})",
            bot, templateId, resetPending ? 1 : 0, uint32(GameTime::GetGameTime().count()));
    }
}

std::string SpecTemplate::Summary(uint8 level) const
{
    std::array<uint32, 3> perTab = { 0, 0, 0 };
    std::size_t const count = level >= 10 ? std::min<std::size_t>(points.size(), std::size_t(level) - 9) : 0;
    for (std::size_t i = 0; i < count; ++i)
        ++perTab[points[i].tab];
    return ::Summary(perTab);
}

SpecService& SpecService::instance()
{
    static SpecService service;
    return service;
}

void SpecService::Load()
{
    _templates.clear();
    _defaults.clear();
    _assigned.clear();
    _pendingReset.clear();
    _nextGuildId = FirstGuildSpecId;

    if (QueryResult result = WorldDatabase.Query("SELECT id, class, name, purpose, role, tab, is_default, points FROM ga_spec_template"))
    {
        do
        {
            Field* fields = result->Fetch();
            SpecTemplate spec;
            spec.id = fields[0].Get<uint32>();
            spec.classId = fields[1].Get<uint8>();
            spec.name = fields[2].Get<std::string>();
            spec.purpose = fields[3].Get<uint8>();
            spec.role = fields[4].Get<uint8>();
            spec.tab = fields[5].Get<uint8>();
            spec.isDefault = fields[6].Get<uint8>() != 0;
            spec.points = ParsePoints(fields[7].Get<std::string>());
            if (spec.id < 2 || spec.id >= FirstGuildSpecId || spec.points.empty())
            {
                LOG_ERROR("module", "mod-guild-adventure: ga_spec_template row {} skipped (id out of range or no points)", spec.id);
                continue;
            }
            if (spec.isDefault)
                _defaults[spec.classId] = spec.id;
            _templates[spec.id] = std::move(spec);
        } while (result->NextRow());
    }
    uint32 const builtIn = uint32(_templates.size());

    if (QueryResult result = CharacterDatabase.Query("SELECT id, owner_account, class, name, purpose, role, tab, points FROM ga_guild_spec_template"))
    {
        do
        {
            Field* fields = result->Fetch();
            SpecTemplate spec;
            spec.id = fields[0].Get<uint32>();
            spec.owner = fields[1].Get<uint32>();
            spec.classId = fields[2].Get<uint8>();
            spec.name = fields[3].Get<std::string>();
            spec.purpose = fields[4].Get<uint8>();
            spec.role = fields[5].Get<uint8>();
            spec.tab = fields[6].Get<uint8>();
            spec.points = ParsePoints(fields[7].Get<std::string>());
            _nextGuildId = std::max(_nextGuildId, spec.id + 1);
            if (spec.id >= FirstGuildSpecId && spec.owner && !spec.points.empty())
                _templates[spec.id] = std::move(spec);
        } while (result->NextRow());
    }

    if (QueryResult result = CharacterDatabase.Query("SELECT bot_guid, template_id, reset_pending FROM ga_bot_spec"))
    {
        do
        {
            Field* fields = result->Fetch();
            ObjectGuid::LowType const bot = fields[0].Get<uint32>();
            _assigned[bot] = fields[1].Get<uint32>();
            if (fields[2].Get<uint8>())
                _pendingReset[bot] = true;
        } while (result->NextRow());
    }

    LOG_INFO("module", "mod-guild-adventure: loaded {} talent templates ({} of guilds), {} class defaults, {} bot assignments",
        _templates.size(), _templates.size() - builtIn, _defaults.size(), _assigned.size());
}

SpecTemplate const* SpecService::Get(uint32 templateId) const
{
    auto itr = _templates.find(templateId);
    return itr != _templates.end() ? &itr->second : nullptr;
}

SpecTemplate const* SpecService::GetDefault(uint8 classId) const
{
    auto itr = _defaults.find(classId);
    return itr != _defaults.end() ? Get(itr->second) : nullptr;
}

std::vector<SpecTemplate const*> SpecService::ListFor(uint32 owner, uint8 classId) const
{
    std::vector<SpecTemplate const*> list;
    for (auto const& [id, spec] : _templates)
        if (spec.classId == classId && (!spec.owner || spec.owner == owner))
            list.push_back(&spec);
    std::sort(list.begin(), list.end(), [](SpecTemplate const* a, SpecTemplate const* b) { return a->id < b->id; });
    return list;
}

uint32 SpecService::GetAssigned(ObjectGuid botGuid) const
{
    auto itr = _assigned.find(botGuid.GetCounter());
    return itr != _assigned.end() ? itr->second : SpecDefault;
}

void SpecService::SetRunTemplate(ObjectGuid botGuid, uint32 templateId)
{
    _runTemplate[botGuid.GetCounter()] = templateId;
}

void SpecService::ClearRunTemplate(ObjectGuid botGuid)
{
    _runTemplate.erase(botGuid.GetCounter());
}

bool SpecService::HasRunTemplate(ObjectGuid botGuid) const
{
    return _runTemplate.find(botGuid.GetCounter()) != _runTemplate.end();
}

std::string SpecService::ApplyTemplate(Player* bot, SpecTemplate const& spec)
{
    if (!bot || bot->GetLevel() < 10 || spec.classId != bot->getClass())
        return "";
    return Apply(bot, spec, !FitsTemplate(bot, spec));
}

SpecTemplate const* SpecService::GetEffective(ObjectGuid botGuid, uint8 classId) const
{
    // On a dungeon run: the template of its role (the second talent spec is active).
    if (auto run = _runTemplate.find(botGuid.GetCounter()); run != _runTemplate.end())
        if (SpecTemplate const* spec = Get(run->second); spec && spec->classId == classId)
            return spec;
    return GetLevelling(botGuid, classId);
}

SpecTemplate const* SpecService::GetLevelling(ObjectGuid botGuid, uint8 classId) const
{
    uint32 const assigned = GetAssigned(botGuid);
    if (assigned == SpecManual)
        return nullptr;
    SpecTemplate const* spec = assigned == SpecDefault ? nullptr : Get(assigned);
    return spec && spec->classId == classId ? spec : GetDefault(classId);
}

bool SpecService::HasPendingReset(ObjectGuid botGuid) const
{
    return _pendingReset.find(botGuid.GetCounter()) != _pendingReset.end();
}

std::string SpecService::CurrentSummary(Player* player)
{
    return ::Summary(PointsPerTab(player));
}

std::string SpecService::Apply(Player* bot, SpecTemplate const& spec, bool reset)
{
    std::array<uint32, 3> const before = PointsPerTab(bot);
    bool const hadTalents = before[0] + before[1] + before[2] > 0;
    if (reset && hadTalents)
        bot->resetTalents(true);

    uint32 learned = 0;
    std::unordered_map<uint32, uint8> wanted;
    for (SpecPoint const& point : spec.points)
    {
        if (!bot->GetFreeTalentPoints())
            break;
        TalentEntry const* talent = FindTalent(spec.classId, point);
        if (!talent)
            continue;
        uint8 const rank = ++wanted[talent->TalentID];
        uint8 const have = RankOf(bot, talent);
        if (have >= rank)
            continue;
        bot->LearnTalent(talent->TalentID, rank - 1);
        learned += RankOf(bot, talent) - have;
    }

    bool const wasReset = reset && hadTalents;
    if (!wasReset && !learned)
        return "";

    std::array<uint32, 3> const after = PointsPerTab(bot);
    bot->SendTalentsInfoData(false);
    // mod-playerbots picks damage, tank or healer strategies (its rotation) from the tree with the most points.
    if (wasReset || !hadTalents || TopTab(before) != TopTab(after))
        BotDriver::ResetStrategies(bot);
    return Acore::StringFormat("{}{} talent point{} spent by \"{}\": {}", wasReset ? "talents reset, " : "", learned,
        learned == 1 ? "" : "s", spec.name, ::Summary(after));
}

std::string SpecService::Maintain(Player* bot)
{
    if (!GetSettings().specsEnable || !bot || bot->GetLevel() < 10)
        return "";

    ObjectGuid::LowType const low = bot->GetGUID().GetCounter();
    // On a dungeon run the second talent spec is active: its free points go along the template of the role. A
    // template assigned meanwhile waits for the levelling spec.
    if (auto run = _runTemplate.find(low); run != _runTemplate.end())
    {
        SpecTemplate const* spec = Get(run->second);
        return spec && spec->classId == bot->getClass() && bot->GetFreeTalentPoints() ? Apply(bot, *spec, false) : std::string();
    }
    auto assigned = _assigned.find(low);
    bool const firstTime = assigned == _assigned.end();
    if (!firstTime && assigned->second == SpecManual)
        return "";

    SpecTemplate const* spec = GetEffective(bot->GetGUID(), bot->getClass());
    if (!spec)
        return "";

    bool reset = false;
    if (auto pending = _pendingReset.find(low); pending != _pendingReset.end())
    {
        reset = true;
        _pendingReset.erase(pending);
        StoreAssignment(low, firstTime ? SpecDefault : assigned->second, false);
    }
    if (firstTime)
    {
        // Talents chosen before the templates existed (by mod-playerbots' premade specs): once, they give way.
        reset = !FitsTemplate(bot, *spec);
        _assigned[low] = SpecDefault;
        StoreAssignment(low, SpecDefault, false);
    }
    if (!reset && !bot->GetFreeTalentPoints())
        return "";
    return Apply(bot, *spec, reset);
}

bool SpecService::Assign(uint32 owner, ObjectGuid botGuid, uint8 classId, uint32 templateId, Player* botInWorld, std::string& error)
{
    if (templateId != SpecDefault && templateId != SpecManual)
    {
        SpecTemplate const* spec = Get(templateId);
        if (!spec || (spec->owner && spec->owner != owner))
        {
            error = "no such template";
            return false;
        }
        if (spec->classId != classId)
        {
            error = "the template is for another class";
            return false;
        }
    }

    ObjectGuid::LowType const low = botGuid.GetCounter();
    _assigned[low] = templateId;
    _pendingReset.erase(low);
    if (templateId == SpecManual)
    {
        StoreAssignment(low, templateId, false);
        return true;
    }

    // (On a dungeon run the second talent spec is active: the levelling spec gets the template after the run.)
    SpecTemplate const* spec = GetLevelling(botGuid, classId);
    bool const now = spec && botInWorld && botInWorld->IsAlive() && !botInWorld->IsInCombat() && botInWorld->GetLevel() >= 10 &&
        !HasRunTemplate(botGuid);
    if (now)
    {
        if (std::string const done = Apply(botInWorld, *spec, true); !done.empty())
            LOG_INFO("module.ga.activity", "activity: {} (level {}) {}", botInWorld->GetName(), uint32(botInWorld->GetLevel()), done);
    }
    else if (spec)
    {
        _pendingReset[low] = true;
        if (botInWorld)
            LOG_INFO("module.ga.activity", "activity: {} gets the talent template \"{}\" at its next quiet moment",
                botInWorld->GetName(), spec->name);
    }
    StoreAssignment(low, templateId, !now && spec != nullptr);
    return true;
}

uint32 SpecService::SaveFrom(uint32 owner, Player* player, std::string const& name, uint8 purpose, std::string& error)
{
    if (!owner || !player)
    {
        error = "no character";
        return 0;
    }
    std::string clean;
    for (char c : name)
        if (static_cast<unsigned char>(c) >= 0x20 && c != '^' && c != '|' && c != '~')
            clean += c;
    while (!clean.empty() && clean.back() == ' ')
        clean.pop_back();
    if (clean.empty() || clean.size() > MaxNameLength)
    {
        error = "the name needs 1 to 40 characters";
        return 0;
    }
    if (purpose > uint8(SpecPurpose::PvP))
    {
        error = "unknown purpose";
        return 0;
    }
    uint32 own = 0;
    for (auto const& [id, spec] : _templates)
        if (spec.owner == owner)
            ++own;
    if (own >= MaxGuildTemplates)
    {
        error = "the guild has 40 templates already";
        return 0;
    }

    BuildTalentIndex();
    uint8 const classId = player->getClass();
    std::array<uint32, 3> const perTab = PointsPerTab(player);
    if (perTab[0] + perTab[1] + perTab[2] == 0)
    {
        error = "the character has no talents";
        return 0;
    }

    // The order: tree by tree, the tree with the most points first; inside a tree row by row, and in a row a talent
    // after the one it depends on.
    std::array<uint8, 3> tabs = { 0, 1, 2 };
    std::stable_sort(tabs.begin(), tabs.end(), [&perTab](uint8 a, uint8 b) { return perTab[a] > perTab[b]; });
    SpecTemplate spec;
    for (uint8 tab : tabs)
    {
        std::vector<TalentEntry const*> talents;
        for (auto const& [talentTab, talent] : classTalents[classId])
            if (talentTab == tab && RankOf(player, talent))
                talents.push_back(talent);
        std::sort(talents.begin(), talents.end(), [](TalentEntry const* a, TalentEntry const* b)
        {
            if (a->Row != b->Row)
                return a->Row < b->Row;
            if (a->DependsOn == b->TalentID)
                return false;
            if (b->DependsOn == a->TalentID)
                return true;
            return a->Col < b->Col;
        });
        for (TalentEntry const* talent : talents)
            for (uint8 rank = RankOf(player, talent); rank > 0; --rank)
                spec.points.push_back({ tab, uint8(talent->Row), uint8(talent->Col) });
    }

    spec.id = _nextGuildId++;
    spec.owner = owner;
    spec.classId = classId;
    spec.name = clean;
    spec.purpose = purpose;
    spec.tab = tabs[0];
    std::string escaped = clean;
    CharacterDatabase.EscapeString(escaped);
    CharacterDatabase.Execute("INSERT INTO ga_guild_spec_template (id, owner_account, class, name, purpose, role, tab, points, created_at) "
        "VALUES ({}, {}, {}, '{}', {}, 0, {}, '{}', {})", spec.id, owner, uint32(classId), escaped, uint32(purpose), uint32(spec.tab),
        FormatPoints(spec.points), uint32(GameTime::GetGameTime().count()));
    uint32 const id = spec.id;
    _templates[id] = std::move(spec);
    return id;
}

bool SpecService::Delete(uint32 owner, uint32 templateId, std::string& error)
{
    SpecTemplate const* spec = Get(templateId);
    if (!spec || !spec->owner || spec->owner != owner)
    {
        error = "not a template of this guild";
        return false;
    }
    // Bots that played by it go back to the default of their class (their talents stay as they are).
    for (auto& [bot, assigned] : _assigned)
        if (assigned == templateId)
        {
            assigned = SpecDefault;
            StoreAssignment(bot, SpecDefault, false);
        }
    CharacterDatabase.Execute("DELETE FROM ga_guild_spec_template WHERE id = {}", templateId);
    _templates.erase(templateId);
    return true;
}
