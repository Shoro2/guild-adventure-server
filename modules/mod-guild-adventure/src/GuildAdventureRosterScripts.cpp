/*
 * Hooks of the guild management part (roster, progression, offline window, GADV protocol).
 *
 * Player hooks can fire on map threads (level-ups, quests, deaths and kills of bots run in Map::Update), so they
 * only queue what happened; the world script drains the queue in WorldScript::OnUpdate on the world thread,
 * outside map updates, where the services run (invariant I5 of docs/take-control.md).
 *
 * Hooks used:
 *   PlayerScript  OnPlayerLogin, OnPlayerLogout, OnPlayerLevelChanged, OnPlayerCompleteQuest, OnPlayerJustDied,
 *                 OnPlayerCreatureKill, OnPlayerCanUseChat (private chat: the GADV transport)
 *   WorldScript   OnAfterConfigLoad, OnStartup, OnUpdate
 */

#include "Chat.h"
#include "ClassAbilities.h"
#include "ControlSwapService.h"
#include "GadvProtocol.h"
#include "GuildAdventureConfig.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildAdventureUtil.h"
#include "GuildBotHolder.h"
#include "GuildEvents.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "OfflineService.h"
#include "OrderService.h"
#include "PlayerbotAIConfig.h"
#include "Player.h"
#include "ProgressionService.h"
#include "QuestDef.h"
#include "RosterService.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "StringFormat.h"
#include "Farming.h"
#include "Professions.h"
#include "RoadMap.h"
#include "WorldIndex.h"
#include "WorldSession.h"
#include <mutex>
#include <vector>

using namespace GuildAdventure;

namespace
{
    // GuildAdventure.Leader.WatchOnLogin: does a login into this character keep its auto mode on (watch mode)? Only the
    // leader with an order; 1 = when the AI was playing it at the login, 2 = at every login.
    bool WatchOnLeaderLogin(uint32 account, ObjectGuid guid, bool wasAIPlaying)
    {
        uint32 const mode = GetSettings().leaderWatchOnLogin;
        if (!mode || (mode == 1 && !wasAIPlaying))
            return false;
        AdventureGuild const* guild = sGuildAdventureMgr.FindGuild(account);
        return guild && guild->leader == guid && guild->leaderPlanned;
    }

    enum class HookType : uint8
    {
        Login,
        Logout,
        LevelChanged,
        QuestDone,
        Died,
        CreatureKill
    };

    struct HookEvent
    {
        HookType type = HookType::Login;
        ObjectGuid guid;
        uint32 accountId = 0;
        bool headless = false;
        uint32 value = 0;      // new level, quest id
        uint32 oldValue = 0;   // old level
    };

    std::mutex sHookLock;
    std::vector<HookEvent> sHookEvents;

    void Queue(HookEvent event)
    {
        std::lock_guard<std::mutex> lock(sHookLock);
        sHookEvents.push_back(std::move(event));
    }

    HookEvent MakeEvent(HookType type, Player* player)
    {
        HookEvent event;
        event.type = type;
        event.guid = player->GetGUID();
        if (WorldSession* session = player->GetSession())
        {
            event.accountId = session->GetAccountId();
            event.headless = session->IsHeadless();
        }
        return event;
    }

