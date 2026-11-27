/*
 * QuestSupport implementation. See QuestSupport.h.
 */

#include "QuestSupport.h"
#include "Formulas.h"
#include "GameTime.h"
#include "BotDriver.h"
#include "BotSettings.h"
#include "Config.h"
#include "Fishing.h"
#include "DatabaseEnv.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "QuestEvents.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include <algorithm>
#include <mutex>

using namespace GuildAdventure;

namespace
{
    // The planner's failure reason that says "it cannot be done" (FailQuest in OrderService.cpp); deaths and quest NPCs
    // that did not answer say something about the bot or the moment, not about the quest.
    constexpr char const* NoProgress = "no_progress";

    // A quest item the bot would have to use: it has a spell cast on use. Quests that complete through such an item are
    // not "scripted events" here (the planner's quest interactions deal with them).
    bool HasUseSpell(uint32 itemId)
    {
        ItemTemplate const* item = itemId ? sObjectMgr->GetItemTemplate(itemId) : nullptr;
        if (!item)
            return false;
        for (_Spell const& spell : item->Spells)
            if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                return true;
        return false;
    }

    bool HasQuestItemToUse(Quest const* quest)
    {
        if (HasUseSpell(quest->GetSrcItemId()))
            return true;
        for (uint32 item : quest->ItemDrop)
            if (HasUseSpell(item))
                return true;
        return false;
    }
}

char const* GuildAdventure::ToString(QuestVerdict verdict)
{
    switch (verdict)
    {
        case QuestVerdict::Supported:     return "supported";
        case QuestVerdict::Listed:        return "listed";
        case QuestVerdict::ScriptedEvent: return "scripted event";
        case QuestVerdict::Learned:       return "learned";
    }
    return "supported";
}

QuestSupport& QuestSupport::Instance()
{
    static QuestSupport instance;
    return instance;
}

