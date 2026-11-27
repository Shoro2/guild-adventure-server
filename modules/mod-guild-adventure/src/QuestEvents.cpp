/*
 * QuestEvents implementation. See QuestEvents.h.
 */

#include "QuestEvents.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "GuildAdventureSettings.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "SmartScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <tuple>

using namespace GuildAdventure;

namespace
{
    std::vector<QuestEventPlan> const NoPlans;

    // One smart_scripts row, as far as the trace needs it.
    struct ScriptRow
    {
        int64 owner = 0;        // entry, or -guid, or the id of a timed action list
        uint8 source = 0;       // SMART_SCRIPT_TYPE_*
        uint32 id = 0;
        uint32 link = 0;
        uint32 event = 0;
        uint32 eventParam[2] = {};
        uint32 action = 0;
        uint32 param[6] = {};
        uint32 targetType = 0;
        uint32 targetParam[2] = {};
    };

    // An event a player causes that leads to a credit.
    struct Root
    {
        uint32 entry = 0;       // creature or game object entry, or area trigger id
        bool gameObject = false;
        bool areaTrigger = false;
        uint32 event = 0;
        uint32 param[2] = {};

        bool operator<(Root const& other) const
        {
            return std::tie(entry, gameObject, areaTrigger, event, param[0], param[1]) <
                std::tie(other.entry, other.gameObject, other.areaTrigger, other.event, other.param[0], other.param[1]);
        }
    };

    // Whether casting `spell` puts the aura `aura` on its target: the spell itself, or one of its effects triggers it.
    bool CastGives(uint32 spell, uint32 aura)
    {
        if (spell == aura)
            return true;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        if (!info)
            return false;
        for (SpellEffectInfo const& effect : info->Effects)
            if (effect.TriggerSpell == aura)
                return true;
        return false;
    }

    class ScriptTrace
    {
    public:
        void Load()
        {
            QueryResult result = WorldDatabase.Query("SELECT entryorguid, source_type, id, link, event_type, event_param1, event_param2, "
                "action_type, action_param1, action_param2, action_param3, action_param4, action_param5, action_param6, "
                "target_type, target_param1, target_param2 FROM smart_scripts WHERE source_type IN (0, 1, 2, 9)");
            if (!result)
                return;
            do
            {
                Field* fields = result->Fetch();
                ScriptRow row;
                row.owner = fields[0].Get<int32>();
                row.source = fields[1].Get<uint8>();
                row.id = fields[2].Get<uint16>();
                row.link = fields[3].Get<uint16>();
                row.event = fields[4].Get<uint8>();
                row.eventParam[0] = fields[5].Get<uint32>();
                row.eventParam[1] = fields[6].Get<uint32>();
                row.action = fields[7].Get<uint8>();
                for (uint8 i = 0; i < 6; ++i)
                    row.param[i] = fields[8 + i].Get<uint32>();
                row.targetType = fields[14].Get<uint8>();
                row.targetParam[0] = fields[15].Get<uint32>();
                row.targetParam[1] = fields[16].Get<uint32>();
                rows.push_back(row);
            } while (result->NextRow());

            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                ScriptRow const& row = rows[i];
                _byScript[{ row.owner, row.source }].push_back(i);
                switch (row.action)
                {
                    case SMART_ACTION_CALL_TIMED_ACTIONLIST:
                        _listCallers[row.param[0]].push_back(i);
                        break;
                    case SMART_ACTION_CALL_RANDOM_TIMED_ACTIONLIST:
                        for (uint32 list : row.param)
                            if (list)
                                _listCallers[list].push_back(i);
                        break;
                    case SMART_ACTION_CALL_RANDOM_RANGE_TIMED_ACTIONLIST:
                        if (row.param[1] >= row.param[0] && row.param[1] - row.param[0] < 20)
                            for (uint32 list = row.param[0]; list <= row.param[1]; ++list)
                                _listCallers[list].push_back(i);
                        break;
                    case SMART_ACTION_SET_DATA:
                        // On which creature: by entry (closest, in range, at a distance) or by spawn.
                        if ((row.targetType == SMART_TARGET_CLOSEST_CREATURE || row.targetType == SMART_TARGET_CREATURE_DISTANCE ||
                            row.targetType == SMART_TARGET_CREATURE_RANGE) && row.targetParam[0])
                            _dataSetters[row.targetParam[0]].push_back(i);
                        else if (row.targetType == SMART_TARGET_CREATURE_GUID)
                        {
                            uint32 entry = row.targetParam[1];
                            if (!entry)
                                if (CreatureData const* data = sObjectMgr->GetCreatureData(row.targetParam[0]))
                                    entry = data->id;
                            if (entry)
                                _dataSetters[entry].push_back(i);
                        }
                        break;
                    case SMART_ACTION_SUMMON_CREATURE:
                        _summoners[row.param[0]].push_back(i);
                        break;
                    default:
                        break;
                }
            }
        }

