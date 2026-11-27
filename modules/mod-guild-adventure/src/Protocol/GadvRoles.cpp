/*
 * The GADV part of the dungeon roles (docs/protocol.md "Dungeon roles", docs/dungeon-roles.md). Members of
 * GadvProtocol, kept apart from GadvProtocol.cpp.
 */

#include "GadvProtocol.h"
#include "BotSettings.h"
#include "StorageService.h"
#include <optional>
#include "DungeonRoles.h"
#include "DungeonSupplies.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "ObjectMgr.h"
#include "OrderService.h"
#include "SpecService.h"
#include "StringFormat.h"

using namespace GuildAdventure;
using GuildAdventure::Gadv::Message;

// ROLE_SET g, role ("none", "tank", "heal"), t: the dungeon role of a bot and the talent template of the role
// (0 = the default of its class for the role).
void GadvProtocol::OnRoleSet(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = message.GetInt("r");
    std::optional<int64> const g = message.GetInt("g");
    std::optional<int64> const t = message.GetInt("t");
    std::string const token = message.GetString("role");
    ObjectGuid const guid = g ? ObjectGuid::Create<HighGuid::Player>(uint32(*g)) : ObjectGuid::Empty;
    GuildBot const* bot = g ? guild.FindBot(guid) : nullptr;
    if (!bot)
    {
        Error(accountId, r, "NOT_OWNER", "That character is not one of your guild bots.");
        return;
    }
    DungeonRole role = DungeonRole::None;
    if (token == "tank")
        role = DungeonRole::Tank;
    else if (token == "heal")
        role = DungeonRole::Healer;
    else if (token != "none")
    {
        Error(accountId, r, "BAD_REQUEST", "The role is none, tank or heal.");
        return;
    }
    std::string error;
    if (!sRoleService.SetRole(guild, *bot, role, uint32(std::max<int64>(t.value_or(0), 0)), error))
    {
        Error(accountId, r, "ROLE_INVALID", Acore::StringFormat("Not set: {}.", error));
        return;
    }
    PushBot(guild, guid, false);
    Ack(accountId, r);
}

// GET what=rolegear g: block ROLEGEAR, the role store of a bot. One record per piece: k 1 = a piece of its dungeon
// set, 2 = a levelling piece put aside while the dungeon set is on (s = the equipment slot it was worn in).
void GadvProtocol::SendRoleGear(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const g = message.GetInt("g");
    ObjectGuid const guid = g ? ObjectGuid::Create<HighGuid::Player>(uint32(*g)) : ObjectGuid::Empty;
    if (!g || !guild.FindBot(guid))
        return;
    std::vector<Message> records;
    if (std::vector<RolePiece> const* store = sRoleService.GetStore(guid))
        for (RolePiece const& piece : *store)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(piece.entry);
            if (!proto)
                continue;
            records.push_back(Message("RG").Set("g", guid.GetCounter()).Set("id", piece.id).Set("k", uint32(piece.kind))
                .Set("s", uint32(piece.slot)).Set("e", piece.entry).Set("rp", piece.randomProperty).Set("n", proto->Name1)
                .Set("q", proto->Quality).Set("il", proto->ItemLevel).Set("it", proto->InventoryType));
        }
    SendBlock(accountId, "ROLEGEAR", records, Message("ROLEGEAR_BEGIN").Set("g", guid.GetCounter()));
}