void QuestSupport::Load()
{
    // What the planner can do about script events comes first: such a quest is not left alone.
    sQuestEvents.Load();

    uint32 const start = getMSTime();
    std::unordered_map<uint32, Entry> entries;
    std::unordered_map<uint32, std::unordered_set<uint32>> blockedBy;
    uint32 listed = 0, scripted = 0, learned = 0, played = 0;

    // (1) The hand-checked list. A row with a setting applies only while that module setting is on (a setting that
    // does not exist, because the feature is not installed, counts as off).
    uint32 inactive = 0;
    if (QueryResult result = WorldDatabase.Query("SELECT quest_id, reason, setting FROM ga_quest_block_template"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const questId = fields[0].Get<uint32>();
            std::string const setting = fields[2].Get<std::string>();
            if (!setting.empty() && !sConfigMgr->GetOption<bool>("GuildAdventure." + setting, false, false))
            {
                ++inactive;
                continue;
            }
            if (!sObjectMgr->GetQuestTemplate(questId))
            {
                LOG_ERROR("module", "mod-guild-adventure: ga_quest_block_template lists unknown quest {}, skipped", questId);
                continue;
            }
            entries[questId] = { QuestVerdict::Listed, fields[1].Get<std::string>() };
            ++listed;
        } while (result->NextRow());
    }

    // (2) Quests that complete through a script event: the "exploration or event" flag without an area trigger to walk
    // into and without a quest item to use.
    std::unordered_set<uint32> explored;
    if (QueryResult result = WorldDatabase.Query("SELECT quest FROM areatrigger_involvedrelation"))
    {
        do
            explored.insert(result->Fetch()[0].Get<uint32>());
        while (result->NextRow());
    }

    // Escorts: the quest giver walks a waypoint path once the quest is taken. Its path is either in script_waypoint
    // (C++ escort scripts) or started by its SmartAI (action 53, "waypoint start", in its own script or in one of its
    // timed action lists, whose ids are entry * 100 + n). The planner follows such an NPC (OrderService, task "escort").
    std::unordered_set<uint32> walkers;
    std::unordered_map<uint32, uint32> escorts;
    // Escorts are always looked for: whether a bot does them is its bot setting Quests.Escorts (IsLeftAlone).
    {
        if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT entry FROM script_waypoint"))
        {
            do
                walkers.insert(result->Fetch()[0].Get<uint32>());
            while (result->NextRow());
        }
        if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT entryorguid, source_type FROM smart_scripts WHERE action_type = 53 "
            "AND source_type IN (0, 9) AND entryorguid > 0"))
        {
            do
            {
                Field* fields = result->Fetch();
                int64 const id = fields[0].Get<int64>();
                walkers.insert(uint32(fields[1].Get<uint8>() == 9 ? id / 100 : id));
            } while (result->NextRow());
        }
    }
    auto escortOf = [&](uint32 questId) -> uint32
    {
        if (QuestRelations const* relations = sObjectMgr->GetCreatureQuestRelationMap())
            for (auto const& [entry, quest] : *relations)
                if (quest == questId && walkers.count(entry))
                    return entry;
        return 0;
    };
    for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
    {
        if (!quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) || explored.count(questId) || entries.count(questId))
            continue;
        if (HasQuestItemToUse(quest))
            continue;
        // A kill, object or item objective beside the event: the bots try those (for some of them the script event is
        // the kill itself); what really cannot be done ends up learned.
        bool hasObjective = false;
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT && !hasObjective; ++i)
            hasObjective = quest->RequiredNpcOrGo[i] != 0;
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT && !hasObjective; ++i)
            hasObjective = quest->RequiredItemId[i] != 0;
        if (hasObjective)
            continue;
        // An event the planner can play (QuestEvents: a talk, an event at the giver, an NPC to lead). A talk whose
        // credit was traced comes before the guess "its giver walks somewhere, so it is an escort" (J.D. Collie walks
        // for one of his quests; the three pylon quests are talks to the pylons).
        QuestEventPlan const* event = sQuestEvents.GetEventPlan(questId);
        if (event && event->kind == QuestEventKind::Talk)
        {
            ++played;
            continue;
        }
        if (uint32 const walker = walkers.empty() ? 0 : escortOf(questId))
        {
            escorts[questId] = walker;
            continue;
        }
        if (event)
        {
            ++played;
            continue;
        }
        entries[questId] = { QuestVerdict::ScriptedEvent, "completes through a script event (defence, talk, an NPC that follows)" };
        ++scripted;
    }

    // (3) What the guild bots learned the hard way.
    uint32 const needed = GetSettings().questsLearnedBlockBots;
    if (QueryResult result = CharacterDatabase.Query("SELECT quest_id, bot_guid FROM ga_bot_quest_block WHERE blocked = 1 AND reason = '{}'",
        NoProgress))
    {
        do
        {
            Field* fields = result->Fetch();
            blockedBy[fields[0].Get<uint32>()].insert(fields[1].Get<uint32>());
        } while (result->NextRow());
    }
    if (needed)
    {
        for (auto const& [questId, bots] : blockedBy)
        {
            if (bots.size() < needed || entries.count(questId))
                continue;
            entries[questId] = { QuestVerdict::Learned, Acore::StringFormat("{} guild bots made no progress on it", bots.size()) };
            ++learned;
        }
    }

    {
        std::unique_lock lock(_lock);
        _entries = std::move(entries);
        _blockedBy = std::move(blockedBy);
        _escorts = std::move(escorts);
    }

    std::size_t escortCount = 0;
    {
        std::shared_lock lock(_lock);
        escortCount = _escorts.size();
    }
    LOG_INFO("module", "mod-guild-adventure: quest support: {} listed ({} more wait for their setting), {} scripted events, "
        "{} learned quests are left alone by AI-played characters{}; {} escort quests are followed, {} event quests are played "
        "({} ms)", listed, inactive, scripted, learned, GetSettings().questsFilter ? "" : " (filter off)", escortCount, played,
        GetMSTimeDiffToNow(start));
}

QuestVerdict QuestSupport::GetVerdict(uint32 questId) const
{
    std::shared_lock lock(_lock);
    auto itr = _entries.find(questId);
    return itr != _entries.end() ? itr->second.verdict : QuestVerdict::Supported;
}

std::string QuestSupport::GetReason(uint32 questId) const
{
    std::shared_lock lock(_lock);
    auto itr = _entries.find(questId);
    return itr != _entries.end() ? itr->second.reason : std::string();
}

uint32 QuestSupport::GetEscortNpc(uint32 questId) const
{
    std::shared_lock lock(_lock);
    auto itr = _escorts.find(questId);
    return itr != _escorts.end() ? itr->second : 0;
}

void QuestSupport::NoteDropped(ObjectGuid guid, uint32 questId, uint32 untilUnix)
{
    std::unique_lock lock(_lock);
    _dropped[(uint64(guid.GetCounter()) << 32) | questId] = untilUnix;
}

