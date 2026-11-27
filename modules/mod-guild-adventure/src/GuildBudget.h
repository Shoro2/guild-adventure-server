/*
 * GuildBudget: what a guild's characters may spend at vendors on gear and quest items, and who pays it.
 *
 * The owner, 2026-11-12: [quote left out]; [quote left out]
 *
 * The owner, 2026-11-13: [quote left out]: until then "the guild paid" meant the purses of the leader and
 * the other bots in the world. Now the guild's part comes out of the WoW guild bank (GuildBank), and an empty guild
 * bank pays nothing.
 *
 * Who pays: the buyer GuildAdventure.Budget.MemberPercent of its own money at most; what is missing the guild bank
 * pays, within two numbers (bot settings, the leader's):
 *   - Budget.ItemPercent: the guild bank pays at most this share of its money for one item;
 *   - Budget.BotAllowance: the copper the guild bank pays for one guild character over its whole life (table
 *     ga_bot_budget holds what it has paid for each).
 * A purchase the buyer's own share covers needs no guild money and books nothing on the allowance.
 *
 * One helper for every buyer in the module (gear: GearUpkeep; quest items sold by vendors):
 *
 *     GuildBudget::Grant grant = GuildBudget::Request(buyer, price);
 *     if (!grant.ok) -> not within the budget, or the guild cannot pay it now
 *     ... buy with the buyer's own money as usual (it now holds at least `price` copper) ...
 *     if the purchase failed: GuildBudget::Cancel(buyer, price, grant);
 *
 * World thread only.
 */

#ifndef MOD_GUILD_ADVENTURE_GUILD_BUDGET_H
#define MOD_GUILD_ADVENTURE_GUILD_BUDGET_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <utility>
#include <vector>

class Player;

namespace GuildAdventure
{
    struct AdventureGuild;

    namespace GuildBudget
    {
        struct Grant
        {
            bool ok = false;
            uint32 fromGuild = 0;      // copper the guild bank handed to the buyer
            std::vector<std::pair<ObjectGuid, uint32>> givers; // unused since the guild bank pays (kept for callers)
        };

        // Reads what the guilds' characters have spent (at startup).
        void Load();

        // What a guild character has spent of its allowance so far, and what is left of it, in copper.
        uint32 SpentBy(ObjectGuid guid);
        uint32 AllowanceLeft(Player const* buyer);

        // The most one item may cost in the buyer's guild right now, in copper.
        uint32 ItemLimit(Player const* buyer);

        // What the leader and the roster bots in the world carry, in copper (not the guild's money any more).
        uint64 TotalGold(AdventureGuild const& guild);
        uint64 TotalGold(Player const* buyer);
        // The money in the buyer's guild bank, in copper.
        uint64 BankMoney(Player const* buyer);

        // The most the guild bank would pay of one purchase by this buyer right now: its limit for one item, what is
        // left of the buyer's allowance, and what is in the bank.
        uint32 GuildPartAvailable(Player const* buyer);
        // The highest price Request would accept for this buyer right now, in copper: its own share plus
        // GuildPartAvailable.
        uint32 Available(Player const* buyer);

        // Hands the buyer the guild bank's part of `price` (what its own share does not cover) and books that part on
        // its allowance. ok = false (nothing changed) when the rules or the guild bank do not allow it, or the buyer is
        // in no guild.
        Grant Request(Player* buyer, uint32 price);

        // The purchase did not happen: the guild bank's part goes back into it, the booking is taken back.
        void Cancel(Player* buyer, uint32 price, Grant const& grant);

        // "1g 20s 5c".
        std::string MoneyText(uint32 copper);
    }
}

#endif