        // The events that lead to a row: through links (the row whose link names it) and through the callers of the
        // timed action list it sits in.
        void Roots(std::size_t index, std::set<Root>& out) const
        {
            std::set<std::size_t> seen;
            Walk(index, out, seen, 0);
        }

        [[nodiscard]] std::vector<std::size_t> const* Script(int64 owner, uint8 source) const
        {
            auto itr = _byScript.find({ owner, source });
            return itr != _byScript.end() ? &itr->second : nullptr;
        }

        std::vector<ScriptRow> rows;

    private:
        void Walk(std::size_t index, std::set<Root>& out, std::set<std::size_t>& seen, uint32 depth) const
        {
            if (depth > 16 || !seen.insert(index).second)
                return;
            ScriptRow const& row = rows[index];
            if (row.source == SMART_SCRIPT_TYPE_TIMED_ACTIONLIST)
            {
                auto callers = _listCallers.find(uint32(row.owner));
                if (callers != _listCallers.end())
                    for (std::size_t caller : callers->second)
                        Walk(caller, out, seen, depth + 1);
                return;
            }
            if (row.event == SMART_EVENT_LINK)
            {
                if (std::vector<std::size_t> const* script = Script(row.owner, row.source))
                    for (std::size_t other : *script)
                        if (other != index && rows[other].link == row.id)
                            Walk(other, out, seen, depth + 1);
                return;
            }

            // A creature's script: "data set" goes back to whoever sets that data on it, and what happens to a summoned
            // creature (its death, its arrival) to whoever summons it.
            if (row.source == SMART_SCRIPT_TYPE_CREATURE)
            {
                uint32 entry = row.owner > 0 ? uint32(row.owner) : 0;
                if (!entry)
                    if (CreatureData const* data = sObjectMgr->GetCreatureData(ObjectGuid::LowType(-row.owner)))
                        entry = data->id;
                std::size_t const before = out.size();
                if (row.event == SMART_EVENT_DATA_SET)
                {
                    auto setters = _dataSetters.find(entry);
                    if (setters != _dataSetters.end())
                        for (std::size_t setter : setters->second)
                            if (rows[setter].param[0] == row.eventParam[0] &&
                                (rows[setter].param[1] == row.eventParam[1] || !row.eventParam[1]))
                                Walk(setter, out, seen, depth + 1);
                }
                else if (row.event == SMART_EVENT_DEATH || row.event == SMART_EVENT_JUST_SUMMONED)
                {
                    auto summoners = _summoners.find(entry);
                    if (summoners != _summoners.end())
                        for (std::size_t summoner : summoners->second)
                            Walk(summoner, out, seen, depth + 1);
                }
                // What the script itself brings about: "text over" goes back to the row that says that text, "has aura"
                // to the row that casts it (a Theramore Guard is handed a leaflet through a gossip choice, says a line,
                // casts the leaflet's spell on itself when the line is over, and gives the credit once it has the aura:
                // "Discrediting the Deserters" had no plan, its guards were an objective to kill, and a paladin stood
                // beside them for three times three minutes).
                else if (row.event == SMART_EVENT_TEXT_OVER || row.event == SMART_EVENT_HAS_AURA)
                {
                    if (std::vector<std::size_t> const* script = Script(row.owner, row.source))
                    {
                        std::vector<std::size_t> casts;
                        for (std::size_t other : *script)
                        {
                            if (other == index)
                                continue;
                            ScriptRow const& from = rows[other];
                            if (row.event == SMART_EVENT_TEXT_OVER)
                            {
                                if (from.action == SMART_ACTION_TALK && from.param[0] == row.eventParam[0] &&
                                    (!row.eventParam[1] || row.eventParam[1] == entry))
                                    Walk(other, out, seen, depth + 1);
                            }
                            else if (from.action == SMART_ACTION_CAST || from.action == SMART_ACTION_SELF_CAST ||
                                from.action == SMART_ACTION_INVOKER_CAST || from.action == SMART_ACTION_CROSS_CAST)
                                casts.push_back(other);
                        }
                        // The cast of that aura: the spell itself or one that triggers it, else the script's only cast.
                        std::vector<std::size_t> fitting;
                        for (std::size_t cast : casts)
                            if (CastGives(rows[cast].param[0], row.eventParam[0]))
                                fitting.push_back(cast);
                        if (fitting.empty() && casts.size() == 1)
                            fitting = casts;
                        for (std::size_t cast : fitting)
                            Walk(cast, out, seen, depth + 1);
                    }
                }
                if (out.size() != before)
                    return;
            }

            Root root;
            root.gameObject = row.source == SMART_SCRIPT_TYPE_GAMEOBJECT;
            root.areaTrigger = row.source == SMART_SCRIPT_TYPE_AREATRIGGER;
            if (row.owner > 0)
                root.entry = uint32(row.owner);
            else if (root.gameObject)
            {
                if (GameObjectData const* data = sObjectMgr->GetGameObjectData(ObjectGuid::LowType(-row.owner)))
                    root.entry = data->id;
            }
            else if (CreatureData const* data = sObjectMgr->GetCreatureData(ObjectGuid::LowType(-row.owner)))
                root.entry = data->id;
            if (!root.entry)
                return;
            root.event = row.event;
            root.param[0] = row.eventParam[0];
            root.param[1] = row.eventParam[1];
            out.insert(root);
        }

