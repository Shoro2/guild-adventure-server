/*
 * OfflineService implementation. See OfflineService.h.
 */

#include "OfflineService.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildAdventureUtil.h"
#include "GuildBotHolder.h"
#include "GuildEvents.h"
#include "RosterService.h"
#include "Log.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

using namespace GuildAdventure;

namespace
{
    constexpr uint32 UpdateIntervalMs = 1000;
}

OfflineService& OfflineService::Instance()
{
    static OfflineService instance;
    return instance;
}

bool OfflineService::IsOwnerOnline(uint32 ownerAccount) const
{
    // A session at the character screen counts as online (a Path R swap passes through it).
    return sWorldSessionMgr->FindSession(ownerAccount) != nullptr;
}

void OfflineService::Update(uint32 diff)
{
    _timer += diff;
    if (_timer < UpdateIntervalMs)
        return;
    _timer = 0;

    uint32 const now = Now();
    for (auto& [owner, guild] : sGuildAdventureMgr.GetGuilds())
    {
        WorldSession* session = sWorldSessionMgr->FindSession(owner);
        auto trackerItr = _trackers.find(owner);

        if (session)
        {
            // Back in the world with any character (the leader through RosterService, or a guild bot picked at
            // the character screen).
            if (trackerItr != _trackers.end() && session->GetPlayer())
                OnOwnerBack(guild);
            continue;
        }

        if (trackerItr == _trackers.end())
        {
            BeginWindow(guild);
            trackerItr = _trackers.find(owner);
        }

        Tracker& tracker = trackerItr->second;
        if (!tracker.windowOver && now >= tracker.since + uint32(guild.offlineWindowMin) * 60)
            EndWindow(guild, tracker);
    }
}

void OfflineService::BeginWindow(AdventureGuild& guild)
{
    Tracker tracker;
    // After a restart the stored logout time is kept, so the window does not start over.
    bool const resumed = guild.ownerLogoutAt != 0;
    if (!guild.ownerLogoutAt)
    {
        guild.ownerLogoutAt = Now();
        sGuildAdventureMgr.SaveGuild(guild);
    }
    tracker.since = guild.ownerLogoutAt;
    tracker.summary.from = guild.ownerLogoutAt;

    for (auto const& [guid, bot] : guild.bots)
    {
        BotStart start;
        start.level = bot.snapshot.level;
        start.zoneId = bot.snapshot.zoneId;
        start.money = bot.snapshot.money;
        start.quests = bot.quests;
        start.deaths = bot.deaths;
        tracker.start[guid] = start;
    }

    _pending.erase(guild.ownerAccount); // an older, unread summary is replaced by the new window
    uint32 const windowEnd = tracker.since + uint32(guild.offlineWindowMin) * 60;
    _trackers[guild.ownerAccount] = std::move(tracker);

    // A window that was running when the worldserver stopped goes on after the restart, but nothing logged its bots
    // in again (they log in with their owner): bring the active ones back for the rest of it, and a leader with an
    // order too (GuildAdventure.Leader.OfflinePlay; without an order it stays out, D12). A window that ended during
    // the downtime ends at the next update instead.
    if (resumed && Now() < windowEnd)
    {
        LOG_INFO("module", "mod-guild-adventure: owner account {} is still away: its offline window goes on for {} min, "
            "its guild bots log in again", guild.ownerAccount, (windowEnd - Now() + 59) / 60);
        sRosterService.LoginActiveBots(guild, nullptr);
        sRosterService.LoginPlannedLeader(guild);
    }

    if (GetSettings().debug)
        LOG_INFO("module", "mod-guild-adventure: owner account {} is away, offline window {} min", guild.ownerAccount, guild.offlineWindowMin);
}

