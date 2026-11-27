/*
 * ClassAbilities: what a class only gets from its class quests (the design log section 7, D24; docs/orders.md "Class
 * abilities").
 *
 * In 3.3.5a a hunter's pet handling, a warlock's voidwalker, succubus and felhunter, a warrior's defensive and
 * berserker stance, a druid's bear form, a paladin's Redemption and a shaman's totems (the four totem items and the
 * first totem spells) come from class quests, not from the class trainer. The order planner does not plan class quests
 * (they belong to no zone, and most need "use this item on that creature"), mod-playerbots teaches their rewards only
 * to random bots, and so a guild bot never got them: a level-20 hunter had no pet.
 *
 * Like the class trainer's spells (D22) they are given where the bot is, when it reaches the level: the rows of the
 * world table ga_class_ability (class, race, level, a spell or an item or the hunter's first pet).
 *
 * World thread only.
 */

#ifndef MOD_GUILD_ADVENTURE_CLASS_ABILITIES_H
#define MOD_GUILD_ADVENTURE_CLASS_ABILITIES_H

#include "Define.h"
#include <string>

class Player;

namespace GuildAdventure::ClassAbilities
{
    // Reads ga_class_ability (startup, `.ga reload`); rows that name an unknown spell, item or creature are skipped
    // with an error line.
    void Load();

    // Gives the character every row of its class and race up to its level that it does not have yet. Returns what it
    // gave, for the log ("" = nothing new).
    std::string Grant(Player* player);
    // True while a hunter is due its good pet and has not got it (its pet was not at its side): look again soon.
    bool IsPetPending(Player* player);

    // Teaches the character every weapon skill its class and race may learn that it lacks (the trainers' own check,
    // Player::IsSpellFitByClassAndRace), at its maximum for the level. mod-autolearn-skills does this on a level-up
    // from a table per class that leaves gaps (paladins get no axes, rogues no maces or bows, mages and warlocks
    // nothing), so a guild character could not use an upgrade lying in the guild storage. Returns the skills learned
    // ("" = none). For guild characters, the leader included (RosterScripts: login, level-up).
    std::string LearnWeaponSkills(Player* player);
    // A rogue's Lockpicking up to five points a level, 300 at 60 (the owner, 2026-11-26: [quote left out]): Pick Lock taught from level 16 when it lacks it, the
    // skill raised to the level's maximum. Returns what changed ("" = nothing). Login and level-up (RosterScripts).
    std::string RaiseLockpicking(Player* player);

    // Spends the free talent points of a hunter's pet along the fixed build of its tree (the owner, 2026-11-13: [quote left out]): Ferocity and Cunning for damage, Tenacity for tanking, in an order
    // that keeps every row's 3-points-per-row rule (docs/orders.md "Class abilities"). mod-playerbots spends pet points
    // only in its level-up auto pick, which is off for guild bots (their talents follow the talent templates), and its
    // premade pet builds are endgame lists in no fixed order. A pet whose talents are not a part of its build (the
    // owner chose them while he played the hunter) is left alone. Callers: the order planner, so only for characters
    // the AI plays; never while the owner plays the hunter. Returns the log text ("" = nothing spent).
    std::string SpendPetTalents(Player* player);
}

#endif
