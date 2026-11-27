/*
 * QuestSupport: the quests AI-played guild characters do not take, because the order planner cannot finish them
 * (docs/orders.md "Quests the bots leave alone").
 *
 * mod-playerbots' NewRpg accepts every quest a giver offers that passes Player::CanTakeQuest, also the ones the
 * planner has no way to complete; each of them then costs a bot two rounds of five to eight minutes before the planner
 * blocks it for that bot (wowbot335 A21), and the next bot pays again. Three sources say "leave it" up front:
 *   - Listed:        ga_quest_block_template in the world database, a hand-checked list with a reason per quest (under
 *                    water, needs bought items, the ender is out of reach, ...);
 *   - ScriptedEvent: the quest completes through a script event the planner cannot trigger (escort, defence, a talk
 *                    with choices): special flag "exploration or event" without an area trigger and without a quest
 *                    item to use;
 *   - Learned:       enough different guild bots ran into the planner's block with "no progress" on it
 *                    (ga_bot_quest_block, GuildAdventure.Quests.LearnedBlockBots), so the others need not try.
 * The veto sits in the core's Player::CanTakeQuest (core patch CP-9, PlayerScript::OnPlayerCanTakeQuest) and applies to
 * characters the AI plays only: a character its owner plays takes any quest, and a quest that is already in the log
 * stays there (the planner has no task for it; the quest log care drops it when the log fills up).
 *
 * Load() runs on the world thread; the hook reads from map threads, so the tables sit behind a shared mutex.
 */

#ifndef MOD_GUILD_ADVENTURE_QUEST_SUPPORT_H
#define MOD_GUILD_ADVENTURE_QUEST_SUPPORT_H

#include "Define.h"
#include "ObjectGuid.h"
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;
class Quest;

namespace GuildAdventure
{
    enum class QuestVerdict : uint8
    {
        Supported,
        Listed,
        ScriptedEvent,
        Learned
    };

    char const* ToString(QuestVerdict verdict);
}

class QuestSupport
{
public:
    static QuestSupport& Instance();

    // World thread: at startup and on .ga reload.
    void Load();

    // Any thread.
    [[nodiscard]] GuildAdventure::QuestVerdict GetVerdict(uint32 questId) const;
    [[nodiscard]] std::string GetReason(uint32 questId) const;
    // The CanTakeQuest veto: false for a character the AI plays and a quest the planner leaves alone.
    [[nodiscard]] bool MayTake(Player* player, Quest const* quest) const;
    // Escort quests the planner can do (the quest giver walks a waypoint path once the quest is taken): the entry of
    // that NPC, 0 for any other quest.
    [[nodiscard]] uint32 GetEscortNpc(uint32 questId) const;
    // True when this character leaves the quest alone: the verdict, or an escort or event quest while the character's
    // bot setting Quests.Escorts / Quests.Events (BotSettings) is off. Any thread.
    [[nodiscard]] bool IsLeftAlone(Player const* player, uint32 questId) const;

    // A quest the planner abandoned to keep a character's quest log open is not taken again by that character for a
    // while (MayTake and the planner's own pickups ask). Without it the giver next to the bot handed the quest out
    // again at once, the log care dropped the next one, and so on: a hunter took and abandoned "The Green Hills of
    // Stranglethorn" and "Chapter I" 126 times in a night, every two seconds until the rescue moved it. Any thread.
    void NoteDropped(ObjectGuid guid, uint32 questId, uint32 untilUnix);
    [[nodiscard]] bool WasDropped(ObjectGuid guid, uint32 questId) const;
    // (For ga_bot_wait: what a character dropped and until when.)
    [[nodiscard]] std::vector<std::pair<uint32, uint32>> DroppedBy(ObjectGuid guid) const;

    // World thread: the planner blocked the quest for a bot (second failure, wowbot335 A21). With the reason
    // "no_progress" it counts towards "learned": enough different bots, and no AI-played character takes it any more.
    void OnQuestBlocked(uint32 questId, uint32 botLowGuid, std::string const& reason);
    // World thread: forgets every block the planner learned, per bot (ga_bot_quest_block) and for everyone. For use
    // after the planner learned new kinds of quests; the listed and the scripted ones stay. Returns the rows deleted.
    uint32 ForgetLearned();

    // For .ga admin quests: counts per verdict and the learned quests.
    [[nodiscard]] std::vector<std::string> Describe() const;
    // For .ga admin quests scripted: the zone quests up to a quest level that are left alone as script events.
    [[nodiscard]] std::vector<std::string> DescribeScripted(uint32 maxLevel) const;

private:
    QuestSupport() = default;

    struct Entry
    {
        GuildAdventure::QuestVerdict verdict = GuildAdventure::QuestVerdict::Supported;
        std::string reason;
    };

    mutable std::shared_mutex _lock;
    std::unordered_map<uint32, Entry> _entries;                           // only quests that are not supported
    std::unordered_map<uint32, std::unordered_set<uint32>> _blockedBy;    // quest -> bots blocked with "no_progress"
    std::unordered_map<uint32, uint32> _escorts;                          // escort quest -> the NPC that walks
    std::unordered_map<uint64, uint32> _dropped;                          // (character, quest) -> unix time it may be taken again
};

#define sQuestSupport QuestSupport::Instance()

#endif