bool QuestSupport::WasDropped(ObjectGuid guid, uint32 questId) const
{
    std::shared_lock lock(_lock);
    auto itr = _dropped.find((uint64(guid.GetCounter()) << 32) | questId);
    return itr != _dropped.end() && itr->second > uint32(GameTime::GetGameTime().count());
}

std::vector<std::pair<uint32, uint32>> QuestSupport::DroppedBy(ObjectGuid guid) const
{
    std::vector<std::pair<uint32, uint32>> quests;
    uint32 const now = uint32(GameTime::GetGameTime().count());
    std::shared_lock lock(_lock);
    for (auto const& [key, until] : _dropped)
        if (uint32(key >> 32) == guid.GetCounter() && until > now)
            quests.emplace_back(uint32(key & 0xFFFFFFFFu), until);
    return quests;
}

bool QuestSupport::MayTake(Player* player, Quest const* quest) const
{
    if (!quest || !GetSettings().questsFilter)
        return true;
    // Only characters the AI plays (a bot, a watched character, a planned leader): their owner takes what he likes.
    if (!BotDriver::GetAI(player))
        return true;
    // Not what the planner has just dropped to keep the quest log open.
    if (WasDropped(player->GetGUID(), quest->GetQuestId()))
        return false;
    // The last two slots of the quest log stay free. mod-playerbots' NewRpg empties a log with fewer than two free
    // slots the next time it looks at a quest giver (NewRpgBaseAction::OrganizeQuestLog): quests far below the bot and
    // quests of other zones go, finished ones included, and NewRpg takes quests in passing. (A shaman's finished quest
    // was dropped on the ship to its hand-in, after it had taken four quests in two cities on the way; it sailed back.)
    // The planner itself picks quests up only with three slots free.
    uint32 freeSlots = 0;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (!player->GetQuestSlotQuestId(slot))
            ++freeSlots;
    if (freeSlots <= 2)
        return false;
    // A quest that is grey for the character is not taken where it would be abandoned at the next look
    // (Quests.AbandonGrey): an item in a shaman's bags started "Signs of the Legion" 59 times in an hour.
    if (quest->GetQuestLevel() > 0 && uint32(quest->GetQuestLevel()) <= Acore::XP::GetGrayLevel(player->GetLevel()) &&
        sBotSettings.IsOn(player, Setting::QuestsAbandonGrey))
        return false;
    return !IsLeftAlone(player, quest->GetQuestId());
}

bool QuestSupport::IsLeftAlone(Player const* player, uint32 questId) const
{
    if (GetVerdict(questId) != QuestVerdict::Supported)
        return true;
    // Escort and event quests are known whatever the conf says; the guild leader's settings decide per character
    // (GuildAdventure.Quests.Escorts / .Events are their defaults, docs/bot-settings.md).
    // An NPC that walks its own way, or follows the character on a way the quest sets, goes past what the quest is
    // made for: such a quest waits until the character has the quest's level. (A level-17 character led the sleeper of
    // "The Sleeper Has Awakened", a level-20 quest, along Ashenvale's coast and died to a level-21 naga; nothing judges
    // that way, and the character cannot choose another.)
    auto const tooLow = [&]()
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        return quest && quest->GetQuestLevel() > 0 && int32(player->GetLevel()) < quest->GetQuestLevel();
    };
    if (GetEscortNpc(questId))
        return !sBotSettings.IsOn(player, Setting::QuestsEscorts) || tooLow();
    if (auto const plan = sQuestEvents.GetEventPlan(questId))
        return !sBotSettings.IsOn(player, Setting::QuestsEvents) || (plan->kind == QuestEventKind::Lead && tooLow());
    // A quest that asks for fish is for those who can fish: the spell and a pole ("The Family and the Fishing Pole").
    if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId); quest && Fishing::NeedsFishing(quest))
        return !Fishing::CanFish(player);
    return false;
}

void QuestSupport::OnQuestBlocked(uint32 questId, uint32 botLowGuid, std::string const& reason)
{
    if (reason != NoProgress)
        return;

    uint32 const needed = GetSettings().questsLearnedBlockBots;
    std::size_t bots = 0;
    bool learnedNow = false;
    {
        std::unique_lock lock(_lock);
        std::unordered_set<uint32>& set = _blockedBy[questId];
        set.insert(botLowGuid);
        bots = set.size();
        if (needed && bots >= needed && !_entries.count(questId))
        {
            _entries[questId] = { QuestVerdict::Learned, Acore::StringFormat("{} guild bots made no progress on it", bots) };
            learnedNow = true;
        }
    }

    if (learnedNow)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        LOG_INFO("module", "mod-guild-adventure: quest {} \"{}\" is left alone from now on: {} guild bots made no progress on it",
            questId, quest ? quest->GetTitle() : "?", bots);
    }
}

