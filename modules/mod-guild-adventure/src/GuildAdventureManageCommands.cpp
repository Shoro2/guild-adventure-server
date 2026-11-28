/*
 * ".ga" GM commands of the guild management part (the design log section 2.13). They join the ".ga" tree of
 * GuildAdventureCommandScript (take control: bot login/logout/list, swap, status); the core merges both.
 *
 *   .ga info                                       your adventure guild
 *   .ga roster [player]                            the guild bots of your guild (or of the player's guild)
 *   .ga renown add <points>                        add (or remove, negative) renown
 *   .ga slots set <n>                              set the unlocked bot slots
 *   .ga offline set <minutes>                      set the offline window
 *   .ga bot recruit <race> <class> <gender> <name> recruit a bot (ids as in docs/protocol.md section 8.7)
 *   .ga bot order <name> <order> [zone]            set a bot's order (AUTO, IDLE, FOLLOW, LEVEL_ZONE <zone id>); for
 *                                                  the leader also HOLD, its default (docs/orders.md "The leader")
 *   .ga bot bench <name> / .ga bot activate <name>
 *   .ga bot plan <name>                            what the order planner does with that bot (also console)
 *   .ga reload                                     reload the world-database templates and the world index
 *
 * The commands above act on the guild of the GM's own account and do not run from the console, except
 * ".ga bot plan" and ".ga reload". The ".ga admin" commands (administrator, also console / SOAP) act on the guild of
 * the named leader; they exist for tests without a client (docs/orders.md "Tests"):
 *
 *   .ga admin login <leader>                       log in the leader's active guild bots (no master)
 *   .ga admin logout <leader>                      log out the leader's AI-driven guild bots
 *   .ga admin order <bot> <order> [zone]           set a bot's order
 *   .ga admin offline <leader> <minutes>           set the offline window
 *   .ga admin recruit <leader> <race> <class> <gender> <name>   recruit a bot for that leader's guild
 *   .ga admin unlock <leader> <unlock id>          buy an unlock for that leader's guild as the owner's Buy does (gold
 *                                                  from the guild bank and / or the leader, bot setting Unlocks.PayFrom)
 *
 *   .ga unlocks [leader]                           the unlocks of your guild (console: of the leader's guild) with their
 *                                                  state, cost and what the guild can pay now (GM, console)
 */

#include "CharacterCache.h"
#include "BotSettings.h"
#include "Chat.h"
#include "CommandScript.h"
#include "ControlSwapService.h"
#include "Creature.h"
#include "GadvProtocol.h"
#include "GuildAdventureConfig.h"
#include "GearUpkeep.h"
#include "Dungeons.h"
#include "GuildAdventureMgr.h"
#include "GuildBudget.h"
#include "GuildAdventureSettings.h"
#include "GuildAdventureUtil.h"
#include "GroundGuard.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "GuildBotHolder.h"
#include "GuildEvents.h"
#include "Log.h"
#include "MapMgr.h"
#include "OfflineService.h"
#include "ObjectAccessor.h"
#include "OrderService.h"
#include "PathProbe.h"
#include "DungeonNav.h"
#include "SpecService.h"
#include "QuestPartners.h"
#include "Squads.h"
#include "StringConvert.h"
#include "Player.h"
#include "ProgressionService.h"
#include "QuestEvents.h"
#include "QuestSupport.h"
#include "Farming.h"
#include "Professions.h"
#include "RoadMap.h"
#include "RosterService.h"
#include "StringConvert.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Util.h"
#include "World.h"
#include "WorldIndex.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <algorithm>
#include <cctype>

using namespace Acore::ChatCommands;
using namespace GuildAdventure;

namespace
{
    AdventureGuild* OwnGuild(ChatHandler* handler)
    {
        if (!IsModuleEnabled())
        {
            handler->SendErrorMessage("Guild Adventure is disabled (GuildAdventure.Enable = 0).", false);
            return nullptr;
        }

        AdventureGuild* guild = sGuildAdventureMgr.FindGuild(handler->GetSession()->GetAccountId());
        if (!guild)
            handler->SendErrorMessage("Your account has no adventure guild yet; it is founded when you enter the world.", false);
        return guild;
    }

    // The adventure guild a leader leads (console commands name the leader instead of using the GM's account).
    AdventureGuild* GuildOfLeader(ChatHandler* handler, PlayerIdentifier const& leader)
    {
        AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfLeader(leader.GetGUID());
        if (!guild)
            handler->SendErrorMessage(Acore::StringFormat("{} leads no adventure guild.", leader.GetName()), false);
        return guild;
    }

    // Sets a bot's order like the protocol's ORDER_SET (validation, persistence, planner, push, event).
    // The leader (docs/orders.md "The leader") passes guild.leaderPlan as 'bot' and may also take HOLD (no order).
    bool ApplyOrder(ChatHandler* handler, AdventureGuild& guild, GuildBot& bot, std::string orderToken, Optional<uint32> zoneId,
        Optional<uint32> count = {})
    {
        bool const isLeader = bot.guid == guild.leader;
        std::transform(orderToken.begin(), orderToken.end(), orderToken.begin(), ::toupper);
        if (isLeader && orderToken == "HOLD")
        {
            sOrderService.SetLeaderOrder(guild, std::nullopt);
            sControlSwapService.EndWatchOf(guild.leader);
            PushBot(guild, guild.leader, false);
            RecordEvent(guild, "ORDER", guild.leader, Acore::StringFormat("{} holds its position again (no order).", bot.name));
            handler->PSendSysMessage("{}: no order, holds its position while the AI holds it.", bot.name);
            return true;
        }

        std::optional<OrderType> type = OrderTypeFromToken(orderToken);
        // A dungeon run (docs/orders.md "Dungeon runs"): DUNGEON <dungeon id of ".ga admin dungeon">.
        if (type && *type == OrderType::Dungeon)
        {
            std::string error;
            if (!zoneId || !sOrderService.CheckDungeonOrder(guild, bot, *zoneId, error))
            {
                handler->SendErrorMessage(Acore::StringFormat("No dungeon order: {}.", zoneId ? error :
                    std::string("name the dungeon's id (.ga admin dungeon lists them)")), false);
                return false;
            }
            BotOrder order = bot.order;
            order.type = OrderType::Dungeon;
            order.zoneId = *zoneId;
            order.setAt = Now();
            bot.order = order;
            sGuildAdventureMgr.SaveOrder(bot);
            // (0 runs: run after run until it is stopped.)
            sOrderService.SetDungeonRuns(bot.guid, count && *count == 0 ? OrderService::DungeonRunsEndless : count.value_or(1));
            sOrderService.OnOrderChanged(guild, bot);
            PushBot(guild, bot.guid, false);
            RecordEvent(guild, "ORDER", bot.guid, Acore::StringFormat("{}: new order DUNGEON.", bot.name), 0, 0);
            handler->PSendSysMessage("{}: order DUNGEON {} ({} run{}).", bot.name, *zoneId, sOrderService.GetDungeonRuns(bot.guid),
                sOrderService.GetDungeonRuns(bot.guid) == 1 ? "" : "s");
            return true;
        }
        if (!type || !IsOrderImplemented(*type))
        {
            handler->SendErrorMessage(isLeader ? "Orders: AUTO, IDLE, FOLLOW, LEVEL_ZONE <zone id>, CRAFT, FARM <item id> [<target stock>], HOLD "
                "(the leader's default)." : "Orders: AUTO, IDLE, FOLLOW, LEVEL_ZONE <zone id>, CRAFT, DUNGEON <dungeon id>, FARM <item id> "
                "[<target stock>].", false);
            return false;
        }

        // The AUTO limits (level cap, continent, ...) set through the addon stay; only the order type and zone change.
        BotOrder order = bot.order;
        order.type = *type;
        order.zoneId = 0;
        order.farmTarget = 0;
        if (OrderNeedsZone(*type))
        {
            ZoneTemplate const* zone = zoneId ? sGuildAdventureMgr.GetZone(*zoneId) : nullptr;
            if (!zone || !zone->AllowsTeam(guild.team) || !zone->AllowsOrder(*type))
            {
                handler->SendErrorMessage("Unknown zone, zone of the other faction, or the order is not allowed there.", false);
                return false;
            }
            order.zoneId = zone->zoneId;
        }
        // A farm order (docs/orders.md "Farm orders"): FARM <item id> [<target stock>].
        if (*type == OrderType::Farm)
        {
            std::string error;
            if (!zoneId || !sOrderService.CheckFarmOrder(guild, *zoneId, error))
            {
                handler->SendErrorMessage(Acore::StringFormat("No farm order: {}.", zoneId ? error : std::string("name the item's id")), false);
                return false;
            }
            order.zoneId = *zoneId;
            order.farmTarget = count.value_or(0);
        }
        // (Any other order ends a dungeon order's further runs.)
        sOrderService.SetDungeonRuns(bot.guid, 1);
        if (isLeader)
            sOrderService.SetLeaderOrder(guild, order);
        else
        {
            order.setAt = Now();
            bot.order = order;
            sGuildAdventureMgr.SaveOrder(bot);
            sOrderService.OnOrderChanged(guild, bot);
        }
        PushBot(guild, bot.guid, false);
        ItemTemplate const* farmItem = order.type == OrderType::Farm ? sObjectMgr->GetItemTemplate(order.zoneId) : nullptr;
        RecordEvent(guild, "ORDER", bot.guid, Acore::StringFormat("{}: new order {}{}.", bot.name, ToToken(order.type),
            farmItem ? " " + farmItem->Name1 : std::string()), 0, farmItem ? 0 : order.zoneId);
        handler->PSendSysMessage("{}: order {}{}{}.", bot.name, ToToken(order.type),
            order.zoneId ? Acore::StringFormat(" ({})", order.zoneId) : std::string(),
            order.farmTarget ? Acore::StringFormat(", target stock {}", order.farmTarget) : std::string());
        return true;
    }