    void ProcessBotEvent(HookEvent const& event)
    {
        AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfBot(event.guid);
        GuildBot* bot = guild ? guild->FindBot(event.guid) : nullptr;
        if (!bot)
        {
            // A leader with an order: its planner looks at the zones again after a level-up (no renown, no event:
            // the leader's own progress is not the guild's roster progress).
            if (event.type == HookType::LevelChanged && event.value > event.oldValue)
                if (AdventureGuild* leaderGuild = sGuildAdventureMgr.FindGuildOfLeader(event.guid))
                    if (leaderGuild->leaderPlanned)
                    {
                        leaderGuild->leaderPlan.snapshot.level = uint8(event.value);
                        sOrderService.OnLevelUp(*leaderGuild, leaderGuild->leaderPlan, uint8(event.value));
                    }
            return;
        }

        Player* player = ObjectAccessor::FindConnectedPlayer(event.guid);
        uint32 const zoneId = player ? player->GetZoneId() : bot->snapshot.zoneId;

        switch (event.type)
        {
            case HookType::LevelChanged:
            {
                if (event.value <= event.oldValue)
                    break;
                bot->snapshot.level = uint8(event.value);
                for (uint32 level = event.oldValue + 1; level <= event.value; ++level)
                {
                    int32 const points = sGuildAdventureMgr.GetRenownPoints("LEVEL_UP");
                    RecordEvent(*guild, "LEVEL_UP", bot->guid, Acore::StringFormat("{} reached level {}.", bot->name, level),
                        uint8(level), zoneId, points);
                    sProgressionService.AwardForEvent(*guild, "LEVEL_UP", bot->guid);
                }
                sOrderService.OnLevelUp(*guild, *bot, uint8(event.value));
                break;
            }
            case HookType::QuestDone:
            {
                ++bot->quests;
                sGuildAdventureMgr.SaveBot(*guild, *bot);
                std::string title;
                if (Quest const* quest = sObjectMgr->GetQuestTemplate(event.value))
                    title = quest->GetTitle();
                int32 const points = sGuildAdventureMgr.GetRenownPoints("QUEST_DONE");
                RecordEvent(*guild, "QUEST_DONE", bot->guid, title.empty() ?
                    Acore::StringFormat("{} completed a quest.", bot->name) :
                    Acore::StringFormat("{} completed \"{}\".", bot->name, title), bot->snapshot.level, zoneId, points);
                sProgressionService.AwardForEvent(*guild, "QUEST_DONE", bot->guid);
                break;
            }
            case HookType::Died:
            {
                ++bot->deaths;
                sGuildAdventureMgr.SaveBot(*guild, *bot);
                std::string const zone = GetAreaName(zoneId);
                RecordEvent(*guild, "DEATH", bot->guid, zone.empty() ? Acore::StringFormat("{} died.", bot->name) :
                    Acore::StringFormat("{} died in {}.", bot->name, zone), bot->snapshot.level, zoneId);
                break;
            }
            case HookType::CreatureKill:
                // Counted only; saved with the next change of the bot row (or the next kill batch below).
                ++bot->kills;
                if (bot->kills % 25 == 0)
                    sGuildAdventureMgr.SaveBot(*guild, *bot);
                break;
            default:
                break;
        }
    }

    // The order planner relies on these mod-playerbots settings (docs/orders.md "Playerbots settings"); warn once at
    // startup instead of silently behaving differently.
    void CheckPlayerbotsSettingsForOrders()
    {
        uint32 weights = 0;
        for (auto const& [status, weight] : sPlayerbotAIConfig.RpgStatusProbWeight)
            weights += weight;
        if (weights)
            LOG_WARN("module", "mod-guild-adventure: Playerbots.RpgStatusProbWeight.* are not all 0; NewRpg will pick random "
                "activities between the order planner's tasks. Set them to 0 (tools/configure-server.ps1).");

        if (sPlayerbotAIConfig.BotCheatMask & (uint32(BotCheatMask::food) | uint32(BotCheatMask::taxi) | uint32(BotCheatMask::gold)))
            LOG_WARN("module", "mod-guild-adventure: Playerbots.BotCheats gives guild bots free food, flight paths or gold; "
                "guild bots should progress legitimately (the design log section 1). Recommended: Playerbots.BotCheats = \"\".");

        if (sPlayerbotAIConfig.AutoPickTalentsForPlayerControlledBots && GetSettings().specsEnable)
            LOG_WARN("module", "mod-guild-adventure: Playerbots.AutoPickTalentsForPlayerControlledBots = 1 lets mod-playerbots spend "
                "the guild bots' talent points before their talent template does (docs/specs.md). Recommended: 0.");

        if (sPlayerbotAIConfig.LootNeedRollLevel == 0)
            LOG_WARN("module", "mod-guild-adventure: Playerbots.LootNeedRollLevel = 0 makes guild bots pass on every roll, so "
                "on a dungeon run no bot rolls for an upgrade (docs/orders.md \"Dungeon runs\"). Recommended: 1.");

        if (sPlayerbotAIConfig.SummonWhenGroup)
            LOG_WARN("module", "mod-guild-adventure: Playerbots.SummonWhenGroup = 1 teleports a following guild bot to the owner. "
                "Recommended: 0.");
    }

