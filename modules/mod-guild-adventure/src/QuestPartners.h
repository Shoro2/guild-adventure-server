/*
 * Quest partners: guild bots that have the same hard quest team up to finish it (docs/orders.md "Quest partners").
 *
 * The owner, 2026-11-13: [quote left out].
 *
 *   - Hard: an elite or group quest (quest type "Group", more than one suggested player, or an elite creature to kill;
 *     dungeon and raid quests are the dungeon runs' and do not count); a quest guild characters died on
 *     GuildAdventure.Squads.PartnerDeaths times within PartnerHours; a quest whose every spot near was too much for a
 *     guild character at its level (GuildAdventure.Squads.PartnerTooHard, the design log section 7 "Open" QP1). A death or a
 *     "too much" counts for guild characters up to the level it happened at (as the death rest of the planner does).
 *   - Without a partner the quest stays in the log, but the planner skips it (no objective task) until another guild
 *     character holds it too; it is not given back for deaths or for no progress meanwhile, the quest log care gives it
 *     up only when nothing else is left, the grey rule still applies. A bot in a squad of two or more (its own, or the
 *     owner's) does not skip it: it has its partners beside it.
 *   - Pairing (Update, every 20 s per guild): AI-played roster bots on AUTO or LEVEL_ZONE in no squad, with the same
 *     hard quest unfinished, within GuildAdventure.Squads.MaxLevelGap levels, on one map within PartnerYards: the
 *     higher level leads, the others follow it (a squad link held in memory only, never stored). A member walks to its
 *     leader along the road map (no catch-up teleport before they met), the leader waits for it; then they play as any
 *     squad, the quest first. When the leader is done and its only member is not, the roles turn round. A link goes
 *     when the quest is done, when they did not meet within PartnerMeetMinutes, after PartnerMaxMinutes, or when the
 *     quest is too much even together; the quest is then not paired again for PartnerRetryMinutes.
 *   - Pickups: a hard quest is picked up only by a bot in a squad of two or more, or by one that stands near its giver
 *     while another guild character that could be its partner keeps it.
 *   - The switch is the bot setting Squads.QuestPartners (guild and bot); off, or squads off: as before.
 *
 * World thread only.
 */

#ifndef MOD_GUILD_ADVENTURE_QUEST_PARTNERS_H
#define MOD_GUILD_ADVENTURE_QUEST_PARTNERS_H

#include "ObjectGuid.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;

namespace GuildAdventure
{
    struct AdventureGuild;
    struct GuildBot;
}

class QuestPartnerService
{
public:
    static QuestPartnerService& instance();

    // Whether quest partners act for this character (squads on, the bot setting Squads.QuestPartners, a roster bot).
    [[nodiscard]] bool IsEnabled(Player const* player) const;
    // An elite or group quest by its data.
    [[nodiscard]] bool IsGroupQuest(uint32 questId) const;
    // Why the quest is hard for a guild character of `level` ("" = it is not): "an elite or group quest",
    // "2 deaths of the guild on it", "too much for Magefrost at level 14".
    [[nodiscard]] std::string WhyHard(uint32 ownerAccount, uint32 questId, uint8 level) const;

    // The planner skips the quest: it is hard, unfinished, and no partner stands beside the bot (CollectLogTasks).
    [[nodiscard]] bool IsSkipped(GuildAdventure::GuildBot const& bot, Player* player, uint32 questId) const;
    // A hard quest kept for a partner instead of being given back (OrderService::FailQuest, reasons "deaths" and
    // "no_progress" of a bot without a partner beside it).
    bool KeepsInsteadOfFailing(GuildAdventure::GuildBot const& bot, Player* player, uint32 questId, std::string const& reason);
    // Whether the planner may walk to a giver for a quest: a hard quest only in a squad of two or more, or near the giver
    // while another guild character that could be its partner keeps it.
    [[nodiscard]] bool AllowsPickup(GuildAdventure::GuildBot const& bot, Player* player, uint32 questId, float giverDistance) const;
    // The quests the bot keeps for a partner now (in the log, unfinished, skipped).
    [[nodiscard]] std::vector<uint32> Waiting(GuildAdventure::GuildBot const& bot, Player* player) const;

    // A guild character died while it worked on the quest (OrderService::OnDied, a squad member's death).
    void OnQuestDeath(Player* player, uint32 questId);
    // Every spot of the quest near the character was too much for it up to `level`, and a second character beside it
    // would make one workable (OrderService::PlanQuests).
    void OnTooHard(Player* player, uint32 questId, uint8 level);
    // The planner left the quest until `untilLevel` (0: only for a time): a pair that parts because its leader left the
    // quest so does not pair up for it again before one of them is that level.
    void OnLeftUntil(Player* player, uint32 questId, uint8 untilLevel);
    // The owner took a quest partner out of its squad: that quest is not paired again for a while.
    void OnOwnerSplit(GuildAdventure::AdventureGuild const& guild, uint32 questId);

    // Pairing, meeting, splitting (OrderService::Update, about every 20 s per guild).
    void Update(GuildAdventure::AdventureGuild& guild);

    // Lines for ".ga squad": the guild's hard quests and the quests its bots keep for a partner.
    [[nodiscard]] std::vector<std::string> DescribeGuild(GuildAdventure::AdventureGuild const& guild) const;

private:
    QuestPartnerService() = default;

    struct Mark
    {
        uint32 at = 0;     // game time
        uint8 level = 0;   // counts for guild characters up to this level
        std::string who;   // the character it happened to ...
        uint8 whoLevel = 0; // ... at its level then
    };
    struct HardMarks
    {
        std::vector<Mark> deaths;
        std::vector<Mark> tooHard;
    };

    void Prune(HardMarks& marks) const;
    void Split(GuildAdventure::AdventureGuild& guild, ObjectGuid member, ObjectGuid leader, uint32 questId, std::string const& why,
        bool retryLater);

    std::unordered_map<uint32, std::unordered_map<uint32, HardMarks>> _hard;   // owner -> quest -> marks
    std::unordered_map<uint32, std::unordered_map<uint32, uint32>> _retryAt;   // owner -> quest -> game time it may be paired again
    std::unordered_map<ObjectGuid, std::unordered_set<uint32>> _announced;      // bot -> quests its "keeps ..." line was written for
    std::unordered_map<ObjectGuid, uint32> _pairRestUntil;                       // bot -> game time: no new pair after one that failed
    std::unordered_map<ObjectGuid, std::unordered_map<uint32, uint8>> _leftUntil; // bot -> quest -> level the planner left it until
    struct Hold
    {
        uint8 level = 0;   // paired again once a holder of the quest is this level ...
        uint32 until = 0;  // ... or after this game time
    };
    std::unordered_map<uint32, std::unordered_map<uint32, Hold>> _held;         // owner -> quest -> after a pair that could not work it
    std::unordered_map<ObjectGuid, uint32> _idleSince;                          // member -> game time since its leader has worked no
                                                                                // task of the pair's quest (after they met)
    std::unordered_map<uint32, uint32> _nextUpdate;                             // owner -> getMSTime()
    mutable std::unordered_map<uint32, bool> _groupQuests;
};

#define sQuestPartners QuestPartnerService::instance()

#endif
