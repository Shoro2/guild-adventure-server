/*
 * .ga admin questscan <character> [min level] [max level]: how the order planner would work every quest of a level range
 * for that character (docs/orders.md "Quest interactions"). Each objective is counted as kill, loot, one of the
 * interaction kinds or explore, or as a gap with its reason; the gaps are listed in Server.log, so the quest types that
 * are still missing can be measured instead of found one failure at a time.
 */

#include "Chat.h"
#include "CommandScript.h"
#include "GuildAdventureConfig.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "QuestInteraction.h"
#include "StringFormat.h"
#include "WorldIndex.h"
#include <map>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;
using namespace GuildAdventure;

namespace
{
    struct ScanResult
    {
        std::map<std::string, uint32> counts;              // what the planner does, per objective
        std::map<std::string, std::vector<uint32>> gaps;   // reason -> quests
        std::map<std::string, std::vector<uint32>> plans;  // interaction kind -> quests (to pick tests)
    };

    void Planned(ScanResult& result, InteractionKind kind, uint32 questId)
    {
        std::string const key = std::string("interact: ") + ToString(kind);
        ++result.counts[key];
        std::vector<uint32>& quests = result.plans[key];
        if (quests.empty() || quests.back() != questId)
            quests.push_back(questId);
    }

    void Gap(ScanResult& result, std::string const& reason, uint32 questId)
    {
        ++result.counts["gap: " + reason];
        std::vector<uint32>& quests = result.gaps[reason];
        if (quests.empty() || quests.back() != questId)
            quests.push_back(questId);
    }

    // Creatures whose kill credits another entry (creature_template KillCredit1/2): the planner only looks for spawns of
    // the objective's own entry.
    std::unordered_map<uint32, uint32> const& KillCreditSources()
    {
        static std::unordered_map<uint32, uint32> sources;
        if (sources.empty())
            for (auto const& [entry, info] : *sObjectMgr->GetCreatureTemplates())
                for (uint32 credit : info.KillCredit)
                    if (credit)
                        sources.emplace(credit, entry);
        return sources;
    }

    // Quests an item starts (item_template.startquest).
    std::unordered_map<uint32, uint32> const& ItemStarters()
    {
        static std::unordered_map<uint32, uint32> starters;
        if (starters.empty())
            for (auto const& [entry, info] : *sObjectMgr->GetItemTemplateStore())
                if (info.StartQuest)
                    starters.emplace(info.StartQuest, entry);
        return starters;
    }

    // The character's side can get the quest: a quest giver friendly to it (or a game object), or an item starts it.
    // Quests without any (unused, removed) do not count.
    bool OfferedTo(Player* player, uint32 questId)
    {
        if (ItemStarters().count(questId))
            return true;
        for (SpawnPoint const& starter : sWorldIndex.GetQuestStarters(questId))
        {
            if (starter.gameObject)
                return true;
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(starter.entry);
            if (info && WorldIndex::IsFriendlyFaction(info->faction, player))
                return true;
        }
        return false;
    }

    void ScanQuest(Player* player, Quest const* quest, ScanResult& result)
    {
        uint32 const questId = quest->GetQuestId();
        bool anyObjective = false;

        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            int32 const target = quest->RequiredNpcOrGo[i];
            if (!target || !quest->RequiredNpcOrGoCount[i])
                continue;
            anyObjective = true;
            if (InteractionPlan const* plan = QuestInteraction::Resolve(player, quest, i))
            {
                Planned(result, plan->kind, questId);
                continue;
            }
            if (target < 0)
            {
                Gap(result, "unknown game object", questId);
                continue;
            }
            uint32 const entry = uint32(target);
            if (!QuestInteraction::CanKill(player, entry))
            {
                Gap(result, "friendly creature without script or item (talk/gossip?)", questId);
                continue;
            }
            if (!sWorldIndex.GetCreatureSpawns(entry).empty())
                ++result.counts["kill"];
            else if (KillCreditSources().count(entry))
                Gap(result, "kill credit from other creatures (KillCredit)", questId);
            else
                Gap(result, "creature without open-world spawn (summoned, event, instance)", questId);
        }

        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            uint32 const item = quest->RequiredItemId[i];
            if (!item || !quest->RequiredItemCount[i])
                continue;
            anyObjective = true;
            if (InteractionPlan const* plan = QuestInteraction::Resolve(player, quest, QUEST_OBJECTIVES_COUNT + i))
            {
                Planned(result, plan->kind, questId);
                continue;
            }
            if (!sWorldIndex.GetItemSources(item).empty())
                ++result.counts["loot"];
            else if (item == quest->GetSrcItemId())
                ++result.counts["given with the quest"];
            else
                Gap(result, "item without a loot source (vendor, reference loot, spell, gossip)", questId);
        }

        if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT))
        {
            anyObjective = true;
            if (QuestInteraction::ResolveExplore(player, quest))
                Planned(result, InteractionKind::Explore, questId);
            else
                Gap(result, "event completed by a script (escort, gossip, spell)", questId);
        }

        if (!anyObjective)
            ++result.counts["no objective (deliver / talk)"];
    }
}