    void ProcessHookEvents()
    {
        std::vector<HookEvent> events;
        {
            std::lock_guard<std::mutex> lock(sHookLock);
            events.swap(sHookEvents);
        }

        for (HookEvent const& event : events)
        {
            // Every weapon skill the class may learn, for every guild character (the leader too): on login and after a
            // level-up (ClassAbilities::LearnWeaponSkills; mod-autolearn-skills leaves gaps).
            if (event.type == HookType::Login || (event.type == HookType::LevelChanged && event.value > event.oldValue))
                if (sGuildAdventureMgr.FindGuildOfMember(event.guid))
                    if (Player* player = ObjectAccessor::FindConnectedPlayer(event.guid))
                    {
                        if (std::string const learned = ClassAbilities::LearnWeaponSkills(player); !learned.empty())
                            LOG_INFO("module", "mod-guild-adventure: {} (level {}) learned weapon skills its class may use: {}",
                                player->GetName(), uint32(player->GetLevel()), learned);
                        // A rogue's Lockpicking with its level (the owner, 2026-11-26).
                        if (std::string const raised = ClassAbilities::RaiseLockpicking(player); !raised.empty())
                            LOG_INFO("module.ga.activity", "activity: {} (level {}) has its Lockpicking for its level: {}",
                                player->GetName(), uint32(player->GetLevel()), raised);
                    }

            switch (event.type)
            {
                case HookType::Login:
                {
                    // Guild bots log in on headless home sessions; only the owner's client logins matter here.
                    if (event.headless)
                        break;
                    if (Player* player = ObjectAccessor::FindConnectedPlayer(event.guid))
                    {
                        sRosterService.OnOwnerCharacterLogin(player);
                        // A login from the database: the leader was not playing by itself (WatchOnLogin = 2 only).
                        if (WorldSession* session = player->GetSession(); session && !session->IsHeadless() &&
                            WatchOnLeaderLogin(session->GetAccountId(), player->GetGUID(), false))
                            sControlSwapService.QueueWatchOnLogin(session->GetAccountId(), player->GetGUID());
                    }
                    break;
                }
                case HookType::Logout:
                    if (!event.headless)
                        sGadvProtocol.OnPlayerLogout(event.accountId, event.guid);
                    break;
                default:
                    ProcessBotEvent(event);
                    break;
            }
        }
    }
}

