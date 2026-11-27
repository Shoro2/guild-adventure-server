/*
 * SquadService implementation. See Squads.h and docs/orders.md "Squads".
 */

#include "Squads.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildAdventureUtil.h"
#include "GuildBotHolder.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include <algorithm>

using namespace GuildAdventure;

namespace
{
    // Automatic squads: bots of the same zone within this many levels.
    constexpr uint8 AutoLevelGap = 2;
    // ... standing within this distance of each other.
    constexpr float AutoPairYards = 400.0f;

    uint8 LevelOf(GuildBot const& bot)
    {
        if (Player* player = ObjectAccessor::FindConnectedPlayer(bot.guid); player && player->IsInWorld())
            return player->GetLevel();
        return bot.snapshot.level;
    }

    // The leader's character while it is in the world and the AI plays it (also in watch mode) or its owner plays it.
    Player* LeaderInWorld(ObjectGuid leader)
    {
        if (Player* planned = sGuildBotHolder.GetPlannedPlayer(leader))
            return planned->IsInWorld() && !planned->IsDuringRemoveFromWorld() ? planned : nullptr;
        if (!sGuildBotHolder.IsManaged(leader))
            return nullptr;
        Player* player = ObjectAccessor::FindConnectedPlayer(leader);
        if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld() || !player->GetSession() ||
            player->GetSession()->IsHeadless())
            return nullptr;
        return player;
    }
}

char const* GuildAdventure::ToToken(SquadState state)
{
    switch (state)
    {
        case SquadState::None:     return "none";
        case SquadState::Active:   return "active";
        case SquadState::Disabled: return "off";
        case SquadState::Benched:  return "benched";
        case SquadState::Away:     return "away";
        case SquadState::LevelGap: return "gap";
        case SquadState::Leads:    return "leads";
    }
    return "none";
}

std::string GuildAdventure::Describe(SquadState state)
{
    switch (state)
    {
        case SquadState::None:     return "in no squad";
        case SquadState::Active:   return "follows its squad leader";
        case SquadState::Disabled: return "squads are switched off on this server";
        case SquadState::Benched:  return "its squad leader is benched: it plays its own order";
        case SquadState::Away:     return "its squad leader is not in the world: it plays its own order";
        case SquadState::LevelGap: return "too many levels apart from its squad leader: it plays its own order";
        case SquadState::Leads:    return "leads a squad";
    }
    return "";
}

SquadService& SquadService::instance()
{
    static SquadService instance;
    return instance;
}

void SquadService::Load()
{
    _links.clear();
    if (QueryResult result = CharacterDatabase.Query("SELECT bot_guid, leader_guid, set_at, automatic FROM ga_bot_squad"))
    {
        do
        {
            Field* fields = result->Fetch();
            ObjectGuid const member = ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>());
            Link link;
            link.leader = ObjectGuid::Create<HighGuid::Player>(fields[1].Get<uint32>());
            link.setAt = fields[2].Get<uint32>();
            link.automatic = fields[3].Get<uint8>() != 0;
            _links[member] = link;
        } while (result->NextRow());
    }
    LOG_INFO("module", "mod-guild-adventure: loaded {} squad members", _links.size());
}

void SquadService::Save(ObjectGuid member, Link const& link) const
{
    CharacterDatabase.Execute("REPLACE INTO ga_bot_squad (bot_guid, leader_guid, set_at, automatic) VALUES ({}, {}, {}, {})",
        member.GetCounter(), link.leader.GetCounter(), link.setAt, link.automatic ? 1 : 0);
}

void SquadService::Delete(ObjectGuid member) const
{
    CharacterDatabase.Execute("DELETE FROM ga_bot_squad WHERE bot_guid = {}", member.GetCounter());
}