// DUNGEON_START d, bots (list of guids), runs (0 = until it is stopped), lead (optional): the run window's start. The
// bots become a group for the run (OrderService::StartDungeonGroup) and are taken into the dungeon.
void GadvProtocol::OnDungeonStart(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = message.GetInt("r");
    std::optional<int64> const d = message.GetInt("d");
    std::optional<std::vector<int64>> const list = message.GetIntList("bots");
    if (!d || !list || list->empty())
    {
        Error(accountId, r, "BAD_REQUEST", "A dungeon and the bots of the group are needed.");
        return;
    }
    std::vector<ObjectGuid> bots;
    for (int64 g : *list)
        bots.push_back(ObjectGuid::Create<HighGuid::Player>(uint32(std::max<int64>(g, 0))));
    int64 const asked = message.GetInt("runs").value_or(1);
    uint32 const runs = asked == 0 ? OrderService::DungeonRunsEndless :
        uint32(std::clamp<int64>(asked, 1, int64(GetSettings().dungeonsMaxRuns)));
    std::optional<int64> const lead = message.GetInt("lead");
    // The preparation step: items (the kinds of items of the guild storage the group may use), and the fight rules
    // tl (seconds the others wait for the tank), em (the healer's emergency limit, percent), cpon / cp (careful pulls,
    // and from how many creatures), ff (fleeing creatures are focused). Rules that are not sent are the server's.
    // (ia: the addon sends what the leader took out, `noitems`; every other kind is used. Without it `items` lists
    // what an older addon ticked; with neither every kind is used.)
    OrderService::DungeonRunOptions options;
    if (std::optional<std::vector<int64>> const items = message.GetIntList("items"); items && !message.GetBool("ia").value_or(false))
    {
        options.listed = true;
        for (int64 entry : *items)
            if (entry > 0)
                options.items.push_back(uint32(entry));
    }
    if (std::optional<std::vector<int64>> const out = message.GetIntList("noitems"))
        for (int64 entry : *out)
            if (entry > 0)
                options.excluded.push_back(uint32(entry));
    if (message.GetInt("tl") || message.GetInt("em") || message.GetInt("cp") || message.GetBool("cpon") || message.GetBool("ff") ||
        message.GetInt("md") || message.GetInt("gr") || message.GetInt("gc"))
    {
        options.hasRules = true;
        options.rules = OrderService::DefaultFightRules();
        options.rules.tankLeadSeconds = uint32(std::max<int64>(message.GetInt("tl").value_or(options.rules.tankLeadSeconds), 0));
        options.rules.emergencyHealth = uint32(std::max<int64>(message.GetInt("em").value_or(options.rules.emergencyHealth), 0));
        options.rules.carefulPullPack = uint32(std::max<int64>(message.GetInt("cp").value_or(options.rules.carefulPullPack), 2));
        options.rules.carefulPulls = message.GetBool("cpon").value_or(options.rules.carefulPulls);
        options.rules.focusFleeing = message.GetBool("ff").value_or(options.rules.focusFleeing);
        // (md: the mode, 0 the bosses, 1 the rooms, 2 the rooms in big pulls; gr, gc: a big pull's yards and creatures.)
        options.rules.mode = uint8(std::clamp<int64>(message.GetInt("md").value_or(options.rules.mode), 0, OrderService::ModeTribute));
        options.rules.gatherYards = uint32(std::clamp<int64>(message.GetInt("gr").value_or(options.rules.gatherYards), 10, 40));
        options.rules.gatherCount = uint32(std::clamp<int64>(message.GetInt("gc").value_or(options.rules.gatherCount), 2, 15));
    }
    std::string error;
    if (!sOrderService.StartDungeonGroup(guild, uint32(std::max<int64>(*d, 0)), bots,
            lead && *lead > 0 ? ObjectGuid::Create<HighGuid::Player>(uint32(*lead)) : ObjectGuid::Empty, runs, error, true, &options))
    {
        Error(accountId, r, "DUNGEON_INVALID", Acore::StringFormat("Not started: {}.", error));
        return;
    }
    for (ObjectGuid bot : bots)
        PushBot(guild, bot, false);
    SendDungeonRuns(accountId, guild, true);
    Ack(accountId, r);
}

// DUNGEON_STOP g (the bot that leads the run), now: the run ends at once, or the run that is going is the last.
void GadvProtocol::OnDungeonStop(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = message.GetInt("r");
    std::optional<int64> const g = message.GetInt("g");
    std::string error;
    if (!g || !sOrderService.StopDungeonRun(guild, ObjectGuid::Create<HighGuid::Player>(uint32(std::max<int64>(*g, 0))),
            message.GetBool("now").value_or(false), error))
    {
        Error(accountId, r, "DUNGEON_INVALID", Acore::StringFormat("Not stopped: {}.", error.empty() ? "no such run" : error));
        return;
    }
    SendDungeonRuns(accountId, guild, true);
    Ack(accountId, r);
}