class GuildAdventureRosterPlayerScript : public PlayerScript
{
public:
    GuildAdventureRosterPlayerScript() : PlayerScript("GuildAdventureRosterPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_LEVEL_CHANGED,
        PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
        PLAYERHOOK_ON_PLAYER_JUST_DIED,
        PLAYERHOOK_ON_CREATURE_KILL,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
        PLAYERHOOK_ON_SPELL_CAST
    }) { }

    // Only the private-chat overload is used; keep the others visible.
    using PlayerScript::OnPlayerCanUseChat;

    void OnPlayerLogin(Player* player) override
    {
        if (IsModuleEnabled())
            Queue(MakeEvent(HookType::Login, player));
    }

    void OnPlayerLogout(Player* player) override
    {
        Queue(MakeEvent(HookType::Logout, player));
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        HookEvent event = MakeEvent(HookType::LevelChanged, player);
        event.value = player->GetLevel();
        event.oldValue = oldLevel;
        Queue(std::move(event));
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        HookEvent event = MakeEvent(HookType::QuestDone, player);
        event.value = quest ? quest->GetQuestId() : 0;
        Queue(std::move(event));
    }

    void OnPlayerJustDied(Player* player) override
    {
        Queue(MakeEvent(HookType::Died, player));
    }

    // (Debug) What an AI-played character casts while it fights: the fight trace samples what is being cast every two
    // seconds and misses every instant spell (a level-60 mage stood "casting nothing" for five seconds of each fight
    // while its mana went down, round 148). Map thread: the line only, nothing of the planner's.
    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!GetSettings().debug || !spell || !player || !player->IsInCombat())
            return;
        WorldSession* session = player->GetSession();
        if (!session || !session->IsHeadless())
            return;
        Unit const* target = spell->m_targets.GetUnitTarget();
        LOG_INFO("module.ga.activity", "activity: {} casts {} ({}) in its fight on {}", player->GetName(),
            spell->GetSpellInfo()->SpellName[0], spell->GetSpellInfo()->Id, target ? (target == player ? std::string("itself") :
            target->GetName()) : std::string("no unit"));
    }

    void OnPlayerCreatureKill(Player* killer, Creature* /*killed*/) override
    {
        Queue(MakeEvent(HookType::CreatureKill, killer));
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
    {
        if (type != CHAT_MSG_WHISPER || language != LANG_ADDON || !receiver)
            return true;

        // GADV transport (docs/protocol.md section 1): an addon whisper to oneself is consumed, never delivered.
        if (receiver == player)
            return !sGadvProtocol.OnAddonWhisper(player, msg);

        // Only client sessions get here with a foreign receiver; their chat runs on the world thread, where the
        // guild bot holder may be read. Bots' own whispers are left alone.
        WorldSession* session = player->GetSession();
        if (!session || session->IsHeadless())
            return true;

        ObjectGuid const receiverGuid = receiver->GetGUID();
        bool const managed = sGuildBotHolder.IsManaged(receiverGuid);

        // After a Path W swap the client still reports the previous character's name as UnitName("player"), so
        // the addon whispers its GADV traffic to that character, which the AI now drives (docs/take-control.md
        // section 5.8). A whisper to one of the sender account's own guild characters is the sender's own traffic.
        if (managed && sGuildBotHolder.GetOwner(receiverGuid) == session->GetAccountId() &&
            sGadvProtocol.OnAddonWhisper(player, msg))
            return false;

        // GADV text travels between a client and the server only; it is never delivered to another player.
        if (GadvProtocol::IsTransportText(msg))
            return false;

        if (!managed)
            return true;

        // A guild character its owner plays is an ordinary player for other players' addons. One the AI drives (or that
        // is still loading) reads no addon whispers, so they are not delivered there.
        WorldSession* receiverSession = receiver->GetSession();
        return receiverSession && !receiverSession->IsHeadless();
    }
};

class GuildAdventureRosterWorldScript : public WorldScript
{
public:
    GuildAdventureRosterWorldScript() : WorldScript("GuildAdventureRosterWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_UPDATE
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadSettings();
    }

