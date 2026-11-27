/*
 * Hooks and commands of the tiered guild achievements (MilestoneService).
 *
 * The kill and quest hooks fire on map threads and only queue; the world script drains the queue and scans the guild
 * characters in WorldScript::OnUpdate on the world thread.
 *
 * Hooks used:
 *   PlayerScript  OnPlayerCreatureKillCredit (kills per creature type; the last boss of a dungeon or raid = a run),
 *                 OnPlayerCompleteQuest
 *   WorldScript   OnStartup, OnUpdate, OnShutdown
 *
 * Commands (they join the ".ga" tree of the other command scripts):
 *   .ga milestones [character]                         the guild's tiers and what it counted (GM; console with a character)
 *   .ga admin milestone add <character> <key> <n>      adds n to a sum (kills_7, quests, dungeons, raids) or raises a
 *                                                      maximum (level_class_1, level_race_3, skill_186) to n, then
 *                                                      awards what it reaches (administrator, also console / SOAP)
 *   .ga admin milestone scan <character>               reads the guild's characters in the world now (no wait for the
 *                                                      next scan)
 */

#include "CharacterCache.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Creature.h"
#include "GuildAdventureConfig.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "Map.h"
#include "MilestoneService.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"

using namespace Acore::ChatCommands;
using namespace GuildAdventure;

namespace
{
    // The run of a dungeon or raid ends with the encounter the core marks as its last (instance_encounters'
    // lastEncounterDungeon, the LFG dungeon it completes).
    void CheckRunEnd(Player* player, Creature* killed)
    {
        Map* map = killed->GetMap();
        if (!map || !map->IsDungeon())
            return;
        DungeonEncounterList const* encounters = sObjectMgr->GetDungeonEncounterList(map->GetId(), map->GetDifficulty());
        if (!encounters)
            return;
        for (DungeonEncounter const* encounter : *encounters)
            if (encounter->lastEncounterDungeon && encounter->creditType == ENCOUNTER_CREDIT_KILL_CREATURE &&
                encounter->creditEntry == killed->GetEntry())
            {
                MilestoneService::QueueRun(player->GetGUID(), map->IsRaid(), map->GetInstanceId(), encounter->lastEncounterDungeon);
                return;
            }
    }
}

class GuildAdventureMilestonePlayerScript : public PlayerScript
{
public:
    GuildAdventureMilestonePlayerScript() : PlayerScript("GuildAdventureMilestonePlayerScript", {
        PLAYERHOOK_ON_CREATURE_KILL_CREDIT,
        PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST
    }) { }

    void OnPlayerCreatureKillCredit(Player* player, Creature* killed) override
    {
        if (!IsModuleEnabled() || !player || !killed)
            return;
        // Pets, totems and guardians of players are no mobs.
        if (killed->IsPet() || killed->IsTotem() || killed->GetCharmerOrOwnerGUID().IsPlayer())
            return;
        MilestoneService::QueueKill(player->GetGUID(), killed->GetCreatureType());
        CheckRunEnd(player, killed);
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        if (IsModuleEnabled() && player && quest)
            MilestoneService::QueueQuest(player->GetGUID());
    }
};

class GuildAdventureMilestoneWorldScript : public WorldScript
{
public:
    GuildAdventureMilestoneWorldScript() : WorldScript("GuildAdventureMilestoneWorldScript", {
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_UPDATE,
        WORLDHOOK_ON_SHUTDOWN
    }) { }

    void OnStartup() override
    {
        // After GuildAdventureRosterWorldScript::OnStartup (registered first), which loads the guilds.
        if (IsModuleEnabled())
            sMilestoneService.Load();
    }

    void OnUpdate(uint32 diff) override
    {
        if (IsModuleEnabled())
            sMilestoneService.Update(diff);
    }

    void OnShutdown() override
    {
        if (IsModuleEnabled())
            sMilestoneService.Flush();
    }
};

class GuildAdventureMilestoneCommandScript : public CommandScript
{
public:
    GuildAdventureMilestoneCommandScript() : CommandScript("GuildAdventureMilestoneCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable milestoneTable =
        {
            { "add",  HandleAdminAdd,  SEC_ADMINISTRATOR, Console::Yes },
            { "scan", HandleAdminScan, SEC_ADMINISTRATOR, Console::Yes }
        };
        static ChatCommandTable adminTable =
        {
            { "milestone", milestoneTable }
        };
        static ChatCommandTable gaTable =
        {
            { "milestones", HandleMilestones, SEC_GAMEMASTER, Console::Yes },
            { "admin",      adminTable }
        };
        static ChatCommandTable commandTable =
        {
            { "ga", gaTable }
        };
        return commandTable;
    }

