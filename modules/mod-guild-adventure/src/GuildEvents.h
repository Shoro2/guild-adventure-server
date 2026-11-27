/*
 * GuildEvents: the one place where something that happened to an adventure guild is recorded and announced.
 * RecordEvent stores the event (event log), feeds the "while you were away" summary and pushes it to the owner's
 * addon; the Push* helpers forward record changes to the addon without an event. World thread only.
 */

#ifndef MOD_GUILD_ADVENTURE_GUILD_EVENTS_H
#define MOD_GUILD_ADVENTURE_GUILD_EVENTS_H

#include "ObjectGuid.h"
#include <string>

namespace GuildAdventure
{
    struct AdventureGuild;

    // type: protocol token (docs/protocol.md section 8.3). Returns the event id.
    uint32 RecordEvent(AdventureGuild& guild, std::string const& type, ObjectGuid bot, std::string const& message,
        uint8 level = 0, uint32 zoneId = 0, int32 points = 0);

    void PushGuildState(AdventureGuild const& guild);
    void PushBot(AdventureGuild const& guild, ObjectGuid bot, bool full);
    void PushBotRemoved(AdventureGuild const& guild, ObjectGuid bot);
    void PushUnlocks(AdventureGuild const& guild);
    void PushNotice(uint32 ownerAccount, std::string const& message, bool warning);
}

#endif