class GuildAdventureQuestScanCommand : public CommandScript
{
public:
    GuildAdventureQuestScanCommand() : CommandScript("GuildAdventureQuestScanCommand") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable adminTable =
        {
            { "questscan", HandleQuestScan, SEC_ADMINISTRATOR, Console::Yes }
        };
        static ChatCommandTable gaTable =
        {
            { "admin", adminTable }
        };
        static ChatCommandTable commandTable =
        {
            { "ga", gaTable }
        };
        return commandTable;
    }

    static bool HandleQuestScan(ChatHandler* handler, PlayerIdentifier target, Optional<uint8> minLevel, Optional<uint8> maxLevel)
    {
        if (!IsModuleEnabled())
        {
            handler->SendErrorMessage("Guild Adventure is disabled (GuildAdventure.Enable = 0).", false);
            return false;
        }
        Player* player = target.GetConnectedPlayer();
        if (!player)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} must be in the world (its faction, race and class decide which "
                "quests count).", target.GetName()), false);
            return false;
        }
        int32 const low = minLevel ? int32(*minLevel) : 1;
        int32 const high = maxLevel ? int32(*maxLevel) : 60;

        ScanResult result;
        uint32 quests = 0;
        for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
        {
            int32 const level = quest->GetQuestLevel() > 0 ? quest->GetQuestLevel() : int32(quest->GetMinLevel());
            if (level < low || level > high)
                continue;
            if (quest->GetAllowableRaces() && !(quest->GetAllowableRaces() & player->getRaceMask()))
                continue;
            if (quest->GetRequiredClasses() && !(quest->GetRequiredClasses() & player->getClassMask()))
                continue;
            if (quest->GetType() != 0 || quest->IsRepeatable() || quest->IsSeasonal() || quest->IsDailyOrWeekly())
                continue; // elite, dungeon, raid and repeatable quests are not planned
            if (!OfferedTo(player, questId))
                continue; // the other side's quests, unused quests
            ++quests;
            ScanQuest(player, quest, result);
        }

        handler->PSendSysMessage(Acore::StringFormat("Quest scan for {} (levels {}-{}): {} quests.", player->GetName(), low, high,
            quests));
        for (auto const& [what, count] : result.counts)
            handler->PSendSysMessage(Acore::StringFormat("  {}: {}", what, count));
        auto logList = [&](char const* what, std::string const& key, std::vector<uint32> const& list)
        {
            std::string ids;
            for (uint32 questId : list)
            {
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                ids += Acore::StringFormat("{}{} \"{}\"", ids.empty() ? "" : ", ", questId, quest ? quest->GetTitle() : "?");
            }
            LOG_INFO("module", "mod-guild-adventure: questscan {} {}-{}: {} {} ({} quests): {}", player->GetName(), low, high,
                what, key, list.size(), ids);
        };
        for (auto const& [kind, list] : result.plans)
            logList("planned", kind, list);
        for (auto const& [reason, list] : result.gaps)
            logList("gap", reason, list);
        handler->PSendSysMessage("The quests of each kind and gap are listed in Server.log (\"questscan\").");
        return true;
    }
};

void AddGuildAdventureQuestScanScripts()
{
    new GuildAdventureQuestScanCommand();
}
