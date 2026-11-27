/*
 * OfflineService: the offline window and the "while you were away" summary (the design log section 2.10).
 *
 * When the owner's client session goes away, the guild's bots keep playing for the guild's offline window
 * (ga_guild.offline_window_min); then they log out. While the owner is away, events of the guild feed an away
 * summary that the owner sees on the next login (protocol AWAY block) until the addon acknowledges it.
 * Away summaries live in memory only: a server restart ends the window anyway (every guild bot is offline then).
 * World thread only.
 */

#ifndef MOD_GUILD_ADVENTURE_OFFLINE_SERVICE_H
#define MOD_GUILD_ADVENTURE_OFFLINE_SERVICE_H

#include "ObjectGuid.h"
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace GuildAdventure
{
    struct AdventureGuild;
    struct GuildEvent;

    struct AwaySummary
    {
        uint32 from = 0;
        uint32 to = 0;
        uint32 levels = 0;
        int32 renown = 0;
        uint64 goldEarned = 0;
        uint32 quests = 0;
        uint32 deaths = 0;
        std::vector<std::pair<ObjectGuid, std::string>> lines;
    };
}

class OfflineService
{
public:
    static OfflineService& Instance();

    void Update(uint32 diff);

    // The owner entered the world with the guild leader (RosterService). Ends a running offline window and turns
    // its activity into a pending summary.
    void OnOwnerBack(GuildAdventure::AdventureGuild& guild);
    void OnEvent(GuildAdventure::AdventureGuild const& guild, GuildAdventure::GuildEvent const& event);

    [[nodiscard]] bool IsOwnerOnline(uint32 ownerAccount) const;
    [[nodiscard]] GuildAdventure::AwaySummary const* GetPendingSummary(uint32 ownerAccount) const;
    void AcknowledgeSummary(uint32 ownerAccount);
    // Seconds left in the running offline window, nullopt when the owner is online or the window is over.
    [[nodiscard]] std::optional<uint32> GetWindowLeft(GuildAdventure::AdventureGuild const& guild) const;

private:
    OfflineService() = default;

    struct BotStart
    {
        uint8 level = 0;
        uint32 zoneId = 0;
        uint32 money = 0;
        uint32 quests = 0;
        uint32 deaths = 0;
    };

    struct Tracker
    {
        uint32 since = 0;
        bool windowOver = false;
        GuildAdventure::AwaySummary summary;
        std::unordered_map<ObjectGuid, BotStart> start;
    };

    void BeginWindow(GuildAdventure::AdventureGuild& guild);
    void EndWindow(GuildAdventure::AdventureGuild& guild, Tracker& tracker);
    GuildAdventure::AwaySummary Finish(GuildAdventure::AdventureGuild const& guild, Tracker const& tracker) const;

    std::unordered_map<uint32, Tracker> _trackers;                       // owners who are away right now
    std::unordered_map<uint32, GuildAdventure::AwaySummary> _pending;    // summaries waiting for AWAY_ACK
    uint32 _timer = 0;
};

#define sOfflineService OfflineService::Instance()

#endif
