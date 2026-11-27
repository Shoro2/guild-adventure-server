/*
 * The GADV part of the tiered guild achievements (docs/protocol.md §7.18, MilestoneService). Members of GadvProtocol,
 * kept apart from GadvProtocol.cpp.
 */

#include "GadvProtocol.h"
#include "GuildAdventureMgr.h"
#include "MilestoneService.h"

using namespace GuildAdventure;
using GuildAdventure::Gadv::Message;

namespace
{
    // How the addon groups the series.
    char const* GroupOf(MilestoneKind kind)
    {
        switch (kind)
        {
            case MilestoneKind::LevelClass: return "class";
            case MilestoneKind::LevelRace:  return "race";
            case MilestoneKind::Skill:      return "prof";
            case MilestoneKind::Kills:      return "kills";
            default:                        return "guild";
        }
    }
}

void GadvProtocol::SendMilestones(uint32 accountId, AdventureGuild const& guild)
{
    std::vector<Message> records;
    for (MilestoneSeries const& series : sMilestoneService.GetSeries())
    {
        std::string thresholds;
        std::string points;
        uint32 reached = 0;
        for (std::size_t i = 0; i < series.tiers.size(); ++i)
        {
            thresholds += (i ? "," : "") + std::to_string(series.tiers[i].threshold);
            points += (i ? "," : "") + std::to_string(sMilestoneService.GetTierPoints(series, i));
            if (sMilestoneService.HasEarned(guild, series.tiers[i].id))
                ++reached;
        }
        records.push_back(Message("MILESTONE")
            .Set("s", series.key)
            .Set("k", ToToken(series.kind))
            .Set("grp", GroupOf(series.kind))
            .Set("sub", series.subject)
            .Set("n", series.name)
            .Set("v", sMilestoneService.GetValue(guild, series))
            .Set("th", thresholds)
            .Set("pt", points)
            .Set("act", uint32(sMilestoneService.GetActiveTiers(series)))
            .Set("got", reached));
    }
    SendBlock(accountId, "MILESTONES", records, Message("MILESTONES_BEGIN"));
}

void GadvProtocol::PushMilestones(AdventureGuild const& guild)
{
    if (FindConnectedLink(guild.ownerAccount))
        SendMilestones(guild.ownerAccount, guild);
}