        std::map<std::pair<int64, uint8>, std::vector<std::size_t>> _byScript;
        std::unordered_map<uint32, std::vector<std::size_t>> _listCallers;
        std::unordered_map<uint32, std::vector<std::size_t>> _dataSetters; // creature entry -> rows that set data on it
        std::unordered_map<uint32, std::vector<std::size_t>> _summoners;   // creature entry -> rows that summon it
    };

    // The gossip menu a creature or game object opens first.
    uint32 FirstMenu(uint32 entry, bool gameObject)
    {
        if (gameObject)
        {
            GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(entry);
            return info ? info->GetGossipMenuId() : 0;
        }
        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(entry);
        return info ? info->GossipMenuId : 0;
    }

    bool HasOption(uint32 menuId, uint32 optionId)
    {
        GossipMenuItemsMapBounds bounds = sObjectMgr->GetGossipMenuItemsMapBounds(menuId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
            if (itr->second.OptionID == optionId)
                return true;
        return false;
    }

    std::string EntryName(uint32 entry, bool gameObject)
    {
        if (gameObject)
        {
            GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(entry);
            return info ? info->name : Acore::StringFormat("game object {}", entry);
        }
        CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(entry);
        return info ? info->Name : Acore::StringFormat("creature {}", entry);
    }
}

char const* GuildAdventure::ToString(QuestEventKind kind)
{
    switch (kind)
    {
        case QuestEventKind::None: return "none";
        case QuestEventKind::Talk: return "talk";
        case QuestEventKind::Stay: return "stay";
        case QuestEventKind::Lead: return "lead";
        case QuestEventKind::Enter: return "enter";
        case QuestEventKind::Clear: return "clear";
    }
    return "none";
}

QuestEvents& QuestEvents::Instance()
{
    static QuestEvents instance;
    return instance;
}

