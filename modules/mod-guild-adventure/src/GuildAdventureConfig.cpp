/*
 * Module-wide configuration. See GuildAdventureConfig.h.
 */

#include "GuildAdventureConfig.h"
#include "Config.h"
#include "ControlSwapService.h"
#include "Log.h"

namespace
{
    bool sEnabled = false;
}

void GuildAdventure::LoadModuleConfig()
{
    sEnabled = sConfigMgr->GetOption<bool>("GuildAdventure.Enable", true);
    sControlSwapService.LoadConfig();
}

bool GuildAdventure::IsModuleEnabled()
{
    return sEnabled;
}

void GuildAdventure::CheckPlayerbotsSettings()
{
    // A PlayerbotMgr is created for every character the client controls; with autologin it would add every
    // character of the session's account as an altbot (PlayerbotMgr.cpp:1741-1763).
    if (sConfigMgr->GetOption<bool>("Playerbots.BotAutologin", false))
        LOG_WARN("module", "mod-guild-adventure: Playerbots.BotAutologin = 1 breaks take control; set it to 0.");

    // Above 2 a new PlayerbotMgr turns its player into a self bot.
    if (sConfigMgr->GetOption<int32>("Playerbots.SelfBotLevel", 1) > 2)
        LOG_WARN("module", "mod-guild-adventure: Playerbots.SelfBotLevel > 2 turns controlled guild bots into self bots; use 2 or less.");

    // A character handed to the AI leaves a group that does not contain its master (PlayerbotMgr.cpp:529-563).
    if (!sConfigMgr->GetOption<bool>("Playerbots.KeepAltsInGroup", false))
        LOG_WARN("module", "mod-guild-adventure: Playerbots.KeepAltsInGroup = 0; a character handed to the AI may leave its group. Recommended: 1.");
}
