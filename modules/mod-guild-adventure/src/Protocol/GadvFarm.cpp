/*
 * The GADV part of the farm order (docs/protocol.md section 7.23, Farming.h): the item picker's search. Member of
 * GadvProtocol, kept apart from GadvProtocol.cpp. The order itself is set with ORDER_SET (o = FARM, zone = the item).
 */

#include "GadvProtocol.h"
#include "BotSettings.h"
#include "Farming.h"
#include "GuildAdventureMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include <algorithm>
#include <cctype>

using namespace GuildAdventure;
using GuildAdventure::Gadv::Message;

namespace
{
    constexpr std::size_t MaxSearchResults = 30;
    constexpr std::size_t MinSearchLength = 2;
}

void GadvProtocol::OnFarmSearch(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = message.GetInt("r");
    std::optional<int64> const g = message.GetInt("g");
    std::string const text = message.GetString("q");
    bool const number = !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    if (text.size() < MinSearchLength && !number)
    {
        Error(accountId, r, "BAD_REQUEST", "Type at least two letters of the item's name, or its id.");
        return;
    }

    // The bot the search is for (optional): its level and its own level margin say which items it can farm now.
    ObjectGuid const guid = g ? ObjectGuid::Create<HighGuid::Player>(uint32(*g)) : ObjectGuid::Empty;
    GuildBot const* bot = g ? (guid == guild.leader ? &guild.leaderPlan : guild.FindBot(guid)) : nullptr;
    uint32 level = 0;
    int32 margin = 0;
    if (bot)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(bot->guid);
        level = player ? player->GetLevel() : bot->snapshot.level;
        margin = sBotSettings.Get(bot->guid, Setting::FarmLevelsBelow);
    }

    std::vector<Message> records;
    for (FarmItem const& item : sFarming.Search(text, guild.team, MaxSearchResults))
    {
        // The level a bot needs for the lowest creature that drops it (with this bot's margin).
        uint32 const need = uint32(item.lowestLevel) + uint32(std::max(margin, 0));
        records.push_back(Message("FARM_ITEM")
            .Set("item", item.item)
            .Set("n", item.name)
            .Set("q", item.quality)
            .Set("il", item.itemLevel)
            .Set("cls", item.itemClass)
            .Set("k", item.kinds)
            .Set("lv", item.lowestLevel)
            .Set("need", need)
            .Set("ok", bot ? level >= need : true));
    }
    SendBlock(accountId, "FARM_ITEMS", records, Message("FARM_ITEMS_BEGIN").Set("q", text).Set("g", g ? uint32(*g) : 0u));
    Ack(accountId, r);
}
