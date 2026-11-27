/*
 * Hooks and commands of the guild-wide achievements (AchievementService).
 *
 * The achievement hook fires wherever the core completes an achievement, mostly on map threads (kills, quests,
 * level-ups of bots run in Map::Update), so it only queues; the world script drains the queue in WorldScript::OnUpdate
 * on the world thread, after the roster scripts (registered later), so a leader's first login has founded the guild
 * before its achievements are looked at.
 *
 * Hooks used:
 *   PlayerScript  OnPlayerAchievementComplete, OnPlayerLogin
 *   ServerScript  CanPacketSend (keeps the announcements of a shared completion from the other clients)
 *   WorldScript   OnStartup, OnUpdate
 *
 * Commands (they join the ".ga" tree of the other command scripts):
 *   .ga achievements [character]              the guild's achievements (GM; also console with a character)
 *   .ga admin achieve <character> <id>        the character completes achievement <id> as if it had met its criteria
 *                                             (administrator, also console / SOAP; tests without a client)
 */

#include "AchievementMgr.h"
#include "AchievementService.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CommandScript.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "GuildAdventureConfig.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <mutex>
#include <vector>

using namespace Acore::ChatCommands;
using namespace GuildAdventure;

namespace
{
    struct AchievementHookEvent
    {
        ObjectGuid guid;
        uint32 achievementId = 0; // 0 = the character entered the world
    };

    std::mutex sAchievementHookLock;
    std::vector<AchievementHookEvent> sAchievementHookEvents;

    void QueueAchievementEvent(ObjectGuid guid, uint32 achievementId)
    {
        std::lock_guard<std::mutex> lock(sAchievementHookLock);
        sAchievementHookEvents.push_back({ guid, achievementId });
    }

    void ProcessAchievementEvents()
    {
        std::vector<AchievementHookEvent> events;
        {
            std::lock_guard<std::mutex> lock(sAchievementHookLock);
            events.swap(sAchievementHookEvents);
        }

        // Completions the processing causes (meta achievements of a shared one) queue again and run next tick.
        for (AchievementHookEvent const& event : events)
        {
            if (event.achievementId)
                sAchievementService.OnAchievementCompleted(event.guid, event.achievementId);
            else
                sAchievementService.OnCharacterInWorld(event.guid);
        }
    }

    std::string FormatAgo(uint32 time)
    {
        uint32 const now = uint32(GameTime::GetGameTime().count());
        uint32 const ago = now > time ? now - time : 0;
        if (ago < MINUTE)
            return "just now";
        if (ago < HOUR)
            return Acore::StringFormat("{} min ago", ago / MINUTE);
        if (ago < DAY)
            return Acore::StringFormat("{} h ago", ago / HOUR);
        return Acore::StringFormat("{} d ago", ago / DAY);
    }
}

class GuildAdventureAchievementPlayerScript : public PlayerScript
{
public:
    GuildAdventureAchievementPlayerScript() : PlayerScript("GuildAdventureAchievementPlayerScript", {
        PLAYERHOOK_ON_ACHI_COMPLETE,
        PLAYERHOOK_ON_LOGIN
    }) { }

    void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override
    {
        // The module's own shares are not news; a meta achievement they complete is (another id).
        if (!IsModuleEnabled() || !player || !achievement || AchievementService::IsBeingShared(player, achievement->ID))
            return;
        QueueAchievementEvent(player->GetGUID(), achievement->ID);
    }

    void OnPlayerLogin(Player* player) override
    {
        // Every guild character: the owner's client logins and the bots' headless ones.
        if (IsModuleEnabled() && player)
            QueueAchievementEvent(player->GetGUID(), 0);
    }
};

class GuildAdventureAchievementServerScript : public ServerScript
{
public:
    GuildAdventureAchievementServerScript() : ServerScript("GuildAdventureAchievementServerScript", {
        SERVERHOOK_CAN_PACKET_SEND
    }) { }

    bool CanPacketSend(WorldSession* session, WorldPacket const& packet) override
    {
        return !AchievementService::ShouldSuppressPacket(session, packet);
    }
};

class GuildAdventureAchievementWorldScript : public WorldScript
{
public:
    GuildAdventureAchievementWorldScript() : WorldScript("GuildAdventureAchievementWorldScript", {
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_UPDATE
    }) { }

    void OnStartup() override
    {
        // After GuildAdventureRosterWorldScript::OnStartup (registered first), which loads the guilds.
        sAchievementService.Load();
        _started = true;
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        if (_started)
            ProcessAchievementEvents();
    }

private:
    bool _started = false;
};

