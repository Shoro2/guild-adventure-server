/*
 * The GADV part of the squads (docs/protocol.md "Squads", docs/orders.md "Squads"). Members of GadvProtocol, kept apart
 * from GadvProtocol.cpp.
 */

#include "GadvProtocol.h"
#include "GuildAdventureMgr.h"
#include "QuestPartners.h"
#include "Squads.h"
#include "StringFormat.h"

using namespace GuildAdventure;
using GuildAdventure::Gadv::Message;

// SQUAD_SET g, l: the bot g follows the guild bot l as a member of its squad; l = 0 takes it out of its squad.
void GadvProtocol::OnSquadSet(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = message.GetInt("r");
    std::optional<int64> const g = message.GetInt("g");
    std::optional<int64> const l = message.GetInt("l");
    ObjectGuid const member = g ? ObjectGuid::Create<HighGuid::Player>(uint32(*g)) : ObjectGuid::Empty;
    if (!g || !guild.FindBot(member))
    {
        Error(accountId, r, "NOT_OWNER", "That character is not one of your guild bots.");
        return;
    }
    ObjectGuid const leader = l && *l > 0 ? ObjectGuid::Create<HighGuid::Player>(uint32(*l)) : ObjectGuid::Empty;
    ObjectGuid const before = sSquadService.GetLeader(member);
    uint32 const partnerQuest = sSquadService.GetPartnerQuest(member);
    std::string error;
    if (!sSquadService.Set(guild, member, leader, error))
    {
        Error(accountId, r, "SQUAD_INVALID", Acore::StringFormat("Not changed: {}.", error));
        return;
    }
    // The owner took a quest partner out of its squad: that quest does not pair it up again at once (QuestPartners.h).
    if (partnerQuest && leader.IsEmpty())
        sQuestPartners.OnOwnerSplit(guild, partnerQuest);
    PushBot(guild, member, false);
    if (!leader.IsEmpty())
        PushBot(guild, leader, false);
    if (!before.IsEmpty() && before != leader)
        PushBot(guild, before, false);
    Ack(accountId, r);
}
