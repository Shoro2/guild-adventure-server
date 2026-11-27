/*
 * The GADV part of the guild perks (docs/protocol.md §7.15, PerkService). Members of GadvProtocol, kept apart from
 * GadvProtocol.cpp.
 */

#include "GadvProtocol.h"
#include "GuildAdventureMgr.h"
#include "PerkService.h"

using namespace GuildAdventure;
using GuildAdventure::Gadv::Message;

void GadvProtocol::SendPerks(uint32 accountId, AdventureGuild const& guild)
{
    std::vector<Message> records;
    for (PerkTemplate const& perk : sPerkService.GetTemplates())
    {
        uint8 const rank = sPerkService.GetRank(guild.ownerAccount, perk.id);
        uint8 const max = perk.MaxRank();
        bool const more = rank < max;
        records.push_back(Message("PERK")
            .Set("id", perk.id)
            .Set("key", perk.key)
            .Set("n", perk.name)
            .Set("rank", uint32(rank))
            .Set("max", uint32(max))
            .Set("cost", more ? perk.costs[rank] : 0u)
            .Set("unit", perk.unit)
            .Set("desc", rank ? perk.Describe(rank) : std::string())
            .Set("next", more ? perk.Describe(rank + 1) : std::string())
            .Set("avail", PerkService::IsAvailable(perk.kind)));
    }
    SendBlock(accountId, "PERKS", records, Message("PERKS_BEGIN"));
}

void GadvProtocol::PushPerks(AdventureGuild const& guild)
{
    if (FindConnectedLink(guild.ownerAccount))
        SendPerks(guild.ownerAccount, guild);
}

void GadvProtocol::OnPerkBuy(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = message.GetInt("r");
    std::optional<int64> const id = message.GetInt("id");
    if (!id || *id <= 0)
    {
        Error(accountId, r, "BAD_REQUEST", "PERK_BUY needs a perk id.");
        return;
    }
    std::string code;
    std::string text;
    if (!sPerkService.Buy(guild, uint32(*id), code, text))
    {
        Error(accountId, r, code, text);
        return;
    }
    // The STATE (points) and PERKS pushes went out in Buy; the ACK comes after the record pushes (protocol §4).
    Ack(accountId, r);
}
