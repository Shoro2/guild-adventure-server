/*
 * Small helpers shared by the module's services.
 */

#ifndef MOD_GUILD_ADVENTURE_UTIL_H
#define MOD_GUILD_ADVENTURE_UTIL_H

#include "Define.h"
#include <string>

class Player;

namespace GuildAdventure
{
    // The profession skill lines the protocol knows (docs/protocol.md section 8.7).
    inline constexpr uint16 PrimaryProfessionSkills[] = { 164, 165, 171, 182, 186, 197, 202, 333, 393, 755, 773 };
    inline constexpr uint16 SecondaryProfessionSkills[] = { 129, 185, 356 };

    // Unix seconds (server clock), the time base of the protocol and the module tables.
    uint32 Now();
    // English AreaTable name of a zone or area id, empty if unknown.
    std::string GetAreaName(uint32 areaId);
    // The professions a character has, "skillId:value:max,..." (primary ones first; the BOT record's sk).
    std::string ProfessionSkillSummary(Player const* player);
}

#endif