    static AdventureGuild* GuildOf(ChatHandler* handler, Optional<PlayerIdentifier> const& target)
    {
        if (!IsModuleEnabled())
        {
            handler->SendErrorMessage("Guild Adventure is disabled (GuildAdventure.Enable = 0).", false);
            return nullptr;
        }
        AdventureGuild* guild = nullptr;
        if (target)
        {
            guild = sGuildAdventureMgr.FindGuildOfMember(target->GetGUID());
            if (!guild)
                handler->SendErrorMessage(Acore::StringFormat("{} is neither the leader nor a bot of an adventure guild.",
                    target->GetName()), false);
            return guild;
        }
        if (WorldSession* session = handler->GetSession())
            guild = sGuildAdventureMgr.FindGuild(session->GetAccountId());
        if (!guild)
            handler->SendErrorMessage(handler->GetSession() ? "Your account has no adventure guild yet." :
                "Name a guild character: .ga milestones <character>", false);
        return guild;
    }

    static bool HandleMilestones(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        AdventureGuild* guild = GuildOf(handler, target);
        if (!guild)
            return false;

        handler->PSendSysMessage("Tiered guild achievements of account {}: {} of {} active tiers, {} guild points, {} renown "
            "(counting {}).", guild->ownerAccount, sMilestoneService.GetEarnedCount(*guild), sMilestoneService.GetActiveCount(),
            sMilestoneService.GetPoints(*guild), sMilestoneService.GetRenown(*guild), GetSettings().milestonesEnable ? "on" : "off");

        // Only the series the guild has counted something for, so the list stays short.
        for (MilestoneSeries const& series : sMilestoneService.GetSeries())
        {
            uint64 const value = sMilestoneService.GetValue(*guild, series);
            uint8 const active = sMilestoneService.GetActiveTiers(series);
            uint32 reached = 0;
            MilestoneTier const* next = nullptr;
            for (std::size_t i = 0; i < series.tiers.size(); ++i)
            {
                if (sMilestoneService.HasEarned(*guild, series.tiers[i].id))
                    ++reached;
                else if (!next && i < active)
                    next = &series.tiers[i];
            }
            if (!value && !reached)
                continue;
            handler->PSendSysMessage("  {}: {} - {}/{} tiers{}", series.key, value, reached, uint32(active), next ?
                Acore::StringFormat(", next \"{}\"", sMilestoneService.DescribeTier(series, *next)) : std::string(", complete"));
        }
        return true;
    }

    static bool HandleAdminAdd(ChatHandler* handler, PlayerIdentifier target, std::string key, uint32 amount)
    {
        AdventureGuild* guild = GuildOf(handler, target);
        if (!guild)
            return false;
        MilestoneSeries const* series = sMilestoneService.FindSeries(key);
        if (!series || series->IsLive())
        {
            handler->SendErrorMessage(Acore::StringFormat("'{}' is no counted series (kills_<type>, quests, dungeons, raids, "
                "level_class_<class>, level_race_<race>, skill_<skill>; storage_items and guild_gold are read live).", key), false);
            return false;
        }
        uint32 const before = sMilestoneService.GetEarnedCount(*guild);
        sMilestoneService.AddToCounter(*guild, key, amount, target.GetGUID());
        handler->PSendSysMessage("{}: {} now {}, {} new tiers for the guild.", target.GetName(), key,
            sMilestoneService.GetValue(*guild, *series), sMilestoneService.GetEarnedCount(*guild) - before);
        return true;
    }

    static bool HandleAdminScan(ChatHandler* handler, PlayerIdentifier target)
    {
        AdventureGuild* guild = GuildOf(handler, target);
        if (!guild)
            return false;
        uint32 const before = sMilestoneService.GetEarnedCount(*guild);
        sMilestoneService.ScanGuild(*guild);
        handler->PSendSysMessage("Scanned the guild of {}: {} new tiers, {} of {} reached.", target.GetName(),
            sMilestoneService.GetEarnedCount(*guild) - before, sMilestoneService.GetEarnedCount(*guild), sMilestoneService.GetActiveCount());
        return true;
    }
};

void AddGuildAdventureMilestoneScripts()
{
    new GuildAdventureMilestonePlayerScript();
    new GuildAdventureMilestoneWorldScript();
    new GuildAdventureMilestoneCommandScript();
}
