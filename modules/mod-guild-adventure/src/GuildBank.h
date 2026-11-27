/*
 * GuildBank: the adventure guild's gold in its WoW guild bank, and the guild tax.
 *
 * The owner, 2026-11-13: [quote left out]
 *
 * The guild bank is the native bank money of the adventure guild's WoW guild (Guild::GetTotalBankMoney), the number
 * the addon's Overview shows as "Guild bank". Until 2026-11-13 nothing of the module ever put gold there. Now:
 *   - the guild tax (bot setting Guild.TaxPercent, guild-wide): that share of every copper a roster bot earns
 *     (looted coin, quest rewards, what it sells, junk the loot filter sells, mail and trade money from outside the
 *     guild) is held back at the moment of the gain and booked into the guild bank by itself, a line each (the owner,
 *     2026-11-14: [quote left out]). Not taxed, being no income:
 *     the guild bank's own gold (its part of a purchase, a withdrawal from the guild bank), money the owner's
 *     characters or other characters of the guild hand a bot by trade or mail (Guild.TaxHandovers), the refunds of
 *     the auction house (outbid, cancelled). The leader pays no tax, whoever plays it (the owner, 2026-11-14);
 *   - what the guild storage sells (Sell / All / Sell junk on the Storage tab, the leader's auctions of the storage's
 *     surplus) goes into the guild bank;
 *   - the guild's part of a purchase (gear, quest items: GuildBudget) comes out of it.
 * Every movement leaves a line in the activity log (module.ga.activity).
 *
 * Threads: Deposit, Withdraw and Money on the world thread; OnMoneyChanged on any thread (Player::ModifyMoney runs on
 * the character's map thread): it holds the tax back and queues the booking, Update (world thread, every world tick)
 * books every queued gain into the guild bank with its own activity line ("guild tax 50% of 10s 4c Hunterbm got
 * (looted coin)"). Until 2026-11-14 the queue was booked once a minute as one sum, so the guild bank moved in steps.
 */

#ifndef MOD_GUILD_ADVENTURE_GUILD_BANK_H
#define MOD_GUILD_ADVENTURE_GUILD_BANK_H

#include "Define.h"
#include <string>

class Guild;
class Player;

namespace GuildAdventure
{
    namespace GuildBank
    {
        // The WoW guild of an adventure guild (nullptr when it has none).
        Guild* WowGuildOf(uint32 owner);
        // Its bank money in copper (0 without a WoW guild).
        uint64 Money(uint32 owner);
        // Puts copper into the guild bank / takes it out (false: no WoW guild, or not that much in the bank). `why`
        // names it in the activity log line ("tax", "sold 20 x Linen Cloth in the auction house", ...).
        bool Deposit(uint32 owner, uint64 copper, std::string const& why);
        bool Withdraw(uint32 owner, uint64 copper, std::string const& why);

        // While one lives on this thread, money a character gets is not taxed (the guild bank's own part of a
        // purchase, handed to the buyer).
        struct NoTax
        {
            NoTax();
            ~NoTax();
            NoTax(NoTax const&) = delete;
            NoTax& operator=(NoTax const&) = delete;
        };

        // While one lives on this thread, money a character gets is named so in the guild tax's line ("junk the loot
        // filter sold", "ammunition sold"). The innermost one counts.
        struct IncomeSource
        {
            explicit IncomeSource(char const* what);
            ~IncomeSource();
            IncomeSource(IncomeSource const&) = delete;
            IncomeSource& operator=(IncomeSource const&) = delete;
        private:
            char const* _previous;
        };

        // PlayerScript::OnPlayerMoneyChanged (any thread): holds the guild tax back from what a roster bot earns and
        // queues its booking.
        void OnMoneyChanged(Player* player, int32& amount);
        // Hooks that name the next gain of a character (same thread, within a second): a quest reward
        // (OnPlayerQuestComputeXP comes right before the reward money), an item sold to a vendor (OnPlayerCanSellItem),
        // and a withdrawal from the WoW guild bank (GuildScript::OnMemberWitdrawMoney: the guild's own gold, no tax).
        void OnQuestReward(Player* player, uint32 questId);
        void OnVendorSale(Player* player, uint32 itemEntry);
        void OnGuildBankWithdrawal(Player* player, uint32 amount, bool repair);
        // World thread: books every queued tax into its guild bank, a line each, and pushes the new bank money.
        void Update(uint32 diff);
        // Shutdown: what is still held back goes in now.
        void Flush();
        // Tax held back and not yet in the guild bank, in copper.
        uint64 PendingTax(uint32 owner);
    }
}

#endif