bool QuestEvents::Reaches(uint32 from, uint32 to) const
{
    if (from == to)
        return true;
    std::unordered_set<uint32> seen{ from };
    std::deque<uint32> todo{ from };
    while (!todo.empty() && seen.size() < 200)
    {
        uint32 const menu = todo.front();
        todo.pop_front();
        GossipMenuItemsMapBounds bounds = sObjectMgr->GetGossipMenuItemsMapBounds(menu);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            uint32 const next = itr->second.ActionMenuID;
            if (!next || !seen.insert(next).second)
                continue;
            if (next == to)
                return true;
            todo.push_back(next);
        }
    }
    return false;
}

void QuestEvents::Load()
{
    uint32 const start = getMSTime();
    _plans.clear();
    _talks = _stays = _leads = _enters = _clears = 0;
    // Always loaded: whether a bot plays them is its bot setting Quests.Events (QuestSupport::IsLeftAlone; the conf key
    // GuildAdventure.Quests.Events is the default of that setting).

    ScriptTrace trace;
    trace.Load();

    // What has a spawn (a plan needs someone to walk to).
    std::unordered_set<uint32> spawnedCreatures, spawnedObjects;
    for (auto const& [guid, data] : sObjectMgr->GetAllCreatureData())
        spawnedCreatures.insert(data.id);
    for (auto const& [guid, data] : sObjectMgr->GetAllGOData())
        spawnedObjects.insert(data.id);

    // Quests with an area trigger are exploration quests, not events at their giver.
    std::unordered_set<uint32> explored;
    if (QueryResult result = WorldDatabase.Query("SELECT quest FROM areatrigger_involvedrelation"))
    {
        do
            explored.insert(result->Fetch()[0].Get<uint32>());
        while (result->NextRow());
    }

    auto talkPlan = [&](Root const& root, int32 objective, QuestEventPlan& plan) -> bool
    {
        if (root.areaTrigger || (root.event != SMART_EVENT_GOSSIP_SELECT && root.event != SMART_EVENT_GOSSIP_HELLO))
            return false;
        if (!(root.gameObject ? spawnedObjects : spawnedCreatures).count(root.entry))
            return false;
        plan.kind = QuestEventKind::Talk;
        plan.objective = objective;
        plan.entry = root.entry;
        plan.gameObject = root.gameObject;
        plan.hello = root.event == SMART_EVENT_GOSSIP_HELLO;
        if (!plan.hello)
        {
            plan.menuId = root.param[0];
            plan.optionId = root.param[1];
            // The choice must exist and be reachable from the first menu.
            if (!HasOption(plan.menuId, plan.optionId) || !Reaches(FirstMenu(root.entry, root.gameObject), plan.menuId))
                return false;
        }
        return true;
    };
    auto hasPlan = [&](uint32 questId, int32 objective)
    {
        auto itr = _plans.find(questId);
        if (itr == _plans.end())
            return false;
        return std::any_of(itr->second.begin(), itr->second.end(), [&](QuestEventPlan const& plan) { return plan.objective == objective; });
    };

    // (1) Lead: "follow" with an end creature and an event credit. Before the talks and stays: the quest giver's
    // "accepted quest" event is what starts the follow.
    for (ScriptRow const& row : trace.rows)
    {
        if (row.action != SMART_ACTION_FOLLOW || !row.param[2] || !row.param[3] || row.param[4] != 1)
            continue;
        uint32 const questId = row.param[3];
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        uint32 const follower = row.source == SMART_SCRIPT_TYPE_TIMED_ACTIONLIST ? uint32(row.owner / 100) : uint32(std::max<int64>(row.owner, 0));
        if (!quest || !quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) || hasPlan(questId, -1) || !follower ||
            !spawnedCreatures.count(follower) || !spawnedCreatures.count(row.param[2]))
            continue;
        QuestEventPlan plan;
        plan.kind = QuestEventKind::Lead;
        plan.entry = follower;
        plan.endEntry = row.param[2];
        _plans[questId].push_back(plan);
    }

    // (2) Talk: a gossip event at the root of a credit.
    std::unordered_map<uint32, std::vector<uint32>> creditQuests; // credit creature -> quests with it as an objective
    for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredNpcOrGo[i] > 0 && quest->RequiredNpcOrGoCount[i])
                creditQuests[uint32(quest->RequiredNpcOrGo[i])].push_back(questId);

    std::map<uint32, uint32> enterRoots; // quest -> area trigger
    for (std::size_t i = 0; i < trace.rows.size(); ++i)
    {
        ScriptRow const& row = trace.rows[i];
        bool const eventCredit = row.action == SMART_ACTION_CALL_AREAEXPLOREDOREVENTHAPPENS ||
            row.action == SMART_ACTION_CALL_GROUPEVENTHAPPENS;
        if (!eventCredit && row.action != SMART_ACTION_CALL_KILLEDMONSTER)
            continue;
        std::set<Root> roots;
        trace.Roots(i, roots);
        for (Root const& root : roots)
        {
            QuestEventPlan plan;
            if (eventCredit)
            {
                Quest const* quest = sObjectMgr->GetQuestTemplate(row.param[0]);
                if (!quest || !quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) || hasPlan(row.param[0], -1))
                    continue;
                if (talkPlan(root, -1, plan))
                    _plans[row.param[0]].push_back(plan);
                else if (root.areaTrigger && root.event == SMART_EVENT_AREATRIGGER_ONTRIGGER)
                    enterRoots[row.param[0]] = root.entry; // only if no talk turns up for the quest
                continue;
            }
            auto quests = creditQuests.find(row.param[0]);
            if (quests == creditQuests.end())
                continue;
            for (uint32 questId : quests->second)
            {
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                for (uint8 objective = 0; objective < QUEST_OBJECTIVES_COUNT; ++objective)
                    if (quest->RequiredNpcOrGo[objective] == int32(row.param[0]) && !hasPlan(questId, objective) &&
                        talkPlan(root, objective, plan))
                        _plans[questId].push_back(plan);
            }
        }
    }

    // (2b) Enter: an area trigger's script at the root, for a quest without a talk. Not the quests the core itself
    // completes at an area trigger (exploration).
    for (auto const& [questId, triggerId] : enterRoots)
    {
        if (hasPlan(questId, -1) || explored.count(questId) || !sObjectMgr->GetAreaTrigger(triggerId))
            continue;
        QuestEventPlan plan;
        plan.kind = QuestEventKind::Enter;
        plan.entry = triggerId;
        _plans[questId].push_back(plan);
    }

    // (3) Stay: the quest giver's own script reacts to the quest being taken, and the quest has nothing else to do.
    if (QuestRelations const* relations = sObjectMgr->GetCreatureQuestRelationMap())
    {
        for (auto const& [giver, questId] : *relations)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest || !quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) || explored.count(questId) ||
                hasPlan(questId, -1) || !spawnedCreatures.count(giver))
                continue;
            bool other = false;
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT && !other; ++i)
                other = quest->RequiredNpcOrGo[i] != 0;
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT && !other; ++i)
                other = quest->RequiredItemId[i] != 0;
            if (other)
                continue;
            std::vector<std::size_t> const* script = trace.Script(giver, SMART_SCRIPT_TYPE_CREATURE);
            if (!script)
                continue;
            bool reacts = false;
            for (std::size_t index : *script)
            {
                ScriptRow const& row = trace.rows[index];
                if (row.event == SMART_EVENT_ACCEPTED_QUEST && (row.eventParam[0] == questId || !row.eventParam[0]))
                {
                    reacts = true;
                    break;
                }
            }
            if (!reacts)
                continue;
            QuestEventPlan plan;
            plan.kind = QuestEventKind::Stay;
            plan.entry = giver;
            _plans[questId].push_back(plan);
        }
    }

    // (4) The hand-checked list (C++ scripts). A row replaces what the trace found for the quest's event.
    if (QueryResult result = WorldDatabase.Query("SELECT quest_id, kind, npc_entry, end_entry FROM ga_quest_event_template"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const questId = fields[0].Get<uint32>();
            std::string const kind = fields[1].Get<std::string>();
            QuestEventPlan plan;
            plan.kind = kind == "stay" ? QuestEventKind::Stay : kind == "lead" ? QuestEventKind::Lead :
                kind == "enter" ? QuestEventKind::Enter : kind == "clear" ? QuestEventKind::Clear : QuestEventKind::None;
            plan.entry = fields[2].Get<uint32>();
            plan.endEntry = fields[3].Get<uint32>();
            // A place to clear stands for the objective that asks for its creature.
            if (Quest const* quest = plan.kind == QuestEventKind::Clear ? sObjectMgr->GetQuestTemplate(questId) : nullptr)
                for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                    if (quest->RequiredNpcOrGo[i] == int32(plan.entry))
                        plan.objective = i;
            bool const known = plan.kind == QuestEventKind::None ? true : // "leave it alone" needs nobody to stand anywhere
                plan.kind == QuestEventKind::Enter ? sObjectMgr->GetAreaTrigger(plan.entry) != nullptr : spawnedCreatures.count(plan.entry) != 0;
            if (!sObjectMgr->GetQuestTemplate(questId) || !known ||
                (plan.kind == QuestEventKind::Lead && !spawnedCreatures.count(plan.endEntry)))
            {
                LOG_ERROR("module", "mod-guild-adventure: ga_quest_event_template: quest {} or its creatures are unknown, skipped", questId);
                continue;
            }
            std::vector<QuestEventPlan>& plans = _plans[questId];
            std::erase_if(plans, [&plan](QuestEventPlan const& old) { return old.objective == -1 || old.objective == plan.objective; });
            if (plan.kind != QuestEventKind::None) // "none": the quest is left alone although the trace found something
                plans.push_back(plan);
            if (plans.empty())
                _plans.erase(questId);
        } while (result->NextRow());
    }

    for (auto const& [questId, plans] : _plans)
        for (QuestEventPlan const& plan : plans)
            switch (plan.kind)
            {
                case QuestEventKind::Talk: ++_talks; break;
                case QuestEventKind::Stay: ++_stays; break;
                case QuestEventKind::Lead: ++_leads; break;
                case QuestEventKind::Enter: ++_enters; break;
                case QuestEventKind::Clear: ++_clears; break;
                default: break;
            }

    LOG_INFO("module", "mod-guild-adventure: quest events: {} talks, {} events at the quest giver, {} NPCs to lead, {} places to "
        "enter, {} to clear, from {} script rows ({} ms)", _talks, _stays, _leads, _enters, _clears, trace.rows.size(),
        GetMSTimeDiffToNow(start));
}