    GuildBot* FindBot(ChatHandler* handler, AdventureGuild& guild, PlayerIdentifier const& target)
    {
        GuildBot* bot = guild.FindBot(target.GetGUID());
        if (!bot)
            handler->SendErrorMessage(Acore::StringFormat("{} is not one of your guild bots.", target.GetName()), false);
        return bot;
    }
}

class GuildAdventureManageCommandScript : public CommandScript
{
public:
    GuildAdventureManageCommandScript() : CommandScript("GuildAdventureManageCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable renownTable =
        {
            { "add", HandleRenownAdd, SEC_GAMEMASTER, Console::No }
        };
        static ChatCommandTable slotsTable =
        {
            { "set", HandleSlotsSet, SEC_GAMEMASTER, Console::No }
        };
        static ChatCommandTable offlineTable =
        {
            { "set", HandleOfflineSet, SEC_GAMEMASTER, Console::No }
        };
        static ChatCommandTable botTable =
        {
            { "recruit",  HandleBotRecruit,  SEC_GAMEMASTER, Console::No },
            { "order",    HandleBotOrder,    SEC_GAMEMASTER, Console::No },
            { "bench",    HandleBotBench,    SEC_GAMEMASTER, Console::No },
            { "activate", HandleBotActivate, SEC_GAMEMASTER, Console::No },
            { "dismiss",  HandleBotDismiss,  SEC_GAMEMASTER, Console::No },
            { "plan",     HandleBotPlan,     SEC_GAMEMASTER, Console::Yes }
        };
        static ChatCommandTable adminTable =
        {
            { "login",   HandleAdminLogin,   SEC_ADMINISTRATOR, Console::Yes },
            { "logout",  HandleAdminLogout,  SEC_ADMINISTRATOR, Console::Yes },
            { "order",   HandleAdminOrder,   SEC_ADMINISTRATOR, Console::Yes },
            { "offline", HandleAdminOffline, SEC_ADMINISTRATOR, Console::Yes },
            { "recruit", HandleAdminRecruit, SEC_ADMINISTRATOR, Console::Yes },
            { "kill",    HandleAdminKill,    SEC_ADMINISTRATOR, Console::Yes },
            { "dismiss", HandleAdminDismiss, SEC_ADMINISTRATOR, Console::Yes },
            { "selfcast", HandleAdminSelfCast, SEC_ADMINISTRATOR, Console::Yes },
            { "attack",  HandleAdminAttack,  SEC_ADMINISTRATOR, Console::Yes },
            { "hub",     HandleAdminHub,     SEC_ADMINISTRATOR, Console::Yes },
            { "dungeon", HandleAdminDungeon, SEC_ADMINISTRATOR, Console::Yes },
            { "farm",    HandleAdminFarm,    SEC_ADMINISTRATOR, Console::Yes },
            { "quests",  HandleAdminQuests,  SEC_ADMINISTRATOR, Console::Yes },
            { "path",    HandleAdminPath,    SEC_ADMINISTRATOR, Console::Yes },
            { "navway",  HandleAdminNavWay,  SEC_ADMINISTRATOR, Console::Yes },
            { "walk",    HandleAdminWalk,    SEC_ADMINISTRATOR, Console::Yes },
            { "route",   HandleAdminRoute,   SEC_ADMINISTRATOR, Console::Yes },
            { "spec",       HandleAdminSpec,       SEC_ADMINISTRATOR, Console::Yes },
            { "specsave",   HandleAdminSpecSave,   SEC_ADMINISTRATOR, Console::Yes },
            { "specdelete", HandleAdminSpecDelete, SEC_ADMINISTRATOR, Console::Yes },
            { "takequest", HandleAdminTakeQuest, SEC_ADMINISTRATOR, Console::Yes },
            { "boost",   HandleAdminBoost,   SEC_ADMINISTRATOR, Console::Yes },
            { "squad",   HandleAdminSquad,   SEC_ADMINISTRATOR, Console::Yes },
            { "gear",    HandleAdminGear,    SEC_ADMINISTRATOR, Console::Yes },
            { "spot",    HandleAdminSpot,    SEC_ADMINISTRATOR, Console::Yes },
            { "unlock",  HandleAdminUnlock,  SEC_ADMINISTRATOR, Console::Yes }
        };
        static ChatCommandTable gaTable =
        {
            { "info",    HandleInfo,   SEC_GAMEMASTER, Console::No },
            { "roster",  HandleRoster, SEC_GAMEMASTER, Console::No },
            { "renown",  renownTable },
            { "slots",   slotsTable },
            { "offline", offlineTable },
            { "bot",     botTable },
            { "admin",   adminTable },
            { "spec",    HandleSpec,   SEC_GAMEMASTER, Console::Yes },
            { "squad",   HandleSquad,  SEC_GAMEMASTER, Console::Yes },
            { "unlocks", HandleUnlocks, SEC_GAMEMASTER, Console::Yes },
            { "reload",  HandleReload, SEC_ADMINISTRATOR, Console::Yes }
        };
        static ChatCommandTable commandTable =
        {
            { "ga", gaTable }
        };
        return commandTable;
    }