bool SquadService::Set(AdventureGuild& guild, ObjectGuid member, ObjectGuid leader, std::string& error, bool automatic, uint32 quest)
{
    Settings const& settings = GetSettings();
    GuildBot const* memberBot = guild.FindBot(member);
    // (The guild leader's own character only as a member of a dungeon group, OrderService::StartDungeonGroup; it can
    // always leave.)
    if (!memberBot && member == guild.leader && (_anyLevel || leader.IsEmpty()))
        memberBot = guild.FindMember(member);
    if (!memberBot)
    {
        error = "that character is not one of your guild bots";
        return false;
    }

    if (leader.IsEmpty())
    {
        auto itr = _links.find(member);
        if (itr == _links.end())
            return true;
        LOG_INFO("module", "mod-guild-adventure: {} leaves its squad", memberBot->name);
        bool const stored = itr->second.quest == 0;
        _links.erase(itr);
        if (stored)
            Delete(member);
        return true;
    }

    if (!settings.squadsEnable)
    {
        error = "squads are switched off on this server";
        return false;
    }
    GuildBot const* leaderBot = guild.FindBot(leader);
    if (!leaderBot)
    {
        error = "the squad leader must be one of your guild bots";
        return false;
    }
    if (leader == member)
    {
        error = "a bot cannot follow itself";
        return false;
    }
    if (ObjectGuid const above = GetLeader(leader); !above.IsEmpty())
    {
        GuildBot const* aboveBot = guild.FindBot(above);
        error = Acore::StringFormat("{} follows {} already; a squad has one leader", leaderBot->name, aboveBot ? aboveBot->name : "another bot");
        return false;
    }
    if (!GetMembers(member).empty())
    {
        error = Acore::StringFormat("{} leads a squad itself", memberBot->name);
        return false;
    }
    std::vector<ObjectGuid> members = GetMembers(leader);
    bool const already = std::find(members.begin(), members.end(), member) != members.end();
    // (The owner's choice over a quest partner link to the same leader: the link stays, but is stored from now on.)
    if (already && !quest)
        if (auto itr = _links.find(member); itr != _links.end() && itr->second.quest)
        {
            itr->second.quest = 0;
            itr->second.automatic = automatic;
            Save(member, itr->second);
        }
    if (already)
        return true;
    // (Not for the group of a dungeon or raid run: its size is the dungeon's, up to the 40 of a raid; the run window and
    // StartDungeonGroup hold it to that.)
    if (!_anyLevel && members.size() + 2 > settings.squadsMaxSize)
    {
        error = Acore::StringFormat("the squad of {} is full ({} characters at most)", leaderBot->name, settings.squadsMaxSize);
        return false;
    }
    uint8 const memberLevel = LevelOf(*memberBot);
    uint8 const leaderLevel = LevelOf(*leaderBot);
    // (Not for the group of a dungeon run: the guild leader sends whom he wants, also bots that have outgrown a dungeon
    // beside ones at its level. The owner: [quote left out].)
    if (!_anyLevel && std::abs(int32(memberLevel) - int32(leaderLevel)) > int32(settings.squadsMaxLevelGap))
    {
        error = Acore::StringFormat("{} (level {}) and {} (level {}) are more than {} levels apart", memberBot->name, uint32(memberLevel),
            leaderBot->name, uint32(leaderLevel), uint32(settings.squadsMaxLevelGap));
        return false;
    }

    Link link;
    link.leader = leader;
    link.setAt = uint32(GameTime::GetGameTime().count());
    link.automatic = automatic;
    link.quest = quest;
    // (A stored link the member had before: a quest partner link replaces it only in memory.)
    if (auto old = _links.find(member); old != _links.end() && old->second.quest == 0 && quest)
        Delete(member);
    _links[member] = link;
    if (!quest)
        Save(member, link);
    LOG_INFO("module", "mod-guild-adventure: {} joins the squad of {}{}", memberBot->name, leaderBot->name,
        quest ? Acore::StringFormat(" (quest partners for quest {})", quest) : automatic ? std::string(" (automatic)") : std::string());
    return true;
}

void SquadService::Forget(ObjectGuid bot)
{
    for (auto itr = _links.begin(); itr != _links.end();)
    {
        if (itr->first == bot || itr->second.leader == bot)
        {
            if (!itr->second.quest)
                Delete(itr->first);
            itr = _links.erase(itr);
        }
        else
            ++itr;
    }
}

ObjectGuid SquadService::GetLeader(ObjectGuid member) const
{
    auto itr = _links.find(member);
    return itr != _links.end() ? itr->second.leader : ObjectGuid::Empty;
}