uint32 QuestSupport::ForgetLearned()
{
    uint32 rows = 0;
    for (auto& [owner, guild] : sGuildAdventureMgr.GetGuilds())
    {
        for (auto& [guid, bot] : guild.bots)
        {
            rows += uint32(bot.questBlocks.size());
            bot.questBlocks.clear();
        }
    }
    CharacterDatabase.Execute("DELETE FROM ga_bot_quest_block");

    {
        std::unique_lock lock(_lock);
        _blockedBy.clear();
        std::erase_if(_entries, [](auto const& entry) { return entry.second.verdict == QuestVerdict::Learned; });
    }
    return rows;
}

std::vector<std::string> QuestSupport::DescribeScripted(uint32 maxLevel) const
{
    std::vector<std::pair<int32, uint32>> quests; // (level, id)
    {
        std::shared_lock lock(_lock);
        for (auto const& [questId, entry] : _entries)
        {
            if (entry.verdict != QuestVerdict::ScriptedEvent)
                continue;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (quest && quest->GetZoneOrSort() > 0 && quest->GetQuestLevel() > 0 && uint32(quest->GetQuestLevel()) <= maxLevel)
                quests.emplace_back(quest->GetQuestLevel(), questId);
        }
    }
    std::sort(quests.begin(), quests.end());

    std::vector<std::string> lines;
    lines.push_back(Acore::StringFormat("{} zone quests up to level {} are left alone as script events:", quests.size(), maxLevel));
    for (auto const& [level, questId] : quests)
        lines.push_back(Acore::StringFormat("  {} (level {}) \"{}\"", questId, level, sObjectMgr->GetQuestTemplate(questId)->GetTitle()));
    return lines;
}

std::vector<std::string> QuestSupport::Describe() const
{
    std::vector<std::string> lines;
    std::shared_lock lock(_lock);

    uint32 listed = 0, scripted = 0, learned = 0;
    std::vector<uint32> learnedIds;
    for (auto const& [questId, entry] : _entries)
    {
        switch (entry.verdict)
        {
            case QuestVerdict::Listed:        ++listed; break;
            case QuestVerdict::ScriptedEvent: ++scripted; break;
            case QuestVerdict::Learned:       ++learned; learnedIds.push_back(questId); break;
            default: break;
        }
    }
    lines.push_back(Acore::StringFormat("Quests AI-played characters leave alone: {} listed (ga_quest_block_template), {} scripted "
        "events, {} learned; filter {}", listed, scripted, learned, GetSettings().questsFilter ? "on" : "off"));

    std::sort(learnedIds.begin(), learnedIds.end());
    for (uint32 questId : learnedIds)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        lines.push_back(Acore::StringFormat("  learned: {} \"{}\" ({})", questId, quest ? quest->GetTitle() : "?", _entries.at(questId).reason));
    }
    return lines;
}

// ---------------------------------------------------------------------------------------------------------------
// Scripts
// ---------------------------------------------------------------------------------------------------------------

class GuildAdventureQuestPlayerScript : public PlayerScript
{
public:
    GuildAdventureQuestPlayerScript() : PlayerScript("GuildAdventureQuestPlayerScript", {
        PLAYERHOOK_ON_CAN_TAKE_QUEST
    }) { }

    // Core patch CP-9: called at the end of Player::CanTakeQuest, on map threads too.
    bool OnPlayerCanTakeQuest(Player* player, Quest const* quest) override
    {
        return sQuestSupport.MayTake(player, quest);
    }
};

class GuildAdventureQuestWorldScript : public WorldScript
{
public:
    GuildAdventureQuestWorldScript() : WorldScript("GuildAdventureQuestWorldScript", {
        WORLDHOOK_ON_STARTUP
    }) { }

    void OnStartup() override
    {
        sQuestSupport.Load();
    }
};

void AddGuildAdventureQuestScripts()
{
    new GuildAdventureQuestPlayerScript();
    new GuildAdventureQuestWorldScript();
}