    static bool HandleInfo(ChatHandler* handler)
    {
        AdventureGuild* guild = OwnGuild(handler);
        if (!guild)
            return false;

        std::string leader;
        sCharacterCache->GetCharacterNameByGuid(guild->leader, leader);
        handler->PSendSysMessage("Adventure guild of account {}: leader {}, WoW guild id {}, faction {}.", guild->ownerAccount, leader,
            guild->guildId, guild->team == TEAM_HORDE ? "Horde" : "Alliance");
        handler->PSendSysMessage("Renown: level {}, {} / {} in this level, {} in total.", guild->renownLevel,
            sProgressionService.GetPointsInLevel(*guild), sGuildAdventureMgr.GetRenownToNext(guild->renownLevel), guild->renownTotal);
        handler->PSendSysMessage("Slots: {} of {} used (max {}). Offline window: {} min.", guild->UsedSlots(), guild->unlockedSlots,
            GetSettings().maxSlots, guild->offlineWindowMin);

        std::string features;
        for (std::string const& feature : sProgressionService.GetFeatures(*guild))
            features += (features.empty() ? "" : ", ") + feature;
        handler->PSendSysMessage("Features: {}.", features.empty() ? "none" : features);
        handler->PSendSysMessage("Bot accounts: {}.", sGuildAdventureMgr.GetBotAccounts(guild->ownerAccount).size());
        return true;
    }

    static bool HandleRoster(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        AdventureGuild* guild = nullptr;
        if (target)
        {
            uint32 const account = sCharacterCache->GetCharacterAccountIdByGuid(target->GetGUID());
            guild = sGuildAdventureMgr.FindGuild(account);
            if (!guild)
                guild = sGuildAdventureMgr.FindGuildOfBot(target->GetGUID());
            if (!guild)
            {
                handler->SendErrorMessage(Acore::StringFormat("{} is not in an adventure guild.", target->GetName()), false);
                return false;
            }
        }
        else if (!(guild = OwnGuild(handler)))
            return false;

        handler->PSendSysMessage("Roster of account {} ({} of {} slots):", guild->ownerAccount, guild->UsedSlots(), guild->unlockedSlots);
        for (auto const& [guid, bot] : guild->bots)
        {
            std::string const zone = GetAreaName(bot.snapshot.zoneId);
            handler->PSendSysMessage("  [{}] {} - level {} race {} class {}, {}, order {}{}, {}", bot.slot, bot.name, bot.snapshot.level,
                bot.race, bot.classId, GetBotStateToken(*guild, bot), ToToken(bot.order.type),
                bot.order.zoneId ? Acore::StringFormat(" ({})", bot.order.zoneId) : std::string(), zone.empty() ? "?" : zone);
        }
        return true;
    }

    static bool HandleRenownAdd(ChatHandler* handler, int32 points)
    {
        AdventureGuild* guild = OwnGuild(handler);
        if (!guild)
            return false;

        sProgressionService.AddRenown(*guild, points);
        handler->PSendSysMessage("Renown {:+}: level {}, {} in total.", points, guild->renownLevel, guild->renownTotal);
        return true;
    }

    static bool HandleSlotsSet(ChatHandler* handler, uint8 slots)
    {
        AdventureGuild* guild = OwnGuild(handler);
        if (!guild)
            return false;

        sProgressionService.SetSlots(*guild, slots);
        handler->PSendSysMessage("Bot slots: {}.", guild->unlockedSlots);
        return true;
    }

    static bool HandleOfflineSet(ChatHandler* handler, uint16 minutes)
    {
        AdventureGuild* guild = OwnGuild(handler);
        if (!guild)
            return false;

        sProgressionService.SetOfflineWindow(*guild, minutes);
        handler->PSendSysMessage("Offline window: {} min.", guild->offlineWindowMin);
        return true;
    }

    static bool HandleBotRecruit(ChatHandler* handler, uint8 race, uint8 classId, uint8 gender, std::string name)
    {
        AdventureGuild* guild = OwnGuild(handler);
        if (!guild)
            return false;

        RecruitRequest request;
        request.race = race;
        request.classId = classId;
        request.gender = gender;
        request.name = name;

        uint32 const account = handler->GetSession()->GetAccountId();
        RosterResult const started = sRosterService.Recruit(*guild, request, [account, name](RosterResult const& result)
        {
            WorldSession* session = sWorldSessionMgr->FindSession(account);
            if (!session || !session->GetPlayer())
                return;
            ChatHandler chat(session);
            if (result)
                chat.PSendSysMessage("Guild Adventure: {} joined your guild.", name);
            else
                chat.SendErrorMessage(Acore::StringFormat("Guild Adventure: recruiting {} failed: {}", name, result.message), false);
        });

        if (!started)
        {
            handler->SendErrorMessage(Acore::StringFormat("{}: {}", ToToken(started.error), started.message), false);
            return false;
        }

        handler->PSendSysMessage("Recruiting {}...", name);
        return true;
    }

    static bool HandleBotOrder(ChatHandler* handler, PlayerIdentifier target, std::string orderToken, Optional<uint32> zoneId,
        Optional<uint32> count)
    {
        AdventureGuild* guild = OwnGuild(handler);
        if (guild && target.GetGUID() == guild->leader)
            return ApplyOrder(handler, *guild, guild->leaderPlan, orderToken, zoneId, count);
        GuildBot* bot = guild ? FindBot(handler, *guild, target) : nullptr;
        if (!bot)
            return false;
        return ApplyOrder(handler, *guild, *bot, orderToken, zoneId, count);
    }

