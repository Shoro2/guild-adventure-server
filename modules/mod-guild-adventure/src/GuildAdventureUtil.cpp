/*
 * Small helpers shared by the module's services. See GuildAdventureUtil.h.
 */

#include "GuildAdventureUtil.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Player.h"
#include "StringFormat.h"

uint32 GuildAdventure::Now()
{
    return uint32(GameTime::GetGameTime().count());
}

std::string GuildAdventure::GetAreaName(uint32 areaId)
{
    if (!areaId)
        return {};

    AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
    if (!area)
        return {};

    // The protocol sends English text; the DBC strings of the enUS client data are in slot 0.
    char const* name = area->area_name[LOCALE_enUS];
    return name ? name : "";
}

std::string GuildAdventure::ProfessionSkillSummary(Player const* player)
{
    std::string skills;
    if (!player)
        return skills;
    auto add = [&](uint16 skill)
    {
        if (!player->HasSkill(skill))
            return;
        if (!skills.empty())
            skills += ',';
        skills += Acore::StringFormat("{}:{}:{}", skill, player->GetPureSkillValue(skill), player->GetPureMaxSkillValue(skill));
    };
    for (uint16 skill : PrimaryProfessionSkills)
        add(skill);
    for (uint16 skill : SecondaryProfessionSkills)
        add(skill);
    return skills;
}
