/*
 * ".ga" GM commands (the design log §2.13). This file holds the commands of the take-control prototype (spike S2a):
 *
 *   .ga bot login <name>   log a character in as a guild bot of your account (you become its master);
 *                          administrators only, and never a character of another adventure guild (its leader or one
 *                          of its roster bots): a guild bot logged in this way can be taken over with .ga swap
 *   .ga bot logout <name>  log an AI-driven guild bot of yours out
 *   .ga bot list           your guild bots and their state
 *   .ga swap <name> [relog|reload|switch|auto]
 *                          take control of one of your guild bots; relog = Path R (character screen,
 *                          docs/take-control.md §5.2), reload = Path W (loading screen only, experiment, §5.8),
 *                          switch = Path S (no loading screen, needs the client DLL, near targets only, §5.11),
 *                          auto = Path S, else Path W where possible; without a path GuildAdventure.Swap.Path
 *                          applies
 *   .ga watch [on|off]     watch mode (docs/take-control.md §5.12): the AI plays the guild bot you control with its
 *                          order and your client watches; off (or .ga watch again) gives you the controls back
 *   .ga status             the state of your swap, your guild bots and your client DLL
 *   .ga client             what the take-control client DLL (Path S) announced on your connection; without a DLL
 *                          on record it also announces the channel again, for a DLL loaded after the character list
 *   .ga client ping        measure a round trip to that DLL (core patch CP-5, docs/take-control.md §5.10)
 *
 * All commands act on the account of the GM who types them (the "owner"); none runs from the console.
 */

#include "Chat.h"
#include "ClientChannel.h"
#include "CommandScript.h"
#include "ControlSwapService.h"
#include "CharacterCache.h"
#include "GuildAdventureConfig.h"
#include "GuildAdventureMgr.h"
#include "GuildBotHolder.h"
#include "Player.h"
#include "WorldSession.h"
#include <algorithm>
#include <cctype>

using namespace Acore::ChatCommands;

namespace
{
    bool CheckEnabled(ChatHandler* handler)
    {
        if (GuildAdventure::IsModuleEnabled())
            return true;

        handler->SendErrorMessage("Guild Adventure is disabled (GuildAdventure.Enable = 0).", false);
        return false;
    }
}

class GuildAdventureCommandScript : public CommandScript
{
public:
    GuildAdventureCommandScript() : CommandScript("GuildAdventureCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable botCommandTable =
        {
            // Administrators only: the command logs in a character of any account, and .ga swap then plays it.
            { "login",  HandleBotLoginCommand,  SEC_ADMINISTRATOR, Console::No },
            { "logout", HandleBotLogoutCommand, SEC_GAMEMASTER, Console::No },
            { "list",   HandleBotListCommand,   SEC_GAMEMASTER, Console::No }
        };

        static ChatCommandTable clientCommandTable =
        {
            { "ping", HandleClientPingCommand,   SEC_GAMEMASTER, Console::No },
            { "",     HandleClientStatusCommand, SEC_GAMEMASTER, Console::No }
        };

        static ChatCommandTable gaCommandTable =
        {
            { "bot",    botCommandTable },
            { "client", clientCommandTable },
            { "swap",   HandleSwapCommand,   SEC_GAMEMASTER, Console::No },
            { "watch",  HandleWatchCommand,  SEC_GAMEMASTER, Console::No },
            { "status", HandleStatusCommand, SEC_GAMEMASTER, Console::No }
        };

        static ChatCommandTable commandTable =
        {
            { "ga", gaCommandTable }
        };

        return commandTable;
    }

