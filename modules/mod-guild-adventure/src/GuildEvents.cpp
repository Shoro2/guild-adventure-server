/*
 * GuildEvents implementation. See GuildEvents.h.
 */

#include "GuildEvents.h"
#include "GadvProtocol.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "Log.h"
#include "OfflineService.h"

using namespace GuildAdventure;

uint32 GuildAdventure::RecordEvent(AdventureGuild& guild, std::string const& type, ObjectGuid bot, std::string const& message,
    uint8 level, uint32 zoneId, int32 points)
{
    GuildEvent event;
    event.type = type;
    event.bot = bot;
    event.message = message;
    event.level = level;
    event.zoneId = zoneId;
    event.points = points;

    GuildEvent const& stored = sGuildAdventureMgr.AddEvent(guild, std::move(event));
    sOfflineService.OnEvent(guild, stored);
    sGadvProtocol.PushEvent(guild, stored);

    if (GetSettings().debug)
        LOG_INFO("module", "mod-guild-adventure: event {} [{}] owner {}: {}", stored.id, stored.type, guild.ownerAccount, stored.message);
    return stored.id;
}

void GuildAdventure::PushGuildState(AdventureGuild const& guild)
{
    sGadvProtocol.PushState(guild);
}

void GuildAdventure::PushBot(AdventureGuild const& guild, ObjectGuid bot, bool full)
{
    sGadvProtocol.PushBot(guild, bot, full);
}

void GuildAdventure::PushBotRemoved(AdventureGuild const& guild, ObjectGuid bot)
{
    sGadvProtocol.PushBotRemoved(guild, bot);
}

void GuildAdventure::PushUnlocks(AdventureGuild const& guild)
{
    sGadvProtocol.PushUnlocks(guild);
}

void GuildAdventure::PushNotice(uint32 ownerAccount, std::string const& message, bool warning)
{
    sGadvProtocol.PushNotice(ownerAccount, message, warning);
}