// Block DRUNS: one record per dungeon run of the guild. g the bot that leads it, d / dn the dungeon, ph 0 not started,
// 1 getting ready, 2 inside; b bosses down of `of`, nb the boss the group is after, w wipes, left the runs the order
// still has (the one that is going included; 255 = until it is stopped), done, min the minutes of this run, m the
// other bots of the group, why the leader's why line.
void GadvProtocol::SendDungeonRuns(uint32 accountId, AdventureGuild& guild, bool force)
{
    std::vector<Message> records;
    std::string text;
    for (OrderService::DungeonRunStatus const& run : sOrderService.GetDungeonRunStatus(guild))
    {
        std::string members;
        for (ObjectGuid member : run.members)
            members += (members.empty() ? "" : ",") + std::to_string(member.GetCounter());
        Message record("DRUN");
        record.Set("g", run.leader.GetCounter()).Set("d", run.dungeon).Set("dn", run.name).Set("ph", uint32(run.phase))
            .Set("b", uint32(run.bossesDown)).Set("of", uint32(run.bosses)).Set("nb", run.nextBoss).Set("w", uint32(run.wipes))
            .Set("left", run.runsLeft).Set("done", run.runsDone).Set("min", run.minutes).Set("m", members).Set("why", run.why);
        // (The minutes are left out of what counts as a change: the block would go out every minute for nothing.)
        text += Acore::StringFormat("{}|{}|{}|{}|{}|{}|{}|{}|{}|{};", run.leader.GetCounter(), run.dungeon, uint32(run.phase),
            uint32(run.bossesDown), run.nextBoss, uint32(run.wipes), run.runsLeft, run.runsDone, members, run.why);
        records.push_back(std::move(record));
    }
    Link& link = _links[accountId];
    // A dungeon the guild finished: the dungeons again (what is open now).
    if (link.sentDungeonProgress != sOrderService.GetDungeonProgressVersion(guild))
        SendDungeons(accountId);
    if (!force && link.sentDungeonRuns == text)
        return;
    link.sentDungeonRuns = text;
    SendBlock(accountId, "DRUNS", records, Message("DRUNS_BEGIN"));
}

// GET what=dungeonprep: block DPREP. DPREP_BEGIN carries the server's fight rules (tl, em, cp, cpon, ff: the defaults
// of the preparation step); one DITEM record per kind of item in the guild storage that a run can use: e the item,
// n its name, q its quality, k 1 elixir or flask, 2 scroll, 3 stone or oil for a weapon, 4 scope; amt in the storage,
// lvl the level it asks for.
void GadvProtocol::SendDungeonPrep(uint32 accountId, AdventureGuild& guild)
{
    std::vector<Message> records;
    // What a run may use of the guild storage: its reserves too while Storage.ReserveForDungeons is on.
    std::optional<StorageService::OwnerOrder> intoReserve;
    if (sBotSettings.GetForGuild(guild.ownerAccount, Setting::StorageReserveForDungeons))
        intoReserve.emplace();
    for (SupplyItem const& supply : DungeonSupplies::List(guild.ownerAccount))
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(supply.entry);
        if (!proto)
            continue;
        records.push_back(Message("DITEM").Set("e", supply.entry).Set("n", proto->Name1).Set("q", proto->Quality)
            .Set("k", uint32(supply.kind)).Set("amt", supply.amount).Set("lvl", proto->RequiredLevel));
    }
    OrderService::DungeonFightRules const rules = OrderService::DefaultFightRules();
    SendBlock(accountId, "DPREP", records, Message("DPREP_BEGIN").Set("tl", rules.tankLeadSeconds).Set("em", rules.emergencyHealth)
        .Set("cp", rules.carefulPullPack).Set("cpon", rules.carefulPulls).Set("ff", rules.focusFleeing).Set("md", uint32(rules.mode))
        .Set("gr", rules.gatherYards).Set("gc", rules.gatherCount));
}
