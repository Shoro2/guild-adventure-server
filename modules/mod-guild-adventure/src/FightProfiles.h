/*
 * FightProfiles: the bots' combat logic as profiles the guild leader can see and edit (docs/fight-profiles.md).
 *
 * mod-playerbots builds a bot's rotation from named strategies per class and talent tree; each strategy is a list of
 * rows "trigger -> action with a relevance", and the engine works the rows off as a priority queue. A fight profile
 * (FightProfileStore.h) is two such lists: the fight and the time between fights (the buffs). At run time a list is
 * the strategy "ga rotation" in the bot's combat engine, and "ga buffs" in its non-combat engine, in place of the
 * strategies it stands for (approach B, FC1); target choice, movement, the generic strategies and the module's own
 * fight rules stay. The strategy, its triggers and its actions are put into the bot's AI context by the module (as
 * BotDriver::GuardConsumables puts its actions there): no patch of mod-playerbots. Besides BotDriver this is the one
 * place of the module that touches playerbots internals.
 *
 * What a profile plays is its own rules plus the rows of the replaced strategies it does not own (movement and target
 * housekeeping, what the module's own rules cover, FC4, and in the buff list every row that is no buff, FC2): those
 * are read from the bot's strategies when the profile goes in and emitted as they are.
 *
 * A bot plays its group profile on a dungeon or raid map and as a member of a dungeon run, else its solo profile; an
 * empty slot plays the default of mod-playerbots, as does a profile of another talent tree than the bot's (FC3).
 * A profile under which the bot does nothing for 10 s in a fight is suspended until it is saved or assigned again, and
 * The owner gets an event (FC9). Changes go in between fights.
 */

#ifndef MOD_GUILD_ADVENTURE_FIGHT_PROFILES_H
#define MOD_GUILD_ADVENTURE_FIGHT_PROFILES_H

#include "FightProfileStore.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

class Player;
class Spell;

namespace GuildAdventure::FightProfiles
{
    // A rule as the engine gets it: options filled in (the bot's own value first), and whether the profile owns it.
    struct PlayedRule
    {
        Rule rule;
        bool hidden = false;  // a row of the replaced strategies that the profile does not own: emitted as it is
    };

    // What a character plays in one engine (0 the fight, 1 between fights).
    struct Played
    {
        std::vector<std::string> replaced;  // the strategies of that engine it stands for
        std::vector<PlayedRule> rules;
        uint32 profileId = 0;               // 0: the template, read from the bot's strategies
        uint32 revision = 0;
        std::string name;
        Slot slot = Slot::Solo;
        bool byDefault = false;             // the default template of its class and tree, not a profile of its slot
    };

    // The strategies a list stands for in the character's engine now (sorted).
    std::vector<std::string> StandStrategies(Player* player, uint8 engine);

    // The template of one engine: the rows of those strategies, each marked whether a profile owns it. False (with a
    // reason) for a character without an AI.
    bool Template(Player* player, uint8 engine, Played& out, std::string& why);
    // A new profile of the character's class and talent tree from its two templates: the rows a profile owns.
    bool CopyTemplate(Player* player, StoredProfile& out, std::string& why);
    // The rows of every strategy of one of its engines, for a look at what is there.
    std::vector<std::string> DumpEngine(Player* player, uint8 engine);

    std::string RuleText(Rule const& rule);
    std::vector<std::string> DescribePlayed(Played const& played);
    std::vector<std::string> DescribeStored(StoredProfile const& profile);
    // What a character plays now and why, its slots and its option values (".ga fight").
    std::vector<std::string> DescribeBot(Player* player);

    // What a character plays by now in an engine (nullptr: the default).
    Played const* Active(Player* player, uint8 engine);
    // Why the character does not play the profile of its slot (empty: it does, or it has none).
    std::string Refusal(ObjectGuid guid);
    // The talent tree a profile is made for and asked against (mod-playerbots' tree of the character).
    uint8 TalentTree(Player* player);
    // The profile a character plays while its slot has none (the owner, 2026-11-27: [quote left out], the design log §7 FC12): the wowbot335 template of its class and talent tree while the bot setting
    // Fight.TemplatesByDefault is on; in a dungeon or raid group (groupSlot) not for a tank or a healer by its talents
    // (the templates are solo fight classes of damage specs). 0: the rotation of mod-playerbots.
    uint32 DefaultFor(Player* player, bool groupSlot);

    // Every planner step of an AI-played character out of a fight: puts the profile of its slot in (groupSlot: on a
    // dungeon or raid map, or a member of a dungeon run), takes it out, or puts it in again after a strategy reset.
    void Keep(Player* player, bool groupSlot);
    // Every planner step of an AI-played character in a fight: the fallback of FC9.
    void WatchFight(Player* player);
    // Takes a character's profiles out at once (its AI goes away; a command).
    void Forget(Player* player);

    // Counting what characters cast in their fights (the spike's comparison, and the fight line of a profile): any thread.
    void StartCounting(Player* player);
    void NoteCast(Player* player, Spell const* spell);
    void NoteCombat(Player* player, bool entered);
    std::vector<std::string> DescribeCounts(Player* player);
}

#endif
