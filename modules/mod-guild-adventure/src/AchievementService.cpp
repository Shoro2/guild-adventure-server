/*
 * AchievementService implementation. See AchievementService.h.
 */

#include "AchievementService.h"
#include "AchievementMgr.h"
#include "CharacterCache.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildEvents.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "ProgressionService.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>

using namespace GuildAdventure;

namespace
{
    // The member whose list the module is filling right now on this thread (world thread only; other threads always
    // see it empty, so their packets and hooks are never touched).
    struct ShareState
    {
        ObjectGuid recipient;
        uint32 achievementId = 0;
    };

    thread_local ShareState tShare;

    class ShareScope
    {
    public:
        ShareScope(ObjectGuid recipient, uint32 achievementId) : _previous(tShare)
        {
            tShare.recipient = recipient;
            tShare.achievementId = achievementId;
        }
        ~ShareScope() { tShare = _previous; }

        ShareScope(ShareScope const&) = delete;
        ShareScope& operator=(ShareScope const&) = delete;

    private:
        ShareState _previous;
    };

    std::string AchievementName(AchievementEntry const* entry)
    {
        char const* name = entry->name[LOCALE_enUS];
        return name && *name ? std::string(name) : Acore::StringFormat("achievement {}", entry->ID);
    }

    std::string CharacterName(ObjectGuid guid)
    {
        std::string name;
        if (!sCharacterCache->GetCharacterNameByGuid(guid, name) || name.empty())
            name = Acore::StringFormat("character {}", guid.GetCounter());
        return name;
    }

    std::vector<ObjectGuid> Members(AdventureGuild const& guild)
    {
        std::vector<ObjectGuid> members;
        members.reserve(guild.bots.size() + 1);
        if (guild.leader)
            members.push_back(guild.leader);
        for (auto const& [guid, bot] : guild.bots)
            members.push_back(guid);
        return members;
    }

    // The event belongs to the bot if a bot earned it; the leader's ones are guild events (EVENT^g = 0).
    ObjectGuid EventBot(AdventureGuild const& guild, ObjectGuid character)
    {
        return guild.FindBot(character) ? character : ObjectGuid::Empty;
    }

    bool FactionAllows(AchievementEntry const* entry, Player const* player)
    {
        if (entry->requiredFaction == ACHIEVEMENT_FACTION_ANY)
            return true;
        TeamId const team = player->GetTeamId(true);
        return (entry->requiredFaction == ACHIEVEMENT_FACTION_HORDE && team == TEAM_HORDE) ||
            (entry->requiredFaction == ACHIEVEMENT_FACTION_ALLIANCE && team == TEAM_ALLIANCE);
    }
}

AchievementService& AchievementService::Instance()
{
    static AchievementService instance;
    return instance;
}

void AchievementService::Load()
{
    uint32 count = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT owner_account, achievement_id, earned_by, earned_at, renown "
        "FROM ga_guild_achievement"))
    {
        do
        {
            Field* fields = result->Fetch();
            AdventureGuild* guild = sGuildAdventureMgr.FindGuild(fields[0].Get<uint32>());
            if (!guild)
                continue;

            GuildAchievement achievement;
            achievement.id = fields[1].Get<uint32>();
            achievement.earnedBy = ObjectGuid::Create<HighGuid::Player>(fields[2].Get<uint32>());
            achievement.earnedAt = fields[3].Get<uint32>();
            achievement.renown = fields[4].Get<int32>();
            guild->achievements[achievement.id] = achievement;
            ++count;
        } while (result->NextRow());
    }
    LOG_INFO("module", "mod-guild-adventure: loaded {} guild achievements", count);
}

AdventureGuild* AchievementService::FindGuildOf(ObjectGuid character) const
{
    if (AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfBot(character))
        return guild;
    return sGuildAdventureMgr.FindGuildOfLeader(character);
}

bool AchievementService::CountsForGuild(AchievementEntry const* entry)
{
    return entry && !(entry->flags & (ACHIEVEMENT_FLAG_COUNTER | ACHIEVEMENT_FLAG_HIDDEN));
}

bool AchievementService::IsShareable(AchievementEntry const* entry)
{
    return CountsForGuild(entry) && !(entry->flags & (ACHIEVEMENT_FLAG_REALM_FIRST_REACH | ACHIEVEMENT_FLAG_REALM_FIRST_KILL));
}

