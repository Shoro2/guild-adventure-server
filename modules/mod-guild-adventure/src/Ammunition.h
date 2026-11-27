/*
 * mod-guild-adventure: the ammunition of the guild's characters.
 *
 * The owner, 2026-11-13: [quote left out].
 *
 * What follows from it, for every guild character the AI plays:
 *  - A stock in rounds (Ammo::Stock): a hunter 2000, below level 20 1000, below level 5 200; any other class with a
 *    bow, crossbow or gun one stack. No purchase goes beyond it: the vendor hook refuses mod-playerbots' own purchases
 *    (it would buy a hunter eight stacks and anyone else two) as well as anything else.
 *  - A hunter tops up at a vendor it visits (Ammo::BuyAt); the planner sends it to one in reach of a hub after a
 *    hand-in, and from anywhere below GuildAdventure.Ammo.RestockBelow rounds (OrderService::PlanUpkeep).
 *  - A hunter takes the best ammunition for its weapon out of the guild storage (StorageSupply), and buys none while
 *    the storage holds some for it.
 *  - Ammunition gets into the guild storage only when a guild character made it (StorageService::DepositItem refuses
 *    it otherwise; the loot filter and the bag sweeps never store it).
 *  - What a character carries beyond that is sold at vendor price (Ammo::Trim): what its weapon does not shoot or it
 *    cannot use, and what is over its stock, the worst kind first. Most ammunition below level 40 has no vendor
 *    price: it is deleted.
 */

#ifndef MOD_GUILD_ADVENTURE_AMMUNITION_H
#define MOD_GUILD_ADVENTURE_AMMUNITION_H

#include "Define.h"
#include <string>
#include <vector>

class Creature;
class Player;
struct ItemTemplate;

namespace GuildAdventure::Ammo
{
    // What a ranged weapon shoots (the item subclass of the projectile: arrows for a bow or crossbow, bullets for a
    // gun); 0: nothing (thrown, a wand, no weapon). For a character: the weapon in its ranged slot.
    uint32 KindOf(ItemTemplate const* weapon);
    uint32 KindOf(Player const* player);

    // Whether `a` is the better ammunition (damage, then required level, then item level).
    bool Better(ItemTemplate const* a, ItemTemplate const* b);

    // Ammunition the character's ranged weapon shoots and the character can use now.
    bool Fits(Player const* player, ItemTemplate const* proto);

    // The rounds the character carries at most. `proto` (ammunition) gives the stack size for the classes that carry
    // stacks; without it a stack is 1000.
    uint32 Stock(Player const* player, ItemTemplate const* proto = nullptr);

    // The fitting rounds in its bags, and the best kind among them (0: none).
    uint32 Count(Player const* player, uint32* best = nullptr);
    // The fitting rounds in its bags that are no worse than `proto`.
    uint32 CountAtLeast(Player const* player, ItemTemplate const* proto);

    // The best fitting ammunition any vendor of the world sells the character for gold (nullptr: none). What it buys
    // at a vendor that has it; what an engineer makes and what it takes out of the guild storage is never worse.
    ItemTemplate const* VendorBest(Player const* player);

    // The best ammunition in the guild storage this hunter would take: usable with its weapon and no worse than the
    // best it carries nor than the best a vendor sells it (0: none; `amount` = how much the storage holds of it).
    // StorageSupply takes it within a minute.
    uint32 BestInStorage(Player const* player, uint32* amount = nullptr);
    bool InStorage(Player const* player);

    // Whether a purchase of that many rounds is within the rules (the vendor hook's answer for an AI-played character).
    bool MayBuy(Player const* player, ItemTemplate const* proto, uint32 rounds);

    // The best fitting ammunition a vendor (creature entry) sells for money (0: none).
    uint32 VendorSells(uint32 vendorEntry, Player const* player);

    // A hunter at a vendor: fills its stock with the best ammunition the vendor sells, with its own money (below the
    // restock threshold and without money: one bundle from the guild's budget). The text for the log, "" for nothing.
    std::string BuyAt(Player* player, Creature* vendor);

    // Parts with what the character must not carry (see above), out of a fight only. Ammunition a guild character
    // made (a kind a recipe makes) and as much of a kind as the character took out of the guild storage (NoteTaken)
    // go back into the storage, with a log line of their own; the rest is sold at vendor price. The text of the sale
    // for the log, "" for none.
    std::string Trim(Player* player);

    // StorageSupply: the character took that much of a kind out of the guild storage.
    void NoteTaken(Player const* player, uint32 entry, uint32 count);

    // The ammo slot shoots the best fitting kind the character carries.
    void UseBest(Player* player);

    // .ga admin ammo: how the rules judge a character, and (test aid) a purchase at the vendor it stands at.
    std::vector<std::string> Describe(Player* player);
    std::string BuyHere(Player* player);
}

#endif
