/*
 * GearUpkeep: AI-played guild characters keep their equipment up where a vendor near them can help (docs/orders.md
 * "Gear and the guild's budget").
 *
 * Found on the integration server (2026-11-12): bots of level 15-19 wore items of average item level 8-15, the rogue
 * that died most a level-10 and a level-6 weapon; durability, abilities and talents were fine. The owner: most gear is
 * found or comes from the guild storage (StorageSupply); a vendor is the exception: [quote left out]. No
 * purchases at a distance.
 *
 *   - BuyAt: at every vendor a bot visits it buys the weapon and the armour pieces this vendor sells that
 *     mod-playerbots rates an upgrade for it, the best per place on the body, and puts them on.
 *   - SellsUpgrade: the planner sends a bot to a vendor within GuildAdventure.Gear.Buy.NearYards that sells it such
 *     a piece (OrderService::PlanUpkeep; the vendor is chosen by the risk rules like any other).
 *   - FillEmptySlots: armour in the bags goes on where the bot wears nothing, whatever its kind (mod-playerbots only
 *     puts on the armour kind of the class: a warrior below 40 leaves cloth shoulders in its bags and its shoulders bare),
 *     and so do necklaces, rings, trinkets, relics and a caster's off-hand piece (BotDriver::FillsEmptyPlace), which
 *     mod-playerbots never puts into a bare place.
 *
 * What a purchase may cost and who pays: GuildBudget. Every purchase the guild pays a part of is an event of the
 * guild ("GEAR").
 *
 * World thread only.
 */

#ifndef MOD_GUILD_ADVENTURE_GEAR_UPKEEP_H
#define MOD_GUILD_ADVENTURE_GEAR_UPKEEP_H

#include "Define.h"
#include <string>
#include <vector>

class Creature;
class Player;
struct ItemTemplate;

namespace GuildAdventure
{
    namespace Gear
    {
        // Pieces that compete for the same place on a character (0: nothing it wears for its fights). Weapons and what
        // is held in the hands are 100, ranged weapons 101, armour its inventory type (a robe counts as a chest piece).
        uint32 SlotGroup(ItemTemplate const* proto);

        // Buys what this vendor sells and the bot should wear (see above) and puts it on. Returns what it bought, e.g.
        // "Cutlass (22s 41c, the guild paid 14s)", empty when nothing.
        std::string BuyAt(Player* bot, Creature* vendor);

        // True if the vendor with that creature entry sells something the bot would buy now.
        bool SellsUpgrade(Player* bot, uint32 vendorEntry);

        // For ".ga admin gear": what the bot may spend now and what the vendors within `yards` sell that it would buy.
        std::vector<std::string> Describe(Player* bot, float yards);

        // Test aid (".ga admin gear <character> buy"): BuyAt at the vendor the character stands at, without the
        // planner's walk. Returns what it bought, or why nothing.
        std::string BuyHere(Player* bot);

        // What of the bot's gear is far below its level (GuildAdventure.Gear.WarnGap item levels or more): the weapon
        // its fights depend on, the armour on its body on average. Empty when nothing is, or below level 10.
        // E.g. "main hand Long Bayonet (item level 10), body armour item level 7 on average".
        std::string Warning(Player const* bot);

        // Puts armour (with GuildAdventure.Gear.FillEmptySlots) and jewellery (always) from the bags on where the
        // bot wears nothing. Returns the names, empty when nothing.
        std::string FillEmptySlots(Player* bot);
    }
}

#endif