int32 AchievementService::GetRenownFor(AchievementEntry const* entry) const
{
    int32 const minimum = sGuildAdventureMgr.GetRenownPoints("ACHIEVEMENT");
    int64 const byPoints = ScaleRenown(int64(entry->points) * GetSettings().achievementsRenownPerPoint);
    return int32(std::max<int64>(minimum, byPoints));
}

int32 AchievementService::Record(AdventureGuild& guild, AchievementEntry const* entry, ObjectGuid earnedBy)
{
    if (guild.achievements.count(entry->ID))
        return -1;

    GuildAchievement achievement;
    achievement.id = entry->ID;
    achievement.earnedBy = earnedBy;
    achievement.earnedAt = uint32(GameTime::GetGameTime().count());
    achievement.renown = GetRenownFor(entry);
    guild.achievements[achievement.id] = achievement;

    CharacterDatabase.Execute("INSERT IGNORE INTO ga_guild_achievement (owner_account, achievement_id, earned_by, earned_at, renown) "
        "VALUES ({}, {}, {}, {}, {})", guild.ownerAccount, achievement.id, earnedBy.GetCounter(), achievement.earnedAt,
        achievement.renown);
    return achievement.renown;
}

void AchievementService::OnAchievementCompleted(ObjectGuid character, uint32 achievementId)
{
    AdventureGuild* guild = FindGuildOf(character);
    AchievementEntry const* entry = sAchievementStore.LookupEntry(achievementId);
    if (!guild || !CountsForGuild(entry))
        return;

    int32 const renown = Record(*guild, entry, character);
    if (renown >= 0)
    {
        std::string const name = CharacterName(character);
        RecordEvent(*guild, "ACHIEVEMENT", EventBot(*guild, character), Acore::StringFormat("{} earned the achievement \"{}\" for the guild.",
            name, AchievementName(entry)), 0, 0, renown);
        sProgressionService.AddRenown(*guild, renown);
        if (GetSettings().debug)
            LOG_INFO("module", "mod-guild-adventure: owner {} guild achievement {} ({}) by {}, +{} renown", guild->ownerAccount,
                entry->ID, AchievementName(entry), name, renown);
    }

    // Also when the guild had it already: a member that was skipped then (GM mode, still loading) gets it now.
    if (GetSettings().achievementsShare && IsShareable(entry))
        ShareWithMembers(*guild, entry, character);
}

void AchievementService::OnCharacterInWorld(ObjectGuid character)
{
    AdventureGuild* guild = FindGuildOf(character);
    Player* player = guild ? ObjectAccessor::FindPlayer(character) : nullptr;
    if (!player || !player->GetAchievementMgr())
        return;

    // 1. What the character brings: achievements it had before it joined, or earned while the module did not see it.
    std::vector<AchievementEntry const*> brought;
    int32 renown = 0;
    for (auto const& [id, data] : player->GetAchievementMgr()->GetCompletedAchievements())
    {
        AchievementEntry const* entry = sAchievementStore.LookupEntry(id);
        if (!CountsForGuild(entry) || guild->achievements.count(id))
            continue;
        renown += std::max(0, Record(*guild, entry, character));
        brought.push_back(entry);
    }

    if (!brought.empty())
    {
        std::string const name = CharacterName(character);
        RecordEvent(*guild, "ACHIEVEMENT", EventBot(*guild, character), brought.size() == 1 ?
            Acore::StringFormat("{} brought the achievement \"{}\" into the guild.", name, AchievementName(brought.front())) :
            Acore::StringFormat("{} brought {} achievements into the guild.", name, brought.size()), 0, 0, renown);
        sProgressionService.AddRenown(*guild, renown);
        LOG_INFO("module", "mod-guild-adventure: {} brought {} achievements into the guild of account {} (+{} renown)", name,
            brought.size(), guild->ownerAccount, renown);
    }

    if (!GetSettings().achievementsShare)
        return;

    for (AchievementEntry const* entry : brought)
        if (IsShareable(entry))
            ShareWithMembers(*guild, entry, character);

    // 2. What the guild has and the character lacks (it was offline, benched or not recruited yet).
    uint32 granted = 0;
    std::vector<uint32> ids;
    ids.reserve(guild->achievements.size());
    for (auto const& [id, achievement] : guild->achievements)
        ids.push_back(id);
    for (uint32 id : ids)
    {
        AchievementEntry const* entry = sAchievementStore.LookupEntry(id);
        if (IsShareable(entry) && Grant(player, entry))
            ++granted;
    }
    if (granted && GetSettings().debug)
        LOG_INFO("module", "mod-guild-adventure: {} got {} guild achievements on entering the world", player->GetName(), granted);
}