    void OnStartup() override
    {
        sGuildAdventureMgr.LoadTemplates();
        sGuildAdventureMgr.LoadState();
        sGuildAdventureMgr.PruneEventLog(GetSettings().eventLogKeepDays);
        sWorldIndex.Build();
        sRoadMap.Load();
        sProfessions.Load();
        sFarming.Load();
        for (auto& [owner, guild] : sGuildAdventureMgr.GetGuilds())
            sProgressionService.ApplyUnlocks(guild);
        sGadvProtocol.Init();
        CheckPlayerbotsSettingsForOrders();

        // Only guild bots that follow get the owner's character as playerbots master (docs/orders.md).
        sGuildBotHolder.SetMasterPolicy([](ObjectGuid bot) { return sOrderService.WantsMaster(bot); });

        // Watch mode (docs/take-control.md §5.12) needs a character the order planner plays: a roster bot, or the
        // leader once it has an order (docs/orders.md "The leader").
        sControlSwapService.SetWatchPolicy([](ObjectGuid guid)
        {
            if (sGuildAdventureMgr.FindGuildOfBot(guid))
                return SwapRefusal::None;
            if (AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfLeader(guid))
                return guild->leaderPlanned ? SwapRefusal::None : SwapRefusal::NoOrder;
            return SwapRefusal::NotGuildBot;
        });
        sGuildBotHolder.SetLeaderPlanPolicy([](ObjectGuid guid) { return sOrderService.IsLeaderPlanned(guid); });
        // A member of a dungeon group that its owner plays by hand carries a placeholder AI (GuildBotHolder.h).
        sGuildBotHolder.SetPlaceholderPolicy([](ObjectGuid guid) { return sOrderService.IsDungeonGuest(guid); });
        sControlSwapService.SetWatchOnLoginPolicy(&WatchOnLeaderLogin);
        // GuildAdventure.Leader.OfflinePlay = 0: the leader logs out with its owner whatever its order (D12).
        sControlSwapService.SetLeaderStaysPolicy([](uint32 ownerAccount, ObjectGuid guid)
        {
            if (!GetSettings().leaderOfflinePlay)
                return false;
            AdventureGuild* guild = sGuildAdventureMgr.FindGuild(ownerAccount);
            return guild && guild->leaderPlanned && (guid.IsEmpty() || guid == guild->leader);
        });
        sControlSwapService.SetPlannedLeadersSource([]()
        {
            std::vector<std::pair<uint32, ObjectGuid>> leaders;
            if (!GetSettings().leaderOfflinePlay)
                return leaders;
            for (auto const& [owner, guild] : sGuildAdventureMgr.GetGuilds())
                if (guild.leaderPlanned)
                    leaders.emplace_back(owner, guild.leader);
            return leaders;
        });

        // The leader can also come back without a database login: the client picks the AI-driven leader at the
        // character screen (Path R attach) or swaps back to it. OnPlayerLogin does not fire then, so the owner's
        // return (offline window, bots that logged out in the meantime) is handled on the swap's completion.
        sControlSwapService.AddListener([](SwapEvent const& event)
        {
            if (event.type != SwapEventType::Completed)
                return;
            AdventureGuild* guild = sGuildAdventureMgr.FindGuild(event.ownerAccountId);
            if (!guild || event.to != guild->leader)
                return;
            if (Player* leader = ObjectAccessor::FindConnectedPlayer(event.to))
                sRosterService.OnOwnerCharacterLogin(leader);
        });
        _started = true;
    }

    void OnUpdate(uint32 diff) override
    {
        if (!_started)
            return;

        // Runs after GuildAdventureWorldScript::OnUpdate (registered first), so bots that the holder adopted this
        // tick are already in the world.
        ProcessHookEvents();
        sGadvProtocol.Update(diff);
        sRosterService.Update(diff);
        sOfflineService.Update(diff);
        sOrderService.Update(diff);

        // The event log is trimmed at startup and a few times a day (GuildAdventure.EventLog.KeepDays).
        _pruneTimer += diff;
        if (_pruneTimer >= PruneIntervalMs)
        {
            _pruneTimer = 0;
            sGuildAdventureMgr.PruneEventLog(GetSettings().eventLogKeepDays);
        }
    }

private:
    static constexpr uint32 PruneIntervalMs = 6 * HOUR * IN_MILLISECONDS;

    bool _started = false;
    uint32 _pruneTimer = 0;
};

void AddGuildAdventureRosterScripts()
{
    new GuildAdventureRosterPlayerScript();
    new GuildAdventureRosterWorldScript();
}