std::vector<QuestEventPlan> const& QuestEvents::GetPlans(uint32 questId) const
{
    auto itr = _plans.find(questId);
    return itr != _plans.end() ? itr->second : NoPlans;
}

QuestEventPlan const* QuestEvents::GetEventPlan(uint32 questId) const
{
    for (QuestEventPlan const& plan : GetPlans(questId))
        if (plan.objective == -1)
            return &plan;
    return nullptr;
}

TalkResult QuestEvents::Talk(Player* player, WorldObject* target, QuestEventPlan const& plan, std::string& detail) const
{
    WorldSession* session = player->GetSession();
    ObjectGuid const guid = target->GetGUID();
    PlayerMenu* talk = player->PlayerTalkClass;
    talk->ClearMenus();

    // The greeting: the client's gossip hello, or the use of a game object (which opens its menu).
    if (target->IsGameObject())
    {
        WorldPacket packet(CMSG_GAMEOBJ_USE, 8);
        packet << guid;
        session->HandleGameObjectUseOpcode(packet);
    }
    else
    {
        WorldPacket packet(CMSG_GOSSIP_HELLO, 8);
        packet << guid;
        session->HandleGossipHelloOpcode(packet);
    }
    if (plan.hello)
    {
        detail = "greeted it";
        talk->SendCloseGossip();
        return TalkResult::Chosen;
    }

    std::string path;
    for (uint32 step = 0; step < 16; ++step)
    {
        GossipMenu const& menu = talk->GetGossipMenu();
        if (menu.GetSenderGUID() != guid || menu.Empty())
        {
            detail = step ? Acore::StringFormat("the menu closed after {}", path) : "it opened no menu";
            return step ? TalkResult::NoWay : TalkResult::NoMenu;
        }
        uint32 const menuId = menu.GetMenuId();
        int32 choice = -1;
        bool last = false;
        if (menuId == plan.menuId && menu.GetItem(plan.optionId))
        {
            choice = int32(plan.optionId);
            last = true;
        }
        else
        {
            // An option of this menu whose action menu leads to the choice.
            for (auto const& [optionId, item] : menu.GetMenuItems())
            {
                GossipMenuItemData const* data = menu.GetItemData(optionId);
                if (data && data->GossipActionMenuId && Reaches(data->GossipActionMenuId, plan.menuId))
                {
                    choice = int32(optionId);
                    break;
                }
            }
        }
        if (choice < 0)
        {
            detail = Acore::StringFormat("menu {} has no option that leads to {}/{}{}", menuId, plan.menuId, plan.optionId,
                path.empty() ? "" : " (after " + path + ")");
            talk->SendCloseGossip();
            return TalkResult::NoWay;
        }

        path += Acore::StringFormat("{}{}/{}", path.empty() ? "" : ", ", menuId, choice);
        WorldPacket packet(CMSG_GOSSIP_SELECT_OPTION, 16);
        packet << guid << menuId << uint32(choice);
        session->HandleGossipSelectOptionOpcode(packet);
        if (last)
        {
            detail = Acore::StringFormat("chose {}", path);
            talk->SendCloseGossip();
            return TalkResult::Chosen;
        }
    }

    detail = Acore::StringFormat("too many menus ({})", path);
    talk->SendCloseGossip();
    return TalkResult::NoWay;
}