std::vector<ObjectGuid> SquadService::GetMembers(ObjectGuid leader) const
{
    std::vector<ObjectGuid> members;
    for (auto const& [member, link] : _links)
        if (link.leader == leader)
            members.push_back(member);
    std::sort(members.begin(), members.end());
    return members;
}

bool SquadService::IsAutomatic(ObjectGuid member) const
{
    auto itr = _links.find(member);
    return itr != _links.end() && itr->second.automatic;
}

uint32 SquadService::GetPartnerQuest(ObjectGuid member) const
{
    auto itr = _links.find(member);
    return itr != _links.end() ? itr->second.quest : 0;
}

uint32 SquadService::GetLedPartnerQuest(ObjectGuid leader) const
{
    for (auto const& [member, link] : _links)
        if (link.leader == leader && link.quest)
            return link.quest;
    return 0;
}

std::vector<ObjectGuid> SquadService::GetPartnerMembers() const
{
    std::vector<ObjectGuid> members;
    for (auto const& [member, link] : _links)
        if (link.quest)
            members.push_back(member);
    std::sort(members.begin(), members.end());
    return members;
}

bool SquadService::HasMet(ObjectGuid member) const
{
    auto itr = _links.find(member);
    return itr != _links.end() && itr->second.met;
}

uint32 SquadService::GetSetAt(ObjectGuid member) const
{
    auto itr = _links.find(member);
    return itr != _links.end() ? itr->second.setAt : 0;
}

void SquadService::MarkMet(ObjectGuid member)
{
    if (auto itr = _links.find(member); itr != _links.end())
        itr->second.met = true;
}

SquadState SquadService::GetState(AdventureGuild const& guild, ObjectGuid member, Player** leaderPlayer) const
{
    if (leaderPlayer)
        *leaderPlayer = nullptr;

    auto itr = _links.find(member);
    if (itr == _links.end())
    {
        for (auto const& [other, link] : _links)
            if (link.leader == member && guild.FindMember(other))
                return SquadState::Leads;
        return SquadState::None;
    }
    if (!GetSettings().squadsEnable)
        return SquadState::Disabled;

    GuildBot const* leaderBot = guild.FindBot(itr->second.leader);
    GuildBot const* memberBot = guild.FindMember(member);
    if (!leaderBot || !memberBot)
        return SquadState::None;
    if (leaderBot->state == BotState::Benched)
        return SquadState::Benched;
    Player* leader = LeaderInWorld(itr->second.leader);
    if (!leader)
        return SquadState::Away;
    uint8 const memberLevel = LevelOf(*memberBot);
    if (leaderBot->order.type != OrderType::Dungeon &&
        std::abs(int32(memberLevel) - int32(leader->GetLevel())) > int32(GetSettings().squadsMaxLevelGap))
        return SquadState::LevelGap;
    if (leaderPlayer)
        *leaderPlayer = leader;
    return SquadState::Active;
}

std::vector<Player*> SquadService::GetFollowers(AdventureGuild const& guild, ObjectGuid leader) const
{
    std::vector<Player*> followers;
    for (auto const& [member, link] : _links)
    {
        if (link.leader != leader)
            continue;
        Player* player = sGuildBotHolder.GetPlannedPlayer(member);
        if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld())
            continue;
        if (GetState(guild, member) != SquadState::Active)
            continue;
        followers.push_back(player);
    }
    return followers;
}

std::vector<Player*> SquadService::GetPartners(Player const* player, float range) const
{
    std::vector<Player*> partners;
    if (!player || !GetSettings().squadsEnable)
        return partners;
    ObjectGuid const guid = player->GetGUID();
    AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(guid);
    if (!guild)
        return partners;

    ObjectGuid leader = GetLeader(guid);
    std::vector<Player*> candidates;
    if (leader.IsEmpty())
        candidates = GetFollowers(*guild, guid); // a leader (or a bot without a squad: none)
    else
    {
        Player* leaderPlayer = nullptr;
        if (GetState(*guild, guid, &leaderPlayer) != SquadState::Active || !leaderPlayer)
            return partners;
        candidates = GetFollowers(*guild, leader);
        candidates.push_back(leaderPlayer);
    }
    for (Player* other : candidates)
        if (other && other != player && other->IsAlive() && other->GetMapId() == player->GetMapId() &&
            other->IsWithinDistInMap(player, range))
            partners.push_back(other);
    return partners;
}

