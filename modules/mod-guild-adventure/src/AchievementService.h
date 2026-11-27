/*
 * AchievementService: guild-wide achievements (the owner, 2026-11-12: [quote left out]; the design log section 2.2, docs/guild-management.md "Guild achievements").
 *
 * When a guild character (the leader or a bot) completes an achievement, the guild has it: the first completion is
 * recorded once per guild (ga_guild_achievement), gives renown (its achievement points times
 * GuildAdventure.Achievements.RenownPerPoint, at least ga_renown_source.ACHIEVEMENT) and an ACHIEVEMENT event. With
 * GuildAdventure.Achievements.Share every other guild character gets it in its own achievement list too: the ones in
 * the world at once, the others when they next enter the world. A character entering the world also brings the
 * achievements it already had into the guild, so the guild's list is the union of its members' lists.
 *
 * Shared completions run through the core's own AchievementMgr::CompletedAchievement (meta achievements, titles and
 * reward mail follow as for a normal completion) inside a ShareScope, during which the guild chat line and the
 * "has earned" lines of that member are kept from the clients (ServerScript::CanPacketSend), so 40 members do not
 * fill the owner's chat with 40 lines; a client that plays the member itself still sees its own toast and line.
 * Realm-first achievements count for the guild but are never shared; counters and hidden (tracking) achievements
 * are ignored.
 *
 * World thread only, except IsBeingShared and ShouldSuppressPacket, which hooks call on any thread.
 */

#ifndef MOD_GUILD_ADVENTURE_ACHIEVEMENT_SERVICE_H
#define MOD_GUILD_ADVENTURE_ACHIEVEMENT_SERVICE_H

#include "Define.h"
#include "ObjectGuid.h"
#include <vector>

class Player;
class WorldPacket;
class WorldSession;
struct AchievementEntry;

namespace GuildAdventure
{
    struct AdventureGuild;
    struct GuildAchievement;
}

class AchievementService
{
public:
    static AchievementService& Instance();

    // Startup (world thread, after GuildAdventureMgr::LoadState): ga_guild_achievement into the guilds.
    void Load();

    // A guild character completed an achievement (hook queue).
    void OnAchievementCompleted(ObjectGuid character, uint32 achievementId);
    // A guild character entered the world (hook queue): its achievements join the guild's, the guild's join its own.
    void OnCharacterInWorld(ObjectGuid character);

    // The guild of a character: the leader's or the one it is a bot of.
    GuildAdventure::AdventureGuild* FindGuildOf(ObjectGuid character) const;

    // Counts for the guild at all: not a statistic (counter) and not a hidden tracking achievement.
    static bool CountsForGuild(AchievementEntry const* entry);
    // Goes into the members' own lists: counts for the guild and is not a realm-first achievement.
    static bool IsShareable(AchievementEntry const* entry);
    int32 GetRenownFor(AchievementEntry const* entry) const;

    uint32 GetPoints(GuildAdventure::AdventureGuild const& guild) const; // achievement points of the guild's list
    int32 GetRenown(GuildAdventure::AdventureGuild const& guild) const;  // renown its achievements gave
    // The guild's shareable achievements the character has / lacks (online characters only).
    void CountOnCharacter(GuildAdventure::AdventureGuild const& guild, Player* player, uint32& has, uint32& shareable) const;
    // The guild's achievements, newest first.
    std::vector<GuildAdventure::GuildAchievement const*> GetNewest(GuildAdventure::AdventureGuild const& guild, std::size_t count) const;

    // Hooks, any thread: the module is putting 'achievementId' into 'player''s list right now on this thread.
    static bool IsBeingShared(Player const* player, uint32 achievementId);
    // Hooks, any thread: true for the announcements of a shared completion that 'session' must not get.
    static bool ShouldSuppressPacket(WorldSession* session, WorldPacket const& packet);

private:
    AchievementService() = default;

    // Records a first completion for the guild; returns the renown it gives, or -1 if the guild had it already.
    int32 Record(GuildAdventure::AdventureGuild& guild, AchievementEntry const* entry, ObjectGuid earnedBy);
    // Puts the achievement into the list of every guild character in the world except 'except'; returns how many.
    uint32 ShareWithMembers(GuildAdventure::AdventureGuild const& guild, AchievementEntry const* entry, ObjectGuid except);
    // Puts the achievement into one character's list, quietly; true if it has it afterwards and did not before.
    bool Grant(Player* member, AchievementEntry const* entry);
};

#define sAchievementService AchievementService::Instance()

#endif