std::vector<std::string> QuestEvents::Describe(uint32 questId) const
{
    std::vector<std::string> lines;
    if (!questId)
    {
        lines.push_back(Acore::StringFormat("Quest events the planner plays: {} talks, {} events at the quest giver, {} NPCs to lead, "
            "{} places to enter, {} to clear ({} quests){}", _talks, _stays, _leads, _enters, _clears, _plans.size(),
            GetSettings().questsEvents ? "" : "; off by default (bot setting Quests.Events)"));
        return lines;
    }

    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    std::vector<QuestEventPlan> const& plans = GetPlans(questId);
    lines.push_back(Acore::StringFormat("Quest {} \"{}\": {} event plan{}", questId, quest ? quest->GetTitle() : "?", plans.size(),
        plans.size() == 1 ? "" : "s"));
    for (QuestEventPlan const& plan : plans)
    {
        std::string const what = plan.objective < 0 ? "the quest's event" : Acore::StringFormat("objective {}", plan.objective);
        switch (plan.kind)
        {
            case QuestEventKind::Talk:
                lines.push_back(plan.hello ?
                    Acore::StringFormat("  talk for {}: greet {} ({}{})", what, EntryName(plan.entry, plan.gameObject),
                        plan.gameObject ? "game object " : "", plan.entry) :
                    Acore::StringFormat("  talk for {}: {} ({}{}), menu {} option {} (first menu {})", what,
                        EntryName(plan.entry, plan.gameObject), plan.gameObject ? "game object " : "", plan.entry, plan.menuId,
                        plan.optionId, FirstMenu(plan.entry, plan.gameObject)));
                break;
            case QuestEventKind::Stay:
                lines.push_back(Acore::StringFormat("  stay with {} ({}) until its event is over", EntryName(plan.entry, false), plan.entry));
                break;
            case QuestEventKind::Lead:
                lines.push_back(Acore::StringFormat("  lead {} ({}) to {} ({})", EntryName(plan.entry, false), plan.entry,
                    EntryName(plan.endEntry, false), plan.endEntry));
                break;
            case QuestEventKind::Enter:
            {
                AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(plan.entry);
                lines.push_back(trigger ?
                    Acore::StringFormat("  enter area trigger {} at ({:.0f}, {:.0f}, {:.0f}) map {}, stay and fight", plan.entry,
                        trigger->x, trigger->y, trigger->z, trigger->map) :
                    Acore::StringFormat("  enter area trigger {} (unknown)", plan.entry));
                break;
            }
            case QuestEventKind::Clear:
                lines.push_back(Acore::StringFormat("  for {}: clear the place around the spawn of {} ({}), stay and fight what comes", what,
                    EntryName(plan.entry, false), plan.entry));
                break;
            default:
                break;
        }
    }
    return lines;
}
