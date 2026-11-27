/*
 * GuildBudget implementation. See GuildBudget.h.
 */

#include "GuildBudget.h"
#include "BotSettings.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildBank.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"
#include <algorithm>
#include <unordered_map>

using namespace GuildAdventure;

namespace
{
    // What the guild bank has paid for each guild character, by character guid.
    std::unordered_map<ObjectGuid::LowType, uint32> spent;

    // The guild's characters that are in the world (the leader and the roster bots).
    std::vector<Player*> MembersInWorld(AdventureGuild const& guild)
    {
        std::vector<Player*> members;
        if (Player* leader = ObjectAccessor::FindPlayer(guild.leader))
            members.push_back(leader);
        for (auto const& [guid, bot] : guild.bots)
            if (Player* player = ObjectAccessor::FindPlayer(guid))
                members.push_back(player);
        return members;
    }

    // What the buyer pays of its own money for one purchase at most.
    uint32 ShareOf(Player const* member)
    {
        return uint32(uint64(member->GetMoney()) * uint32(sBotSettings.Get(member, Setting::BudgetMemberPercent)) / 100);
    }

    // The most the guild bank pays for one item right now.
    uint32 ItemLimitOf(AdventureGuild const& guild)
    {
        uint64 const limit = GuildBank::Money(guild.ownerAccount) *
            uint32(sBotSettings.GetForGuild(guild.ownerAccount, Setting::BudgetItemPercent)) / 100;
        return uint32(std::min<uint64>(limit, 0xFFFFFFFFu));
    }

    void SaveSpent(ObjectGuid guid)
    {
        CharacterDatabase.Execute("REPLACE INTO ga_bot_budget (bot_guid, spent, updated_at) VALUES ({}, {}, {})", guid.GetCounter(),
            spent[guid.GetCounter()], uint32(GameTime::GetGameTime().count()));
    }
}

void GuildBudget::Load()
{
    spent.clear();
    if (QueryResult result = CharacterDatabase.Query("SELECT bot_guid, spent FROM ga_bot_budget"))
        do
        {
            Field* fields = result->Fetch();
            spent[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
        } while (result->NextRow());
    LOG_INFO("module", "mod-guild-adventure: loaded what the guild banks paid for {} character{}", spent.size(),
        spent.size() == 1 ? "" : "s");
}

uint32 GuildBudget::SpentBy(ObjectGuid guid)
{
    auto itr = spent.find(guid.GetCounter());
    return itr != spent.end() ? itr->second : 0;
}

uint32 GuildBudget::AllowanceLeft(Player const* buyer)
{
    uint32 const allowance = uint32(sBotSettings.Get(buyer, Setting::BudgetBotAllowance));
    uint32 const used = SpentBy(buyer->GetGUID());
    return allowance > used ? allowance - used : 0;
}

uint32 GuildBudget::ItemLimit(Player const* buyer)
{
    AdventureGuild const* guild = sGuildAdventureMgr.FindGuildOfMember(buyer->GetGUID());
    return guild ? ItemLimitOf(*guild) : 0;
}

std::string GuildBudget::MoneyText(uint32 copper)
{
    uint32 const gold = copper / GOLD;
    uint32 const silver = (copper % GOLD) / SILVER;
    uint32 const rest = copper % SILVER;
    std::string text;
    if (gold)
        text += Acore::StringFormat("{}g", gold);
    if (silver)
        text += Acore::StringFormat("{}{}s", text.empty() ? "" : " ", silver);
    if (rest || text.empty())
        text += Acore::StringFormat("{}{}c", text.empty() ? "" : " ", rest);
    return text;
}

uint64 GuildBudget::TotalGold(AdventureGuild const& guild)
{
    uint64 total = 0;
    for (Player const* member : MembersInWorld(guild))
        total += member->GetMoney();
    return total;
}

uint64 GuildBudget::TotalGold(Player const* buyer)
{
    AdventureGuild const* guild = sGuildAdventureMgr.FindGuildOfMember(buyer->GetGUID());
    return guild ? TotalGold(*guild) : 0;
}

uint64 GuildBudget::BankMoney(Player const* buyer)
{
    AdventureGuild const* guild = sGuildAdventureMgr.FindGuildOfMember(buyer->GetGUID());
    return guild ? GuildBank::Money(guild->ownerAccount) : 0;
}

uint32 GuildBudget::GuildPartAvailable(Player const* buyer)
{
    AdventureGuild const* guild = sGuildAdventureMgr.FindGuildOfMember(buyer->GetGUID());
    if (!guild)
        return 0;
    return uint32(std::min<uint64>({ uint64(ItemLimitOf(*guild)), uint64(AllowanceLeft(buyer)), GuildBank::Money(guild->ownerAccount) }));
}

uint32 GuildBudget::Available(Player const* buyer)
{
    if (!sGuildAdventureMgr.FindGuildOfMember(buyer->GetGUID()))
        return 0;
    return uint32(std::min<uint64>(uint64(ShareOf(buyer)) + GuildPartAvailable(buyer), 0xFFFFFFFFu));
}

GuildBudget::Grant GuildBudget::Request(Player* buyer, uint32 price)
{
    Grant grant;
    AdventureGuild const* guild = sGuildAdventureMgr.FindGuildOfMember(buyer->GetGUID());
    if (!guild || !price)
        return grant;

    // The buyer's own share first; what is missing the guild bank pays, within the leader's limits (the owner,
    // 2026-11-13: an empty guild bank pays nothing).
    uint32 const own = std::min(price, ShareOf(buyer));
    uint32 const guildPart = price - own;
    if (guildPart)
    {
        if (guildPart > ItemLimitOf(*guild) || guildPart > AllowanceLeft(buyer))
            return grant;
        if (!GuildBank::Withdraw(guild->ownerAccount, guildPart, Acore::StringFormat("for a purchase of {} by {}", MoneyText(price),
            buyer->GetName())))
            return grant;
        {
            GuildBank::NoTax noTax; // the guild's own gold, handed over for the purchase: no tax on it
            buyer->ModifyMoney(int32(guildPart));
        }
        grant.fromGuild = guildPart;
        spent[buyer->GetGUID().GetCounter()] += guildPart;
        SaveSpent(buyer->GetGUID());
    }
    grant.ok = true;
    return grant;
}

void GuildBudget::Cancel(Player* buyer, uint32 /*price*/, Grant const& grant)
{
    if (!grant.ok || !grant.fromGuild)
        return;
    buyer->ModifyMoney(-int32(grant.fromGuild));
    if (AdventureGuild const* guild = sGuildAdventureMgr.FindGuildOfMember(buyer->GetGUID()))
        GuildBank::Deposit(guild->ownerAccount, grant.fromGuild, Acore::StringFormat("back from {}: the purchase did not happen",
            buyer->GetName()));
    uint32& used = spent[buyer->GetGUID().GetCounter()];
    used = used > grant.fromGuild ? used - grant.fromGuild : 0;
    SaveSpent(buyer->GetGUID());
}