    static bool HandleBotPlan(ChatHandler* handler, PlayerIdentifier target)
    {
        AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(target.GetGUID());
        if (!guild)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not a guild bot.", target.GetName()), false);
            return false;
        }
        for (std::string const& line : sOrderService.Describe(target.GetGUID()))
            handler->PSendSysMessage("{}", line);
        if (GuildBot const* bot = guild->FindBot(target.GetGUID()))
            handler->PSendSysMessage("{}", SpecLine(*bot));
        return true;
    }

    // "talents 0/0/5, template "Leveling: Shadow" (default of the class)"
    static std::string SpecLine(GuildBot const& bot)
    {
        Player* player = ObjectAccessor::FindPlayer(bot.guid);
        uint32 const assigned = sSpecService.GetAssigned(bot.guid);
        SpecTemplate const* spec = sSpecService.GetEffective(bot.guid, bot.classId);
        return Acore::StringFormat("talents {}, template {}{}", player ? SpecService::CurrentSummary(player) : std::string("?"),
            assigned == SpecManual ? std::string("none (manual)") : spec ? Acore::StringFormat("{} \"{}\"", spec->id, spec->name) : std::string("none for this class"),
            sSpecService.HasRunTemplate(bot.guid) ? " (of its dungeon role, for the run)" : assigned == SpecDefault && spec ? " (default of the class)" : "");
    }

    // The templates a guild character can play by, and the one it plays by.
    static bool HandleSpec(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        if (!target)
            target = PlayerIdentifier::FromTargetOrSelf(handler);
        if (!target)
        {
            handler->SendErrorMessage("Name a guild character.", false);
            return false;
        }
        AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfBot(target->GetGUID());
        GuildBot const* bot = guild ? guild->FindBot(target->GetGUID()) : nullptr;
        if (!bot)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not a guild bot.", target->GetName()), false);
            return false;
        }
        handler->PSendSysMessage("{}: {}", bot->name, SpecLine(*bot));
        uint8 const level = bot->snapshot.level ? bot->snapshot.level : 60;
        static char const* const purposes[] = { "leveling", "dungeon and raid", "raid", "PvP" };
        static char const* const roles[] = { "damage", "tank", "healer" };
        for (SpecTemplate const* spec : sSpecService.ListFor(guild->ownerAccount, bot->classId))
            handler->PSendSysMessage("  {} \"{}\" ({}, {}{}{}): {} at level {}", spec->id, spec->name, purposes[std::min<uint8>(spec->purpose, 3)],
                roles[std::min<uint8>(spec->role, 2)], spec->isDefault ? ", default" : "", spec->owner ? ", of the guild" : "",
                spec->Summary(level), uint32(level));
        return true;
    }

    // .ga squad [character]: the squads of the guild of that character (a guild bot or the leader; default: the target or
    // the GM himself), its hard quests and the quests its bots keep for a quest partner (QuestPartners.h).
    static bool HandleSquad(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        if (!target)
            target = PlayerIdentifier::FromTargetOrSelf(handler);
        AdventureGuild* guild = target ? sGuildAdventureMgr.FindGuildOfMember(target->GetGUID()) : nullptr;
        if (!guild)
        {
            handler->SendErrorMessage("Name a character of an adventure guild.", false);
            return false;
        }
        for (std::string const& line : sSquadService.DescribeGuild(*guild))
            handler->PSendSysMessage("{}", line);
        for (std::string const& line : sQuestPartners.DescribeGuild(*guild))
            handler->PSendSysMessage("{}", line);
        return true;
    }

    // .ga admin squad <bot> <leader | none>: the bot follows that guild bot of its guild as a squad member, or leaves its
    // squad (docs/orders.md "Squads").
    static bool HandleAdminSquad(ChatHandler* handler, PlayerIdentifier member, std::string leaderName)
    {
        AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfBot(member.GetGUID());
        if (!guild || !guild->FindBot(member.GetGUID()))
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not a guild bot.", member.GetName()), false);
            return false;
        }
        ObjectGuid leader;
        if (leaderName != "none" && leaderName != "NONE")
        {
            if (!normalizePlayerName(leaderName))
            {
                handler->SendErrorMessage("Name the squad leader (a guild bot) or \"none\".", false);
                return false;
            }
            leader = sCharacterCache->GetCharacterGuidByName(leaderName);
            if (leader.IsEmpty())
            {
                handler->SendErrorMessage(Acore::StringFormat("There is no character {}.", leaderName), false);
                return false;
            }
        }
        std::string error;
        uint32 const partnerQuest = sSquadService.GetPartnerQuest(member.GetGUID());
        if (!sSquadService.Set(*guild, member.GetGUID(), leader, error))
        {
            handler->SendErrorMessage(Acore::StringFormat("Not changed: {}.", error), false);
            return false;
        }
        if (partnerQuest && leader.IsEmpty())
            sQuestPartners.OnOwnerSplit(*guild, partnerQuest);
        GadvProtocol::Instance().PushBot(*guild, member.GetGUID(), false);
        if (!leader.IsEmpty())
            GadvProtocol::Instance().PushBot(*guild, leader, false);
        handler->PSendSysMessage(leader.IsEmpty() ? "{} is in no squad now." : "{} follows {} now.", member.GetName(), leaderName);
        return true;
    }

    // .ga admin spec <bot> <template id | default | manual>
    static bool HandleAdminSpec(ChatHandler* handler, PlayerIdentifier target, std::string choice)
    {
        AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfBot(target.GetGUID());
        GuildBot const* bot = guild ? guild->FindBot(target.GetGUID()) : nullptr;
        if (!bot)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not a guild bot.", target.GetName()), false);
            return false;
        }
        uint32 templateId = SpecDefault;
        if (choice == "manual")
            templateId = SpecManual;
        else if (choice != "default")
        {
            Optional<uint32> const id = Acore::StringTo<uint32>(choice);
            if (!id || *id < 2)
            {
                handler->SendErrorMessage("Give a template id, \"default\" or \"manual\".", false);
                return false;
            }
            templateId = *id;
        }
        std::string error;
        if (!sSpecService.Assign(guild->ownerAccount, bot->guid, bot->classId, templateId, sGuildBotHolder.GetAIDrivenPlayer(bot->guid), error))
        {
            handler->SendErrorMessage(Acore::StringFormat("{}: {}.", bot->name, error), false);
            return false;
        }
        handler->PSendSysMessage("{}: {}", bot->name, SpecLine(*bot));
        return true;
    }

    // .ga admin specsave <character> <purpose 0-3> <name>: the character's talents become a template of its guild.
    static bool HandleAdminSpecSave(ChatHandler* handler, PlayerIdentifier target, uint8 purpose, Tail name)
    {
        AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfBot(target.GetGUID());
        if (!guild)
            guild = sGuildAdventureMgr.FindGuildOfLeader(target.GetGUID());
        Player* player = ObjectAccessor::FindPlayer(target.GetGUID());
        if (!guild || !player)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not a guild character in the world.", target.GetName()), false);
            return false;
        }
        std::string error;
        uint32 const id = sSpecService.SaveFrom(guild->ownerAccount, player, std::string(name), purpose, error);
        if (!id)
        {
            handler->SendErrorMessage(Acore::StringFormat("Not saved: {}.", error), false);
            return false;
        }
        SpecTemplate const* spec = sSpecService.Get(id);
        handler->PSendSysMessage("Template {} \"{}\" saved: {} ({} points).", id, spec->name, spec->Summary(80), spec->points.size());
        return true;
    }

    // .ga admin specdelete <leader> <template id>
    static bool HandleAdminSpecDelete(ChatHandler* handler, PlayerIdentifier leader, uint32 templateId)
    {
        AdventureGuild* guild = GuildOfLeader(handler, leader);
        if (!guild)
            return false;
        std::string error;
        if (!sSpecService.Delete(guild->ownerAccount, templateId, error))
        {
            handler->SendErrorMessage(Acore::StringFormat("Not deleted: {}.", error), false);
            return false;
        }
        handler->PSendSysMessage("Template {} deleted.", templateId);
        return true;
    }

    static bool HandleAdminLogin(ChatHandler* handler, PlayerIdentifier leader)
    {
        AdventureGuild* guild = GuildOfLeader(handler, leader);
        if (!guild)
            return false;
        sRosterService.LoginActiveBots(*guild, nullptr);
        if (sRosterService.LoginPlannedLeader(*guild))
            handler->PSendSysMessage("Logging in the active guild bots of {} and {} itself (it has an order).",
                leader.GetName(), leader.GetName());
        else
            handler->PSendSysMessage("Logging in the active guild bots of {}.", leader.GetName());
        return true;
    }

    static bool HandleAdminLogout(ChatHandler* handler, PlayerIdentifier leader)
    {
        AdventureGuild* guild = GuildOfLeader(handler, leader);
        if (!guild)
            return false;
        uint32 count = 0;
        for (auto const& [guid, bot] : guild->bots)
            if (sGuildBotHolder.IsAIDriven(guid) && sGuildBotHolder.Logout(guid))
                ++count;
        handler->PSendSysMessage("{} guild bots of {} logged out.", count, leader.GetName());
        return true;
    }

    // (DUNGEON takes the dungeon's id and, optionally, how many runs; FARM the item's id and, optionally, the target
    // stock.)
    static bool HandleAdminOrder(ChatHandler* handler, PlayerIdentifier target, std::string orderToken, Optional<uint32> zoneId,
        Optional<uint32> count)
    {
        if (AdventureGuild* led = sGuildAdventureMgr.FindGuildOfLeader(target.GetGUID()))
            return ApplyOrder(handler, *led, led->leaderPlan, orderToken, zoneId, count);
        AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfBot(target.GetGUID());
        GuildBot* bot = guild ? guild->FindBot(target.GetGUID()) : nullptr;
        if (!bot)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not a guild bot.", target.GetName()), false);
            return false;
        }
        return ApplyOrder(handler, *guild, *bot, orderToken, zoneId, count);
    }

    // Test aid for the order planner's death rules: kills an AI-driven guild bot where it stands.
    // Debugging the order planner's zone hubs: per team the hub of a zone, and the zone and region its spot lies in.
    // .ga admin dungeon [<dungeon id>]: the dungeons a run can be ordered into, or one with its entrance and bosses.
    static bool HandleAdminDungeon(ChatHandler* handler, Optional<uint32> dungeonId)
    {
        for (std::string const& line : sDungeons.Describe(dungeonId ? *dungeonId : 0))
            handler->SendSysMessage(line);
        return true;
    }

    static bool HandleAdminHub(ChatHandler* handler, uint32 zoneId)
    {
        ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(zoneId);
        if (!zone)
        {
            handler->SendErrorMessage(Acore::StringFormat("Zone {} is not in ga_zone_template.", zoneId), false);
            return false;
        }
        for (TeamId team : { TEAM_ALLIANCE, TEAM_HORDE })
        {
            char const* const teamName = team == TEAM_HORDE ? "Horde" : "Alliance";
            ZoneHub const* hub = sWorldIndex.GetHub(zoneId, team);
            if (!hub)
            {
                handler->PSendSysMessage("{} ({}): no hub", zone->name, teamName);
                continue;
            }
            uint32 spotZone = 0, spotArea = 0;
            if (Map* map = sMapMgr->FindBaseNonInstanceMap(hub->mapId))
                map->GetZoneAndAreaId(PHASEMASK_NORMAL, spotZone, spotArea, hub->x, hub->y, hub->z);
            handler->PSendSysMessage("{} ({}): hub {} at ({:.0f}, {:.0f}) map {}, in {} ({}), region {}{}", zone->name, teamName,
                hub->name, hub->x, hub->y, hub->mapId, GetAreaName(spotZone), spotZone,
                ToString(GetRegionAt(hub->mapId, hub->x, hub->y, hub->z)), spotZone == zoneId ? "" : " - OUTSIDE THE ZONE");
        }
        return true;
    }

    // .ga admin quests [forget | scripted | <quest id>]: the quests AI-played characters leave alone (QuestSupport) and the quest
    // events the planner plays (QuestEvents). "forget" drops every block the planner learned, per bot and for everyone:
    // for use after the planner got new abilities. "scripted" lists the zone quests up to level 60 that are left alone
    // as script events. With a quest id: what the planner makes of that quest.
    static bool HandleAdminQuests(ChatHandler* handler, Optional<std::string> action)
    {
        if (action && *action == "forget")
        {
            uint32 const rows = sQuestSupport.ForgetLearned();
            handler->PSendSysMessage("Forgot {} quest blocks of the guild bots and every learned quest.", rows);
            return true;
        }
        if (action && *action == "scripted")
        {
            for (std::string const& line : sQuestSupport.DescribeScripted(60))
                handler->SendSysMessage(line);
            return true;
        }
        if (action)
        {
            Optional<uint32> const questId = Acore::StringTo<uint32>(*action);
            if (!questId || !sObjectMgr->GetQuestTemplate(*questId))
            {
                handler->SendErrorMessage("Usage: .ga admin quests [forget | scripted | <quest id>]", false);
                return false;
            }
            QuestVerdict const verdict = sQuestSupport.GetVerdict(*questId);
            for (std::string const& line : sQuestEvents.Describe(*questId))
                handler->SendSysMessage(line);
            handler->PSendSysMessage("  verdict: {}{}{}", ToString(verdict), verdict == QuestVerdict::Supported ? "" : ": ",
                sQuestSupport.GetReason(*questId));
            if (uint32 const escort = sQuestSupport.GetEscortNpc(*questId))
                handler->PSendSysMessage("  escort: the bot follows creature {}", escort);
            return true;
        }
        for (std::string const& line : sQuestSupport.Describe())
            handler->SendSysMessage(line);
        for (std::string const& line : sQuestEvents.Describe())
            handler->SendSysMessage(line);
        return true;
    }

    // .ga admin path <character> <x> <y> <z> [<from x> <from y> [<from z>]]: the path the core would move that
    // character along, from where it stands (or from the given spot; without a height, from the surface there) to the
    // point, and how much of it leaves the ground (PathProbe).
    // The way on the whole navmesh of a dungeon map from the character to a point (DungeonNav): its length, its drops, or
    // why there is none.
    static bool HandleAdminNavWay(ChatHandler* handler, PlayerIdentifier target, float x, float y, float z, Optional<float> fromX,
        Optional<float> fromY, Optional<float> fromZ)
    {
        Player* player = target.GetConnectedPlayer();
        if (!player || !player->IsInWorld())
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not in the world.", target.GetName()), false);
            return false;
        }
        // (From a point of the character's map instead of where it stands.)
        float const sx = fromX ? *fromX : player->GetPositionX();
        float const sy = fromY ? *fromY : player->GetPositionY();
        float const sz = fromZ ? *fromZ : player->GetPositionZ();
        std::vector<GuildAdventure::NavPoint> corners;
        float length = 0.0f;
        std::string why;
        if (!sDungeonNav.FindWay(player->GetMapId(), sx, sy, sz, x, y, z, &corners, &length, player->GetMap(), &why))
        {
            handler->PSendSysMessage("no way from ({:.1f}, {:.1f}, {:.1f}) to ({:.1f}, {:.1f}, {:.1f}) on map {}: {}", sx, sy, sz, x, y, z,
                player->GetMapId(), why);
            return true;
        }
        uint32 drops = 0;
        std::string dropText;
        for (std::size_t i = 1; i < corners.size(); ++i)
            if (corners[i].drop)
            {
                ++drops;
                dropText += Acore::StringFormat("; a drop of {:.0f} yd from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f})",
                    corners[i - 1].z - corners[i].z, corners[i - 1].x, corners[i - 1].y, corners[i - 1].z, corners[i].x, corners[i].y, corners[i].z);
            }
        handler->PSendSysMessage("a way of {:.0f} yd ({:.0f} yd straight), {} corners, {} drops{}", length,
            std::sqrt((x - sx) * (x - sx) + (y - sy) * (y - sy) + (z - sz) * (z - sz)), corners.size(), drops, dropText);
        return true;
    }

    static bool HandleAdminPath(ChatHandler* handler, PlayerIdentifier target, float x, float y, float z, Optional<float> fromX,
        Optional<float> fromY, Optional<float> fromZ)
    {
        Player* player = target.GetConnectedPlayer();
        if (!player || !player->IsInWorld())
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not in the world.", target.GetName()), false);
            return false;
        }

        float startX = player->GetPositionX(), startY = player->GetPositionY(), startZ = player->GetPositionZ();
        if (fromX && fromY)
        {
            startX = *fromX;
            startY = *fromY;
            startZ = fromZ ? *fromZ : player->GetMap()->GetHeight(player->GetPhaseMask(), startX, startY, MAX_HEIGHT, true, MAX_HEIGHT);
        }
        handler->PSendSysMessage("from ({:.1f}, {:.1f}, {:.1f}) to ({:.1f}, {:.1f}, {:.1f}) on map {}", startX, startY, startZ, x, y, z,
            player->GetMapId());
        for (std::string const& line : Describe(ProbePath(player, startX, startY, startZ, x, y, z)))
            handler->SendSysMessage(line);
        return true;
    }

    // .ga admin boost <character> <level> [rare | epic]: test aid for runs at a level no test bot has reached yet. Sets the
    // level of an AI-played character and gives it what a character that levelled there has: bags, uncommon gear of that
    // level (rare or epic for a test raid: what a guild brings from the dungeons), ammunition, some money. Class spells,
    // weapon skills, riding and talents follow from the level-up as in play.
    static bool HandleAdminBoost(ChatHandler* handler, PlayerIdentifier target, uint8 level, Optional<std::string> quality)
    {
        Player* player = target.GetConnectedPlayer();
        if (!player || !player->IsInWorld() || !BotDriver::GetAI(player) || !level || level > sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not an AI-played character in the world, or the level is out of range.",
                target.GetName()), false);
            return false;
        }
        uint8 const old = player->GetLevel();
        if (level != old)
        {
            player->GiveLevel(level);
            player->InitTalentForLevel();
            player->SetUInt32Value(PLAYER_XP, 0);
        }
        bool const geared = BotDriver::EquipForLevel(player, !quality ? 2 : *quality == "epic" ? 4 : *quality == "rare" ? 3 : 2);
        // (And what a character of that level has: its class spells and its talent points along its template. The
        // Deadmines group boosted from 1 to 20 had 10 to 21 spells and 3 to 8 talents, and was wiped out twice.)
        uint32 const spells = sOrderService.LearnClassSpellsNow(player);
        std::string const talents = sSpecService.Maintain(player);
        player->ModifyMoney(int32(level) * int32(level) * 100);
        player->SetFullHealth();
        // (And full mana: a priest boosted from 1 to 60 kept the mana of level 1 and began its raid at 2 %.)
        if (player->getPowerType() == POWER_MANA)
            player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
        handler->PSendSysMessage("{}: level {} -> {}, {}, {} class spells learned, talents: {}, {} copper added.", player->GetName(),
            uint32(old), uint32(level), !geared ? "no gear (no bot AI)" : quality && (*quality == "rare" || *quality == "epic") ?
            "bags and " + *quality + " gear for the level" : std::string("bags and gear for the level"), spells,
            talents.empty() ? std::string("as they were") : talents, uint32(level) * uint32(level) * 100);
        return true;
    }

    // .ga admin takequest <character> <quest id>: test aid for quest events. With a giver of the quest within 100 yd
    // the character accepts it there like a client (first call: it is put next to the giver; second call: the accept,
    // with every script that reacts to it); else the quest is put into its log directly, and no such script runs.
    static bool HandleAdminTakeQuest(ChatHandler* handler, PlayerIdentifier target, uint32 questId)
    {
        Player* player = target.GetConnectedPlayer();
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!player || !player->IsInWorld() || !quest)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not in the world, or there is no quest {}.", target.GetName(), questId),
                false);
            return false;
        }
        if (player->GetQuestStatus(questId) != QUEST_STATUS_NONE || !player->CanAddQuest(quest, false))
        {
            handler->SendErrorMessage(Acore::StringFormat("{} has quest {} already, or no room for it.", player->GetName(), questId), false);
            return false;
        }

        // With one of the quest's givers within 100 yd the quest is accepted from it like a client does, so scripts that
        // react to the accept run (an escort sets off, an event starts). The character is put next to it first.
        Creature* giver = nullptr;
        if (QuestRelations const* relations = sObjectMgr->GetCreatureQuestRelationMap())
            for (auto const& [entry, relatedQuest] : *relations)
                if (relatedQuest == questId)
                    if (Creature* found = player->FindNearestCreature(entry, 100.0f, true))
                        if (!giver || player->GetDistance(found) < player->GetDistance(giver))
                            giver = found;
        if (giver && !player->IsWithinDistInMap(giver, 4.0f))
        {
            player->NearTeleportTo(giver->GetPositionX() + 1.5f, giver->GetPositionY(), giver->GetPositionZ() + 0.3f, player->GetOrientation());
            handler->PSendSysMessage("{} is put next to {}; run the command again to accept quest {} from it.", player->GetName(),
                giver->GetName(), questId);
            return true;
        }
        if (giver)
        {
            WorldPacket packet(CMSG_QUESTGIVER_ACCEPT_QUEST, 16);
            packet << giver->GetGUID() << uint32(questId) << uint32(0);
            player->GetSession()->HandleQuestgiverAcceptQuestOpcode(packet);
            if (player->GetQuestStatus(questId) != QUEST_STATUS_NONE)
            {
                handler->PSendSysMessage("{} accepted quest {} \"{}\" from {}.", player->GetName(), questId, quest->GetTitle(),
                    giver->GetName());
                return true;
            }
            handler->PSendSysMessage("{} did not give quest {} (a prerequisite, the level, the race or class); it is put into the log "
                "without it.", giver->GetName(), questId);
        }
        player->AddQuestAndCheckCompletion(quest, nullptr);
        handler->PSendSysMessage("{} has quest {} \"{}\" in its log now (no quest giver involved).", player->GetName(), questId,
            quest->GetTitle());
        return true;
    }

    // .ga admin walk <character> <x> <y> <z>: starts the point movement playerbots uses for a walk (a generated path,
    // no forced destination) for an AI-played character. Test aid for the ground guard (core patch CP-10).
    static bool HandleAdminWalk(ChatHandler* handler, PlayerIdentifier target, float x, float y, float z)
    {
        Player* player = target.GetConnectedPlayer();
        if (!player || !player->IsInWorld() || !BotDriver::GetAI(player))
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not an AI-played character in the world.", target.GetName()), false);
            return false;
        }
        player->GetMotionMaster()->Clear();
        player->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, true, false);
        handler->PSendSysMessage("{} at ({:.1f}, {:.1f}, {:.1f}): point movement to ({:.1f}, {:.1f}, {:.1f}) started; ground guard: {} "
            "checked, {} cancelled so far", player->GetName(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
            x, y, z, sGroundGuard.GetChecked(), sGroundGuard.GetRefused());
        return true;
    }

    // .ga admin route <character> <x> <y> <z> [<level>] [<from x> <from y>]: the route of the road map that character
    // would walk to the point (from where it stands, or from the given spot), at its own or the given level.
    static bool HandleAdminRoute(ChatHandler* handler, PlayerIdentifier target, float x, float y, float z, Optional<uint32> level,
        Optional<float> fromX, Optional<float> fromY)
    {
        Player* player = target.GetConnectedPlayer();
        if (!player || !player->IsInWorld())
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not in the world.", target.GetName()), false);
            return false;
        }
        float const startX = fromX && fromY ? *fromX : player->GetPositionX();
        float const startY = fromX && fromY ? *fromY : player->GetPositionY();
        uint8 const walkerLevel = uint8(level ? std::min<uint32>(*level, 80) : player->GetLevel());
        // As a bot's walk gets it: the walks to the first and from the last node checked with the path finder.
        float const startZ = fromX && fromY ? player->GetMap()->GetHeight(player->GetPhaseMask(), startX, startY, MAX_HEIGHT) :
            player->GetPositionZ();
        RoadMap::LegCheck const legs = [player](RoutePoint const& from, RoutePoint const& to)
        {
            return WalkReaches(player, from.x, from.y, from.z, to.x, to.y, to.z);
        };
        // And around the groups the character cannot take (the risk model's answer for it, as for its walks).
        RoadMap::CrowdCheck const takes = [player](uint8 count, uint8 crowdLevel)
        {
            return sOrderService.CanTakeGroup(player, crowdLevel, count);
        };
        RoadRoute const route = sRoadMap.FindRoute(player->GetMapId(), startX, startY, startZ, x, y, z, player->GetTeamId(), walkerLevel,
            legs, {}, takes);
        handler->PSendSysMessage("road map: {} nodes, {} links; from ({:.0f}, {:.0f}) to ({:.0f}, {:.0f}) on map {} at level {}: "
            "{:.0f} yd straight, strongest hostile creatures on the straight line level {}", sRoadMap.GetNodeCount(),
            sRoadMap.GetLinkCount(), startX, startY, x, y, player->GetMapId(), uint32(walkerLevel), route.direct,
            uint32(sRoadMap.DangerAlong(player->GetMapId(), startX, startY, x, y, player->GetTeamId())));
        if (!route.found)
        {
            handler->SendSysMessage("no route (no node near the start or the destination, or no chain of links between them)");
            return true;
        }
        handler->PSendSysMessage("route: {:.0f} yd over {} nodes, {} points; strongest hostile creatures on it level {}; biggest group "
            "on it that {} cannot take: {} (up to level {})", route.length, route.nodes, route.points.size(), uint32(route.danger),
            player->GetName(), uint32(route.crowd), uint32(route.crowdLevel));
        handler->PSendSysMessage("via {}", route.via);
        // Without a look at groups, and for a walker that can take no group of three or more.
        RoadRoute const plain = sRoadMap.FindRoute(player->GetMapId(), startX, startY, startZ, x, y, z, player->GetTeamId(), walkerLevel, legs);
        if (plain.found)
            handler->PSendSysMessage("without a look at groups: {:.0f} yd over {} nodes, biggest group {} (up to level {}), via {}",
                plain.length, plain.nodes, uint32(plain.crowd), uint32(plain.crowdLevel), plain.via);
        RoadRoute const around = sRoadMap.FindRoute(player->GetMapId(), startX, startY, startZ, x, y, z, player->GetTeamId(), walkerLevel,
            legs, {}, [](uint8, uint8) { return false; });
        if (around.found)
            handler->PSendSysMessage("around every group: {:.0f} yd over {} nodes, biggest group left {} (up to level {}), via {}",
                around.length, around.nodes, uint32(around.crowd), uint32(around.crowdLevel), around.via);
        return true;
    }

    static bool HandleAdminKill(ChatHandler* handler, PlayerIdentifier target)
    {
        Player* player = sGuildBotHolder.GetAIDrivenPlayer(target.GetGUID());
        if (!player)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not an AI-driven guild bot in the world.", target.GetName()), false);
            return false;
        }
        if (!player->IsAlive())
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is dead already.", target.GetName()), false);
            return false;
        }
        Unit::Kill(player, player);
        handler->PSendSysMessage("{} killed at ({:.1f}, {:.1f}) map {}.", target.GetName(), player->GetPositionX(),
            player->GetPositionY(), player->GetMapId());
        return true;
    }

    // Test aid: an AI-driven guild character casts a spell on itself at once (a form, a buff), so a test can put it
    // into a state it would otherwise have to play itself into.
    static bool HandleAdminSelfCast(ChatHandler* handler, PlayerIdentifier target, uint32 spellId)
    {
        Player* player = sGuildBotHolder.GetAIDrivenPlayer(target.GetGUID());
        if (!player || !player->IsAlive())
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not a living AI-driven guild bot in the world.", target.GetName()), false);
            return false;
        }
        if (!sSpellMgr->GetSpellInfo(spellId))
        {
            handler->SendErrorMessage(Acore::StringFormat("There is no spell {}.", spellId), false);
            return false;
        }
        player->CastSpell(player, spellId, true);
        handler->PSendSysMessage("{} cast spell {} on itself (form now {}).", target.GetName(), spellId,
            uint32(player->GetShapeshiftForm()));
        return true;
    }

    // Test aid: the AI of a guild character turns to the nearest living creature of an entry within 60 yards, as
    // playerbots does when it picks a target (what the pull check then makes of it is in the activity log).
    static bool HandleAdminAttack(ChatHandler* handler, PlayerIdentifier target, uint32 entry)
    {
        Player* player = sGuildBotHolder.GetAIDrivenPlayer(target.GetGUID());
        if (!player || !player->IsAlive())
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not a living AI-driven guild bot in the world.", target.GetName()), false);
            return false;
        }
        Creature* creature = player->FindNearestCreature(entry, 60.0f, true);
        if (!creature)
        {
            handler->SendErrorMessage(Acore::StringFormat("No living creature {} within 60 yd of {}.", entry, target.GetName()), false);
            return false;
        }
        bool const done = BotDriver::Attack(player, creature);
        handler->PSendSysMessage("{} {} {} (level {}, {:.0f} yd away).", target.GetName(), done ? "turns to" : "cannot attack",
            creature->GetName(), uint32(creature->GetLevel()), player->GetExactDist(creature));
        return done;
    }

    static bool HandleAdminRecruit(ChatHandler* handler, PlayerIdentifier leader, uint8 race, uint8 classId, uint8 gender,
        std::string name)
    {
        AdventureGuild* guild = GuildOfLeader(handler, leader);
        if (!guild)
            return false;

        RecruitRequest request;
        request.race = race;
        request.classId = classId;
        request.gender = gender;
        request.name = name;
        RosterResult const started = sRosterService.Recruit(*guild, request, [name](RosterResult const& result)
        {
            if (result)
                LOG_INFO("module", "mod-guild-adventure: .ga admin recruit: {} joined the guild", name);
            else
                LOG_ERROR("module", "mod-guild-adventure: .ga admin recruit: {} failed: {}", name, result.message);
        });
        if (!started)
        {
            handler->SendErrorMessage(Acore::StringFormat("{}: {}", ToToken(started.error), started.message), false);
            return false;
        }
        handler->PSendSysMessage("Recruiting {} for {} (result in Server.log).", name, leader.GetName());
        return true;
    }

    // ".ga admin farm <character> <item id>": where the character would farm the item (Farming.h): the creatures that
    // drop it, the ones at or below its level limit, the best hunting grounds with the risk model's verdict, or why it
    // has none. Reads only.
    static bool HandleAdminFarm(ChatHandler* handler, PlayerIdentifier target, uint32 item)
    {
        Player* player = target.GetConnectedPlayer();
        if (!player)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not in the world.", target.GetName()), false);
            return false;
        }
        for (std::string const& line : sOrderService.DescribeFarm(player, item))
            handler->SendSysMessage(line);
        return true;
    }

    // ".ga admin spot <character> <x> <y> <z> [<creature entry>]": how the risk model judges a spot for the character
    // (the target, the hostile creatures that would join, what the fight would cost it, whether it would work there).
    static bool HandleAdminSpot(ChatHandler* handler, PlayerIdentifier target, float x, float y, float z, Optional<uint32> entry)
    {
        Player* player = target.GetConnectedPlayer();
        if (!player)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not in the world.", target.GetName()), false);
            return false;
        }
        handler->SendSysMessage(sOrderService.DescribeSpot(player, x, y, z, entry.value_or(0)));
        return true;
    }

    // ".ga admin gear <character> [<yards> | buy]": what the character may spend on a piece of gear now and what the
    // vendors near it sell that it would buy (GearUpkeep.h); "buy" (test aid) makes it buy at the vendor it stands at.
    static bool HandleAdminGear(ChatHandler* handler, PlayerIdentifier target, Optional<Variant<uint32, EXACT_SEQUENCE("buy")>> arg)
    {
        Player* player = target.GetConnectedPlayer();
        if (!player)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not in the world.", target.GetName()), false);
            return false;
        }
        if (arg && arg->holds_alternative<EXACT_SEQUENCE("buy")>())
        {
            handler->SendSysMessage(Gear::BuyHere(player));
            return true;
        }
        Optional<uint32> yards;
        if (arg)
            yards = arg->get<uint32>();
        for (std::string const& line : Gear::Describe(player, float(yards.value_or(uint32(sBotSettings.GetFloat(player, Setting::GearBuyNearYards))))))
            handler->SendSysMessage(line);
        return true;
    }

    static bool HandleAdminOffline(ChatHandler* handler, PlayerIdentifier leader, uint16 minutes)
    {
        AdventureGuild* guild = GuildOfLeader(handler, leader);
        if (!guild)
            return false;
        sProgressionService.SetOfflineWindow(*guild, minutes);
        handler->PSendSysMessage("Offline window of {}: {} min.", leader.GetName(), guild->offlineWindowMin);
        return true;
    }

    static bool SetState(ChatHandler* handler, PlayerIdentifier const& target, BotState state)
    {
        AdventureGuild* guild = OwnGuild(handler);
        if (!guild || !FindBot(handler, *guild, target))
            return false;

        RosterResult const result = sRosterService.SetBotState(*guild, target.GetGUID(), state);
        if (!result)
        {
            handler->SendErrorMessage(result.message, false);
            return false;
        }

        PushBot(*guild, target.GetGUID(), false);
        PushGuildState(*guild);
        handler->PSendSysMessage("{} is {} now.", target.GetName(), ToString(state));
        return true;
    }

    static bool HandleBotBench(ChatHandler* handler, PlayerIdentifier target)
    {
        return SetState(handler, target, BotState::Benched);
    }

    static bool HandleBotActivate(ChatHandler* handler, PlayerIdentifier target)
    {
        return SetState(handler, target, BotState::Active);
    }

    // Dismissing a bot (the owner, 2026-11-28; RosterService::Dismiss). It cannot be undone, so it wants the word "confirm"
    // after the name; without it the command says what would happen.
    static bool Dismiss(ChatHandler* handler, AdventureGuild* guild, PlayerIdentifier const& target, Optional<std::string> const& confirm)
    {
        if (!guild)
            return false;
        GuildBot const* bot = guild->FindBot(target.GetGUID());
        if (!bot)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is no bot of that adventure guild.", target.GetName()), false);
            return false;
        }
        if (!confirm || !StringEqualI(*confirm, "confirm"))
        {
            handler->PSendSysMessage("Dismissing {} deletes the character for good: what the guild storage takes of its items (nothing "
                "soulbound) goes there and its gold into the guild bank; the guild's renown and unlocks stay. It cannot be undone: "
                "add \"confirm\" after the name to do it.", bot->name);
            return true;
        }
        RosterResult const result = sRosterService.Dismiss(*guild, target.GetGUID(), nullptr);
        if (!result)
        {
            handler->SendErrorMessage(result.message, false);
            return false;
        }
        handler->SendSysMessage(result.message);
        return true;
    }

    static bool HandleBotDismiss(ChatHandler* handler, PlayerIdentifier target, Optional<std::string> confirm)
    {
        return Dismiss(handler, OwnGuild(handler), target, confirm);
    }

    // The same from the console or SOAP: the bot names its guild (test aid).
    static bool HandleAdminDismiss(ChatHandler* handler, PlayerIdentifier target, Optional<std::string> confirm)
    {
        AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfBot(target.GetGUID());
        if (!guild)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is no guild bot.", target.GetName()), false);
            return false;
        }
        return Dismiss(handler, guild, target, confirm);
    }

    static bool HandleUnlocks(ChatHandler* handler, Optional<PlayerIdentifier> leader)
    {
        AdventureGuild* guild = nullptr;
        if (leader)
            guild = GuildOfLeader(handler, *leader);
        else if (handler->GetSession())
            guild = OwnGuild(handler);
        else
            handler->SendErrorMessage("From the console name the guild leader: .ga unlocks <leader>.", false);
        if (!guild)
            return false;

        handler->PSendSysMessage("Unlocks of the adventure guild of account {} (renown level {}, {} bot slots, offline window {} min):",
            guild->ownerAccount, guild->renownLevel, guild->unlockedSlots, guild->offlineWindowMin);
        for (UnlockTemplate const& unlock : sGuildAdventureMgr.GetUnlocks())
        {
            UnlockState const state = sProgressionService.GetUnlockState(*guild, unlock);
            handler->PSendSysMessage("  {:>2}  renown {}  {:<9}  {}{}", unlock.id, unlock.renownLevel, ToToken(state), unlock.description,
                unlock.goldCost ? Acore::StringFormat(" ({})", GuildBudget::MoneyText(uint32(unlock.goldCost))) : std::string());
        }
        handler->PSendSysMessage("Paid from: {} (bot setting Unlocks.PayFrom); it can pay {} now.", ToToken(sProgressionService.GetPayFrom(*guild)),
            GuildBudget::MoneyText(uint32(std::min<uint64>(sProgressionService.GetUnlockFunds(*guild), UINT32_MAX))));
        return true;
    }

    static bool HandleAdminUnlock(ChatHandler* handler, PlayerIdentifier leader, uint32 unlockId)
    {
        AdventureGuild* guild = GuildOfLeader(handler, leader);
        if (!guild)
            return false;
        UnlockTemplate const* unlock = sGuildAdventureMgr.GetUnlock(unlockId);
        if (!unlock)
        {
            handler->SendErrorMessage(Acore::StringFormat("There is no unlock {} (.ga unlocks lists them).", unlockId), false);
            return false;
        }
        UnlockBuyResult const result = sProgressionService.BuyUnlock(*guild, *unlock, "an administrator's command");
        if (!result.ok)
        {
            handler->SendErrorMessage(Acore::StringFormat("{}: {}", result.code, result.message), false);
            return false;
        }
        handler->PSendSysMessage("{} Bot slots now {}, offline window {} min.", result.message, guild->unlockedSlots, guild->offlineWindowMin);
        return true;
    }

    static bool HandleReload(ChatHandler* handler)
    {
        sGuildAdventureMgr.LoadTemplates();
        sWorldIndex.Build();
        sRoadMap.Load();
        sProfessions.Load();
        sFarming.Load();
        sQuestSupport.Load();
        for (auto& [owner, guild] : sGuildAdventureMgr.GetGuilds())
            sProgressionService.ApplyUnlocks(guild);
        handler->PSendSysMessage("Guild Adventure templates reloaded: {} zones (version {}), {} unlocks; world index {} spawns.",
            sGuildAdventureMgr.GetZones().size(), sGuildAdventureMgr.GetZoneVersion(), sGuildAdventureMgr.GetUnlocks().size(),
            sWorldIndex.GetSpawnCount());
        return true;
    }
};

void AddGuildAdventureManageCommandScripts()
{
    new GuildAdventureManageCommandScript();
}