void OfflineService::EndWindow(AdventureGuild& guild, Tracker& tracker)
{
    // A login still in flight would slip past the logout below and play on with no window to end it (a window of a few
    // seconds, or an owner who leaves right after logging in). The window ends once every character of this owner is
    // out of the world or driven by the AI; Update calls this again every second until then.
    std::vector<ManagedInfo> const characters = sGuildBotHolder.List(guild.ownerAccount);
    for (ManagedInfo const& info : characters)
        if (info.state == ManagedState::Loading || info.state == ManagedState::Loaded)
            return;

    tracker.windowOver = true;
    tracker.summary.to = Now();

    // Every guild character of this owner that the AI still drives logs out (the bots, and a leader that is
    // still in the world for any reason).
    uint32 loggedOut = 0;
    for (ManagedInfo const& info : characters)
        if (info.state == ManagedState::Bot && sGuildBotHolder.Logout(info.guid))
            ++loggedOut;

    if (loggedOut)
        RecordEvent(guild, "LOGOUT", ObjectGuid::Empty,
            Acore::StringFormat("The offline window ended; {} guild members logged out.", loggedOut));

    LOG_INFO("module", "mod-guild-adventure: offline window of owner account {} ended, {} characters logged out", guild.ownerAccount, loggedOut);
}

void OfflineService::OnOwnerBack(AdventureGuild& guild)
{
    auto itr = _trackers.find(guild.ownerAccount);
    if (itr != _trackers.end())
    {
        Tracker& tracker = itr->second;
        if (!tracker.windowOver)
            tracker.summary.to = Now();

        AwaySummary summary = Finish(guild, tracker);
        bool const anything = summary.levels || summary.quests || summary.deaths || summary.renown || summary.goldEarned ||
            !summary.lines.empty();
        if (anything)
            _pending[guild.ownerAccount] = std::move(summary);
        _trackers.erase(itr);
    }

    if (guild.ownerLogoutAt)
    {
        guild.ownerLogoutAt = 0;
        sGuildAdventureMgr.SaveGuild(guild);
    }
}

AwaySummary OfflineService::Finish(AdventureGuild const& guild, Tracker const& tracker) const
{
    AwaySummary summary = tracker.summary;
    for (auto const& [guid, bot] : guild.bots)
    {
        auto startItr = tracker.start.find(guid);
        if (startItr == tracker.start.end())
            continue;

        BotStart const& start = startItr->second;
        if (bot.snapshot.money > start.money)
            summary.goldEarned += bot.snapshot.money - start.money;

        uint32 const quests = bot.quests > start.quests ? bot.quests - start.quests : 0;
        uint32 const deaths = bot.deaths > start.deaths ? bot.deaths - start.deaths : 0;
        bool const levelChanged = bot.snapshot.level != start.level;
        bool const zoneChanged = bot.snapshot.zoneId != start.zoneId && bot.snapshot.zoneId;
        if (!levelChanged && !zoneChanged && !quests && !deaths)
            continue;

        std::string line = bot.name + ": ";
        line += levelChanged ? Acore::StringFormat("{} -> {}", start.level, bot.snapshot.level) : Acore::StringFormat("level {}", bot.snapshot.level);
        if (quests || deaths)
            line += Acore::StringFormat(" ({} quests, {} deaths)", quests, deaths);
        if (zoneChanged)
        {
            std::string const from = GetAreaName(start.zoneId);
            std::string const to = GetAreaName(bot.snapshot.zoneId);
            line += from.empty() ? ", now in " + to : ", " + from + " -> " + to;
        }
        summary.lines.emplace_back(guid, std::move(line));
    }
    return summary;
}

void OfflineService::OnEvent(AdventureGuild const& guild, GuildEvent const& event)
{
    auto itr = _trackers.find(guild.ownerAccount);
    if (itr == _trackers.end())
        return;

    AwaySummary& summary = itr->second.summary;
    if (event.type == "LEVEL_UP")
        ++summary.levels;
    else if (event.type == "QUEST_DONE")
        ++summary.quests;
    else if (event.type == "DEATH")
        ++summary.deaths;
    summary.renown += event.points;
}

AwaySummary const* OfflineService::GetPendingSummary(uint32 ownerAccount) const
{
    auto itr = _pending.find(ownerAccount);
    return itr != _pending.end() ? &itr->second : nullptr;
}

void OfflineService::AcknowledgeSummary(uint32 ownerAccount)
{
    _pending.erase(ownerAccount);
}

std::optional<uint32> OfflineService::GetWindowLeft(AdventureGuild const& guild) const
{
    auto itr = _trackers.find(guild.ownerAccount);
    if (itr == _trackers.end() || itr->second.windowOver)
        return std::nullopt;

    uint32 const end = itr->second.since + uint32(guild.offlineWindowMin) * 60;
    uint32 const now = Now();
    return end > now ? end - now : 0;
}
