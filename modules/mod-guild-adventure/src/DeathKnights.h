/*
 * DeathKnights: death knight recruits without the starting area, and the death knight's runes (the design log section 7
 * DK1-DK4; docs/orders.md "Death knights").
 *
 * The core creates a death knight at StartHeroicPlayerLevel (55) in Acherus (map 609), with the green Acherus starting
 * gear, no hearthstone and no Death Gate. A death knight leaves Acherus through its starting campaign: quests that are
 * almost all vehicles and script events (the Eye of Acherus, the stolen horse, the duels, the battle at Light's Hope),
 * which the order planner does not play, and Player::TeleportTo refuses every far teleport out of Acherus to a death
 * knight without Death Gate (50977). Its talent points come from those quests while it stands there
 * (Player::CalculateTalentsPoints). A recruited death knight so stood in Acherus for good (docs/orders.md, "Open
 * points" before 2026-11-14).
 *
 * The owner (2026-11-14): [quote left out]. That module's effect is built here
 * instead (it acts on a player's first login in Acherus, gives the campaign's experience and gold, a fixed weapon, level
 * 58 and teleports during the login): with the guild's bot setting DeathKnights.SkipStart a recruited death knight is
 * saved in its side's capital with its hearthstone bound there (no teleport), and at its first plan the starting
 * campaign counts as done: the quests are marked rewarded (no experience, no gold, no quest scripts), their reputation
 * and the spells their rewards teach (Death Gate, the Acherus Deathcharger, runeforging) are given, and, unless the
 * setting says "starting gear only", their item rewards, each piece put on where it beats what the bot wears (the
 * Acherus starting piece it replaces is destroyed: soulbound, worth copper). Outside Acherus the core gives it the
 * talent points of its level, which its talent template spends.
 *
 * Runes: a death knight enchants its own weapons at a runeforge (Acherus, Ebon Hold), which bots never visit, so the
 * planner puts the best rune a death knight the AI plays knows on a weapon that has no permanent enchantment, as the
 * runeforge would (bot setting ClassAbilities.Runeforging).
 *
 * World thread only.
 */

#ifndef MOD_GUILD_ADVENTURE_DEATH_KNIGHTS_H
#define MOD_GUILD_ADVENTURE_DEATH_KNIGHTS_H

#include "DatabaseEnvFwd.h"
#include "Define.h"
#include <string>

class Player;

namespace GuildAdventure::DeathKnights
{
    // Recruitment (RosterService::Recruit, between Player::Create and the save's commit): a new death knight of a guild
    // whose DeathKnights.SkipStart is not 0 is saved in its side's capital (Stormwind, Orgrimmar) with its hearthstone
    // bound at the inn there, by two statements appended to the save's transaction. Returns true when it was placed.
    bool PlaceRecruit(Player* player, uint32 ownerAccount, CharacterDatabaseTransaction trans);

    // True for a death knight outside Acherus whose starting campaign has not been counted yet (the last quest of it,
    // "The Light of Dawn", is not rewarded): a recruit PlaceRecruit put into the capital, before its first plan.
    bool IsStartPending(Player* player);

    // Counts the starting campaign as done (see the file comment). Callers: the order planner, for roster bots the AI
    // plays (the item choice and the "is it better" rating ask the bot's playerbots AI). withRewards: the campaign's
    // item rewards too. Returns the log text.
    std::string FinishStart(Player* player, bool withRewards);

    // Puts the best rune the death knight knows on each weapon it wears that has no permanent enchantment (tank runes
    // while its dungeon set as a tank is on). Returns the log text ("" = nothing done).
    std::string Runeforge(Player* player);
}

#endif