class GuildAdventureAchievementCommandScript : public CommandScript
{
public:
    GuildAdventureAchievementCommandScript() : CommandScript("GuildAdventureAchievementCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable adminTable =
        {
            { "achieve", HandleAdminAchieve, SEC_ADMINISTRATOR, Console::Yes }
        };
        static ChatCommandTable gaTable =
        {
            { "achievements", HandleAchievements, SEC_GAMEMASTER, Console::Yes },
            { "admin",        adminTable }
        };
        static ChatCommandTable commandTable =
        {
            { "ga", gaTable }
        };
        return commandTable;
    }

    static bool HandleAchievements(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        if (!IsModuleEnabled())
        {
            handler->SendErrorMessage("Guild Adventure is disabled (GuildAdventure.Enable = 0).", false);
            return false;
        }

        AdventureGuild* guild = nullptr;
        if (target)
        {
            guild = sAchievementService.FindGuildOf(target->GetGUID());
            if (!guild)
            {
                handler->SendErrorMessage(Acore::StringFormat("{} is neither the leader nor a bot of an adventure guild.",
                    target->GetName()), false);
                return false;
            }
        }
        else if (WorldSession* session = handler->GetSession())
            guild = sGuildAdventureMgr.FindGuild(session->GetAccountId());
        if (!guild)
        {
            handler->SendErrorMessage(target || handler->GetSession() ? "Your account has no adventure guild yet." :
                "Name a guild character: .ga achievements <character>", false);
            return false;
        }

        handler->PSendSysMessage("Guild achievements of account {}: {} achievements, {} achievement points, {} renown from them "
            "(sharing with the members {}).", guild->ownerAccount, guild->achievements.size(), sAchievementService.GetPoints(*guild),
            sAchievementService.GetRenown(*guild), GetSettings().achievementsShare ? "on" : "off");

        if (target && target->IsConnected())
        {
            uint32 has = 0;
            uint32 shareable = 0;
            sAchievementService.CountOnCharacter(*guild, target->GetConnectedPlayer(), has, shareable);
            handler->PSendSysMessage("{} has {} of the guild's {} shareable achievements.", target->GetName(), has, shareable);
        }

        for (GuildAchievement const* achievement : sAchievementService.GetNewest(*guild, 10))
        {
            AchievementEntry const* entry = sAchievementStore.LookupEntry(achievement->id);
            char const* name = entry ? entry->name[LOCALE_enUS] : nullptr;
            std::string earner;
            sCharacterCache->GetCharacterNameByGuid(achievement->earnedBy, earner);
            handler->PSendSysMessage("  {} \"{}\" - {}, +{} renown, {}", achievement->id, name ? name : "?", earner.empty() ? "?" : earner,
                achievement->renown, FormatAgo(achievement->earnedAt));
        }
        return true;
    }

    static bool HandleAdminAchieve(ChatHandler* handler, PlayerIdentifier target, uint32 achievementId)
    {
        Player* player = target.GetConnectedPlayer();
        if (!player || !player->IsInWorld())
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not in the world.", target.GetName()), false);
            return false;
        }
        if (!sAchievementService.FindGuildOf(player->GetGUID()))
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is neither the leader nor a bot of an adventure guild.", target.GetName()), false);
            return false;
        }

        AchievementEntry const* entry = sAchievementStore.LookupEntry(achievementId);
        if (!entry)
        {
            handler->SendErrorMessage(Acore::StringFormat("Unknown achievement {}.", achievementId), false);
            return false;
        }
        if (player->HasAchieved(achievementId))
        {
            handler->PSendSysMessage("{} already has \"{}\" ({}).", player->GetName(), entry->name[LOCALE_enUS], achievementId);
            return true;
        }
        if (player->IsGameMaster())
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is in GM mode; the core refuses achievements there.", player->GetName()), false);
            return false;
        }

        player->CompletedAchievement(entry);
        if (!player->HasAchieved(achievementId))
        {
            handler->SendErrorMessage(Acore::StringFormat("{} could not earn \"{}\" ({}).", player->GetName(), entry->name[LOCALE_enUS],
                achievementId), false);
            return false;
        }
        handler->PSendSysMessage("{} earned \"{}\" ({}, {} points).", player->GetName(), entry->name[LOCALE_enUS], achievementId, entry->points);
        return true;
    }
};

void AddGuildAdventureAchievementScripts()
{
    new GuildAdventureAchievementPlayerScript();
    new GuildAdventureAchievementServerScript();
    new GuildAdventureAchievementWorldScript();
    new GuildAdventureAchievementCommandScript();
}