uint32 AchievementService::ShareWithMembers(AdventureGuild const& guild, AchievementEntry const* entry, ObjectGuid except)
{
    uint32 granted = 0;
    for (ObjectGuid member : Members(guild))
    {
        if (member == except)
            continue;
        // Characters still loading get it when they enter the world (OnCharacterInWorld).
        Player* player = ObjectAccessor::FindPlayer(member);
        if (player && player->IsInWorld() && Grant(player, entry))
            ++granted;
    }
    return granted;
}

bool AchievementService::Grant(Player* member, AchievementEntry const* entry)
{
    if (!member->GetSession() || member->HasAchieved(entry->ID) || !FactionAllows(entry, member))
        return false;

    // The core refuses achievements in GM mode and tells the player so; the member gets it on its next entry instead.
    if (member->IsGameMaster())
        return false;

    {
        ShareScope scope(member->GetGUID(), entry->ID);
        member->CompletedAchievement(entry);
    }
    return member->HasAchieved(entry->ID);
}

uint32 AchievementService::GetPoints(AdventureGuild const& guild) const
{
    uint32 points = 0;
    for (auto const& [id, achievement] : guild.achievements)
        if (AchievementEntry const* entry = sAchievementStore.LookupEntry(id))
            points += entry->points;
    return points;
}

int32 AchievementService::GetRenown(AdventureGuild const& guild) const
{
    int32 renown = 0;
    for (auto const& [id, achievement] : guild.achievements)
        renown += achievement.renown;
    return renown;
}

void AchievementService::CountOnCharacter(AdventureGuild const& guild, Player* player, uint32& has, uint32& shareable) const
{
    has = 0;
    shareable = 0;
    for (auto const& [id, achievement] : guild.achievements)
    {
        AchievementEntry const* entry = sAchievementStore.LookupEntry(id);
        if (!IsShareable(entry) || !FactionAllows(entry, player))
            continue;
        ++shareable;
        if (player->HasAchieved(id))
            ++has;
    }
}

std::vector<GuildAchievement const*> AchievementService::GetNewest(AdventureGuild const& guild, std::size_t count) const
{
    std::vector<GuildAchievement const*> list;
    list.reserve(guild.achievements.size());
    for (auto const& [id, achievement] : guild.achievements)
        list.push_back(&achievement);
    std::sort(list.begin(), list.end(), [](GuildAchievement const* a, GuildAchievement const* b)
    {
        return a->earnedAt != b->earnedAt ? a->earnedAt > b->earnedAt : a->id > b->id;
    });
    if (list.size() > count)
        list.resize(count);
    return list;
}

bool AchievementService::IsBeingShared(Player const* player, uint32 achievementId)
{
    return tShare.achievementId && player && tShare.achievementId == achievementId && tShare.recipient == player->GetGUID();
}

bool AchievementService::ShouldSuppressPacket(WorldSession* session, WorldPacket const& packet)
{
    if (!tShare.achievementId)
        return false;

    switch (packet.GetOpcode())
    {
        case SMSG_ACHIEVEMENT_EARNED:
            break;
        case SMSG_MESSAGECHAT:
        case SMSG_GM_MESSAGECHAT:
        {
            // uint8 type, int32 language, uint64 sender (ChatHandler::BuildChatPacket)
            if (packet.size() < 13)
                return false;
            uint8 const type = packet.read<uint8>(0);
            if (type != CHAT_MSG_ACHIEVEMENT && type != CHAT_MSG_GUILD_ACHIEVEMENT)
                return false;
            if (packet.read<uint64>(5) != tShare.recipient.GetRawValue())
                return false;
            break;
        }
        default:
            return false;
    }

    // The client that plays the member sees its own achievement as usual.
    Player* receiver = session ? session->GetPlayer() : nullptr;
    return !receiver || receiver->GetGUID() != tShare.recipient;
}