    static bool HandleBotLoginCommand(ChatHandler* handler, PlayerIdentifier target)
    {
        if (!CheckEnabled(handler))
            return false;

        WorldSession* session = handler->GetSession();
        Player* self = session->GetPlayer();
        if (target.GetGUID() == self->GetGUID())
        {
            handler->SendErrorMessage("You play that character yourself.", false);
            return false;
        }

        // Never a character another owner's adventure guild holds: its leader, one of its roster bots, or any
        // character of that owner's account.
        uint32 const account = session->GetAccountId();
        uint32 const characterAccount = sCharacterCache->GetCharacterAccountIdByGuid(target.GetGUID());
        GuildAdventure::AdventureGuild const* other = sGuildAdventureMgr.FindGuildOfBot(target.GetGUID());
        if (!other)
            other = sGuildAdventureMgr.FindGuildOfLeader(target.GetGUID());
        if (!other && characterAccount != account)
            other = sGuildAdventureMgr.FindGuild(characterAccount);
        if (other && other->ownerAccount != account)
        {
            handler->SendErrorMessage(Acore::StringFormat("{} belongs to another player's adventure guild.",
                target.GetName()), false);
            return false;
        }

        GuildAdventure::LoginResult const result = sGuildBotHolder.Login(target.GetGUID(), account, self->GetGUID());
        if (result != GuildAdventure::LoginResult::Started)
        {
            handler->SendErrorMessage(Acore::StringFormat("{}: {}.", target.GetName(), GuildAdventure::ToString(result)), false);
            return false;
        }

        handler->PSendSysMessage("Logging in {} as a guild bot...", target.GetName());
        return true;
    }

    static bool HandleBotLogoutCommand(ChatHandler* handler, PlayerIdentifier target)
    {
        WorldSession* session = handler->GetSession();
        if (sGuildBotHolder.GetOwner(target.GetGUID()) != session->GetAccountId())
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not one of your guild bots.", target.GetName()), false);
            return false;
        }

        if (!sGuildBotHolder.IsAIDriven(target.GetGUID()))
        {
            handler->SendErrorMessage(Acore::StringFormat("{} is not driven by the AI right now (loading or controlled).", target.GetName()), false);
            return false;
        }

        if (!sGuildBotHolder.Logout(target.GetGUID()))
        {
            handler->SendErrorMessage(Acore::StringFormat("{} could not be logged out.", target.GetName()), false);
            return false;
        }

