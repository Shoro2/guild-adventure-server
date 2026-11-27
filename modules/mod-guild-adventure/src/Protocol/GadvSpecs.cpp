/*
 * The GADV part of the talent templates (docs/protocol.md "Talent templates", docs/specs.md). Members of GadvProtocol,
 * kept apart from GadvProtocol.cpp.
 */

#include "GadvProtocol.h"
#include "GuildAdventureMgr.h"
#include "GuildBotHolder.h"
#include "Player.h"
#include "ProgressionService.h"
#include "SpecService.h"
#include "StringFormat.h"
#include <algorithm>

using namespace GuildAdventure;
using GuildAdventure::Gadv::Message;

namespace
{
    std::optional<int64> RequestOf(Message const& message)
    {
        return message.GetInt("r");
    }
}

// Block SPECS: every built-in template and the guild's own. `sum` is the points per tree at level 60.
void GadvProtocol::SendSpecs(uint32 accountId)
{
    std::vector<Message> records;
    for (uint8 classId = 1; classId < MAX_CLASSES; ++classId)
        for (SpecTemplate const* spec : sSpecService.ListFor(accountId, classId))
            records.push_back(Message("SPEC").Set("id", spec->id).Set("class", uint32(spec->classId)).Set("n", spec->name)
                .Set("p", uint32(spec->purpose)).Set("ro", uint32(spec->role)).Set("t", uint32(spec->tab))
                .Set("d", spec->isDefault).Set("own", spec->owner != 0).Set("sum", spec->Summary(60)));
    SendBlock(accountId, "SPECS", records, Message("SPECS_BEGIN"));
}

// SPEC_SET g, t: the template a bot plays by (0 = default of its class, 1 = manual).
void GadvProtocol::OnSpecSet(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = RequestOf(message);
    std::optional<int64> const g = message.GetInt("g");
    std::optional<int64> const t = message.GetInt("t");
    ObjectGuid const guid = g ? ObjectGuid::Create<HighGuid::Player>(uint32(*g)) : ObjectGuid::Empty;
    GuildBot const* bot = g ? guild.FindBot(guid) : nullptr;
    if (!bot || !t || *t < 0)
    {
        Error(accountId, r, bot ? "BAD_REQUEST" : "NOT_OWNER", bot ? "No template given." : "That character is not one of your guild bots.");
        return;
    }
    if (!sProgressionService.HasFeature(guild, "talents"))
    {
        Error(accountId, r, "LOCKED", "Talent templates are not unlocked yet.");
        return;
    }
    std::string error;
    if (!sSpecService.Assign(guild.ownerAccount, guid, bot->classId, uint32(*t), sGuildBotHolder.GetAIDrivenPlayer(guid), error))
    {
        Error(accountId, r, "SPEC_INVALID", Acore::StringFormat("Not assigned: {}.", error));
        return;
    }
    PushBot(guild, guid, false);
    Ack(accountId, r);
}

// SPEC_SAVE n, p: the talents of the character the owner plays become a template of the guild.
void GadvProtocol::OnSpecSave(uint32 accountId, Player* player, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = RequestOf(message);
    std::string const name = message.GetString("n");
    std::optional<int64> const purpose = message.GetInt("p");
    if (name.empty() || !sProgressionService.HasFeature(guild, "talents"))
    {
        Error(accountId, r, name.empty() ? "BAD_REQUEST" : "LOCKED",
            name.empty() ? "A template needs a name." : "Talent templates are not unlocked yet.");
        return;
    }
    std::string error;
    if (!sSpecService.SaveFrom(guild.ownerAccount, player, name, uint8(std::clamp<int64>(purpose.value_or(0), 0, 3)), error))
    {
        Error(accountId, r, "SPEC_INVALID", Acore::StringFormat("Not saved: {}.", error));
        return;
    }
    SendSpecs(accountId);
    Ack(accountId, r);
}

// SPEC_DEL t: deletes a template of the guild; its bots go back to the default of their class.
void GadvProtocol::OnSpecDelete(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = RequestOf(message);
    std::optional<int64> const t = message.GetInt("t");
    std::string error;
    if (!t || *t < 0 || !sSpecService.Delete(guild.ownerAccount, uint32(*t), error))
    {
        Error(accountId, r, "SPEC_INVALID", Acore::StringFormat("Not deleted: {}.", t ? error : std::string("no template given")));
        return;
    }
    SendSpecs(accountId);
    for (auto const& [guid, bot] : guild.bots)
        PushBot(guild, guid, false);
    Ack(accountId, r);
}
