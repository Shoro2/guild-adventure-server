/*
 * Squads: two or three guild bots level together (docs/orders.md "Squads").
 *
 * A single bot loses a fight against two foes of its level about every second time (bot quality analysis, 2026-11-12),
 * and the gear it finds keeps it below its level. A squad is one leader bot that plays its own order and one or two
 * members that follow it: playerbots master = the leader, strategy profile Follow, all in the leader's party.
 *
 *   - Storage: ga_bot_squad (member -> leader). A member's own order is kept but suspended while it follows; it plays its
 *     order again when it leaves the squad, while its leader is benched, offline or dead for good, and while the level
 *     gap to its leader is larger than GuildAdventure.Squads.MaxLevelGap.
 *   - XP and kill credit come from the core's party rules; the party loots free for all, each member its own quest items.
 *   - Quests (OrderService, OrderSquads.cpp): a member standing at a quest giver takes the quests its leader took there,
 *     and hands in completed quests where it stands at their ender; it plans no quests of its own.
 *   - The leader waits while a member near it rests, is dead and runs back to its corpse, fights, or lags behind.
 *   - Fights (FightEscape) and spots (RiskModel) count the squad: the escape estimate adds what the members near the
 *     leader can still take and the foes they fight, the danger limit of a spot grows with the squad.
 *   - Who forms squads: the owner, per bot (addon Orders sub-tab, GADV SQUAD_SET), and optionally the module itself
 *     (GuildAdventure.Squads.Auto: bots of the same zone within two levels).
 *   - Quest partners (QuestPartners.h): a squad for one hard quest, formed and split by the module; its links are held
 *     in memory only (never in ga_bot_squad), carry the quest, and whether the member has met its leader yet.
 *
 * World thread only.
 */

#ifndef MOD_GUILD_ADVENTURE_SQUADS_H
#define MOD_GUILD_ADVENTURE_SQUADS_H

#include "ObjectGuid.h"
#include <string>
#include <unordered_map>
#include <vector>

class Player;

namespace GuildAdventure
{
    struct AdventureGuild;
    struct GuildBot;

    // Where a squad member stands right now.
    enum class SquadState : uint8
    {
        None,       // in no squad
        Active,     // follows its leader
        Disabled,   // GuildAdventure.Squads.Enable = 0
        Benched,    // the leader is benched
        Away,       // the leader is not in the world
        LevelGap,   // the level gap to the leader is larger than GuildAdventure.Squads.MaxLevelGap
        Leads       // not a member: this bot leads a squad
    };

    // Protocol token (BOT^sqs): "none", "active", "off", "benched", "away", "gap", "leads".
    char const* ToToken(SquadState state);
    // A line for the why text and the commands.
    std::string Describe(SquadState state);
}

class SquadService
{
public:
    static SquadService& instance();

    // ga_bot_squad (characters). Startup and ".ga reload".
    void Load();

    // Puts `member` into the squad of `leader` (empty leader: out of its squad). Both are roster bots of the guild; a
    // squad has one leader and at most GuildAdventure.Squads.MaxSize characters; a leader follows nobody and a member
    // leads nobody. `error` says why not, for the owner.
    // `quest` != 0: a quest partner link (QuestPartners.h), not stored.
    // (While this is set, Set does not look at the levels: the group of a dungeon run, OrderService::StartDungeonGroup.)
    void AllowAnyLevel(bool on) { _anyLevel = on; }
    bool _anyLevel = false;
    bool Set(GuildAdventure::AdventureGuild& guild, ObjectGuid member, ObjectGuid leader, std::string& error, bool automatic = false,
        uint32 quest = 0);
    // The bot left the guild: its link and the links of its members go.
    void Forget(ObjectGuid bot);

    // The leader a member is linked to (empty: in no squad), whatever its state.
    [[nodiscard]] ObjectGuid GetLeader(ObjectGuid member) const;
    // The members linked to a leader.
    [[nodiscard]] std::vector<ObjectGuid> GetMembers(ObjectGuid leader) const;
    [[nodiscard]] bool IsAutomatic(ObjectGuid member) const;
    // Quest partners (QuestPartners.h): the quest of a member's partner link (0 = none), the quest of a partner link that
    // leads to `leader` (0 = none), the members with a partner link, whether a member has met its leader yet.
    [[nodiscard]] uint32 GetPartnerQuest(ObjectGuid member) const;
    [[nodiscard]] uint32 GetLedPartnerQuest(ObjectGuid leader) const;
    [[nodiscard]] std::vector<ObjectGuid> GetPartnerMembers() const;
    [[nodiscard]] bool HasMet(ObjectGuid member) const;
    // The game time a link was set (0 = no link).
    [[nodiscard]] uint32 GetSetAt(ObjectGuid member) const;
    void MarkMet(ObjectGuid member);

    // The state of a member now; `leaderPlayer` gets the leader's character when the member follows it (Active). The
    // leader counts while the AI plays it and while its owner plays it himself (the member then follows him).
    GuildAdventure::SquadState GetState(GuildAdventure::AdventureGuild const& guild, ObjectGuid member, Player** leaderPlayer = nullptr) const;
    // The members that follow the leader now and that the AI plays (in the world).
    [[nodiscard]] std::vector<Player*> GetFollowers(GuildAdventure::AdventureGuild const& guild, ObjectGuid leader) const;
    // The characters a guild character fights beside: for a leader its followers within `range`, for a following
    // member its leader and the other followers within `range`. Alive, on the same map.
    [[nodiscard]] std::vector<Player*> GetPartners(Player const* player, float range) const;
    // How many characters stand together in the squad of `player` (itself included, within `range`); 1 alone.
    [[nodiscard]] uint32 CountTogether(Player const* player, float range) const;

    // GuildAdventure.Squads.Auto: bots without a squad that quest in the same zone within two levels of each other form
    // squads by themselves (called about once a minute per guild).
    void AutoPair(GuildAdventure::AdventureGuild& guild);

    // Lines for ".ga squad".
    [[nodiscard]] std::vector<std::string> DescribeGuild(GuildAdventure::AdventureGuild const& guild) const;

private:
    SquadService() = default;

    struct Link
    {
        ObjectGuid leader;
        uint32 setAt = 0;
        bool automatic = false;
        uint32 quest = 0;    // a quest partner link: not stored
        bool met = false;    // ... the member has met its leader
    };

    void Save(ObjectGuid member, Link const& link) const;
    void Delete(ObjectGuid member) const;

    std::unordered_map<ObjectGuid, Link> _links; // member -> leader
};

#define sSquadService SquadService::instance()

#endif