        handler->PSendSysMessage("{} logged out.", target.GetName());
        return true;
    }

    static bool HandleBotListCommand(ChatHandler* handler)
    {
        WorldSession* session = handler->GetSession();
        std::vector<GuildAdventure::ManagedInfo> const bots = sGuildBotHolder.List(session->GetAccountId());
        if (bots.empty())
        {
            handler->SendSysMessage("You have no guild bots in the world. Use .ga bot login <name>.");
            return true;
        }

        handler->PSendSysMessage("Your guild bots ({}):", bots.size());
        for (GuildAdventure::ManagedInfo const& bot : bots)
        {
            bool const leader = bot.characterAccountId == bot.ownerAccountId;
            handler->PSendSysMessage("  {} - {}{}{} (account {})", bot.name, GuildAdventure::ToString(bot.state),
                bot.watched ? ", watch mode (the AI plays it)" : "", leader ? ", your own account" : "", bot.characterAccountId);
        }
        return true;
    }

    static bool HandleSwapCommand(ChatHandler* handler, PlayerIdentifier target, Optional<std::string> pathText)
    {
        if (!CheckEnabled(handler))
            return false;

        std::optional<GuildAdventure::SwapPathMode> mode;
        if (pathText)
        {
            mode = GuildAdventure::ParseSwapPathMode(*pathText);
            if (!mode)
            {
                handler->SendErrorMessage(Acore::StringFormat("Unknown path '{}'; use relog, reload, switch or auto.", *pathText), false);
                return false;
            }
        }

        GuildAdventure::SwapRequestResult const result = sControlSwapService.RequestSwap(handler->GetSession(), target.GetGUID(), mode);
        if (!result)
        {
            handler->SendErrorMessage(Acore::StringFormat("Cannot switch to {}: {} ({}).", target.GetName(), result.message,
                GuildAdventure::ToString(result.refusal)), false);
            return false;
        }

        if (result.path == GuildAdventure::SwapPath::Switch)
            handler->PSendSysMessage("Switch to {} requested (in-world switch, no loading screen).", target.GetName());
        else if (result.path == GuildAdventure::SwapPath::Reload)
            handler->PSendSysMessage("Switch to {} requested (world reload). A loading screen follows; you come back as {}.",
                target.GetName(), target.GetName());
        else
            handler->PSendSysMessage("Switch to {} requested. You return to the character screen; pick {} there and enter the world.",
                target.GetName(), target.GetName());
        return true;
    }

    static bool HandleWatchCommand(ChatHandler* handler, Optional<std::string> state)
    {
        if (!CheckEnabled(handler))
            return false;

        WorldSession* session = handler->GetSession();
        Player* player = session->GetPlayer();
        if (!player)
            return false;

        bool on = !sGuildBotHolder.IsWatched(player->GetGUID());
        if (state)
        {
            std::string text = *state;
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (text == "on" || text == "1")
                on = true;
            else if (text == "off" || text == "0")
                on = false;
            else
            {
                handler->SendErrorMessage(Acore::StringFormat("Unknown state '{}'; use on or off.", *state), false);
                return false;
            }
        }

        bool const was = sGuildBotHolder.IsWatched(player->GetGUID());
        GuildAdventure::SwapRequestResult const result = sControlSwapService.RequestWatch(session, on);
        if (!result)
        {
            handler->SendErrorMessage(Acore::StringFormat("Cannot watch {}: {} ({}).", player->GetName(), result.message,
                GuildAdventure::ToString(result.refusal)), false);
            return false;
        }

        if (on && was)
            handler->PSendSysMessage("Watch mode is already on: the AI plays {}.", player->GetName());
        else if (on)
            handler->PSendSysMessage("Watch mode on: the AI plays {} with its order, you watch. .ga watch off gives you the "
                "controls back.", player->GetName());
        else if (was)
            handler->PSendSysMessage("Watch mode off: you control {} again.", player->GetName());
        else
            handler->PSendSysMessage("Watch mode is off: you control {}.", player->GetName());
        return true;
    }

    static bool HandleStatusCommand(ChatHandler* handler)
    {
        WorldSession* session = handler->GetSession();
        uint32 const account = session->GetAccountId();

        handler->PSendSysMessage("Guild Adventure: module {}.", GuildAdventure::IsModuleEnabled() ? "enabled" : "disabled");
        handler->PSendSysMessage("Take control: {}.", sControlSwapService.DescribeState(account));

        std::vector<GuildAdventure::ManagedInfo> const bots = sGuildBotHolder.List(account);
        uint32 aiDriven = 0;
        uint32 controlled = 0;
        uint32 watched = 0;
        for (GuildAdventure::ManagedInfo const& bot : bots)
        {
            if (bot.state == GuildAdventure::ManagedState::Bot)
                ++aiDriven;
            else if (bot.state == GuildAdventure::ManagedState::Controlled)
                ++controlled;
            if (bot.watched)
                ++watched;
        }

        handler->PSendSysMessage("Guild bots: {} in total, {} AI-driven, {} controlled by you ({} in watch mode).", bots.size(),
            aiDriven, controlled, watched);
        handler->PSendSysMessage("Client DLL: {}.", sClientChannel.DescribeState(session));
        return true;
    }

    static bool HandleClientStatusCommand(ChatHandler* handler)
    {
        WorldSession* session = handler->GetSession();
        handler->PSendSysMessage("Client DLL: {}.", sClientChannel.DescribeState(session));

        // A DLL that was loaded after the character screen has not seen an ANNOUNCE yet; give it one.
        if (!sClientChannel.GetClientDll(session) && GuildAdventure::IsModuleEnabled())
        {
            sClientChannel.Announce(session);
            handler->SendSysMessage("Channel announced; a client DLL answers with HELLO (see .ga client).");
        }
        return true;
    }

    static bool HandleClientPingCommand(ChatHandler* handler)
    {
        if (!CheckEnabled(handler))
            return false;

        WorldSession* session = handler->GetSession();
        switch (sClientChannel.SendPing(session, session->GetAccountId()))
        {
            case GuildAdventure::PingResult::Sent:
                handler->SendSysMessage("Ping sent to your client DLL; the round trip follows when it answers.");
                return true;
            case GuildAdventure::PingResult::NoClientDll:
                handler->SendErrorMessage("Your client has no DLL that said HELLO on this connection.", false);
                return false;
            case GuildAdventure::PingResult::NotSupported:
                handler->SendErrorMessage("Your client DLL does not announce ping support.", false);
                return false;
            case GuildAdventure::PingResult::Outstanding:
                handler->SendErrorMessage("The previous ping is still unanswered; try again in a few seconds.", false);
                return false;
        }
        return false;
    }
};

void AddGuildAdventureCommandScripts()
{
    new GuildAdventureCommandScript();
}