uint32 SquadService::CountTogether(Player const* player, float range) const
{
    return 1 + uint32(GetPartners(player, range).size());
}

void SquadService::AutoPair(AdventureGuild& guild)
{
    Settings const& settings = GetSettings();
    if (!settings.squadsEnable || !settings.squadsAuto || settings.squadsMaxSize < 2)
        return;

    // Automatic links that no longer fit (another zone, the levels drifted apart) are given up first.
    for (auto const& [guid, bot] : guild.bots)
    {
        auto itr = _links.find(guid);
        if (itr == _links.end() || !itr->second.automatic)
            continue;
        Player* member = sGuildBotHolder.GetAIDrivenPlayer(guid);
        Player* leader = sGuildBotHolder.GetAIDrivenPlayer(itr->second.leader);
        if (!member || !leader)
            continue;
        if (member->GetZoneId() != leader->GetZoneId() && member->GetMapId() == leader->GetMapId() &&
            !member->IsWithinDistInMap(leader, AutoPairYards))
        {
            std::string error;
            Set(guild, guid, ObjectGuid::Empty, error);
        }
    }

    // Bots without a squad, in the world, on an autonomous order: grouped by zone, nearest levels together.
    std::vector<Player*> loose;
    for (auto const& [guid, bot] : guild.bots)
    {
        if (bot.state != BotState::Active || (bot.order.type != OrderType::Auto && bot.order.type != OrderType::LevelZone))
            continue;
        if (_links.count(guid) || !GetMembers(guid).empty())
            continue;
        if (Player* player = sGuildBotHolder.GetAIDrivenPlayer(guid); player && player->IsInWorld() && player->IsAlive())
            loose.push_back(player);
    }
    std::sort(loose.begin(), loose.end(), [](Player const* a, Player const* b)
    {
        return a->GetZoneId() != b->GetZoneId() ? a->GetZoneId() < b->GetZoneId() :
            a->GetLevel() != b->GetLevel() ? a->GetLevel() > b->GetLevel() : a->GetGUID() < b->GetGUID();
    });

    std::vector<bool> taken(loose.size(), false);
    for (std::size_t i = 0; i < loose.size(); ++i)
    {
        if (taken[i])
            continue;
        Player* leader = loose[i];
        uint32 size = 1;
        for (std::size_t j = i + 1; j < loose.size() && size < settings.squadsMaxSize; ++j)
        {
            Player* member = loose[j];
            if (taken[j] || member->GetZoneId() != leader->GetZoneId() || member->GetMapId() != leader->GetMapId() ||
                leader->GetLevel() > member->GetLevel() + AutoLevelGap || !member->IsWithinDistInMap(leader, AutoPairYards))
                continue;
            std::string error;
            if (Set(guild, member->GetGUID(), leader->GetGUID(), error, true))
            {
                taken[j] = true;
                ++size;
            }
        }
        if (size > 1)
            taken[i] = true;
    }
}

std::vector<std::string> SquadService::DescribeGuild(AdventureGuild const& guild) const
{
    std::vector<std::string> lines;
    for (auto const& [guid, bot] : guild.bots)
    {
        std::vector<ObjectGuid> const members = GetMembers(guid);
        if (members.empty())
            continue;
        std::string line = Acore::StringFormat("Squad of {} (level {}):", bot.name, uint32(LevelOf(bot)));
        for (ObjectGuid const& member : members)
        {
            GuildBot const* memberBot = guild.FindMember(member);
            uint32 const quest = GetPartnerQuest(member);
            line += Acore::StringFormat(" {} (level {}, {}{})", memberBot ? memberBot->name : member.ToString(),
                memberBot ? uint32(LevelOf(*memberBot)) : 0u, ToToken(GetState(guild, member)),
                quest ? Acore::StringFormat(", quest partner for quest {}{}", quest, HasMet(member) ? "" : ", on its way") :
                IsAutomatic(member) ? std::string(", automatic") : std::string());
        }
        lines.push_back(line);
    }
    if (lines.empty())
        lines.push_back("No squads.");
    return lines;
}
