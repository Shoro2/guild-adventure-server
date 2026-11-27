/*
 * mod-guild-adventure: the poisons of the guild's rogues.
 *
 * The owner, 2026-11-13: [quote left out] In 3.3.5a
 * there is nothing to learn: since patch 3.1 poisons are items a poison vendor sells from level 20 (Instant Poison at
 * 20, Deadly Poison at 30, a new rank every eight levels), applied to a weapon by using them; the items need no skill
 * and the old Poisons skill (spell 2842) only opens an empty crafting window. mod-playerbots applies them by itself
 * (GenericRogueNonCombatStrategy / GenericRogueStrategy: Instant Poison on the main hand, Deadly Poison on the off
 * hand, the one it carries on both hands when it carries one kind) whenever a weapon has no temporary enchantment,
 * but nothing ever gave a guild rogue a poison: it gives them only to random bots (PlayerbotFactory::InitReagents),
 * and the order planner bought none.
 *
 * What follows, for every rogue the AI plays from level 20:
 *  - It carries a stock of the poisons of its pair (GuildAdventure.Poisons.Pair: Instant on the main hand and Deadly
 *    on the off hand, or Instant on both, or Deadly on both; Deadly only from level 30, Instant before that) of
 *    GuildAdventure.Poisons.Stock doses per hand (10: one dose lasts an hour), counting every rank it can use.
 *  - The guild storage first: below its stock it takes what the storage holds of the kinds it uses, the best rank
 *    first, once a minute with the other supplies (Poisons::TakeFromStorage), a log line per take.
 *  - Otherwise it buys the best rank its level allows at a poison vendor on its normal shopping stop: at a vendor it
 *    visits anyway, and at a hub after a hand-in when a poison vendor stands there and it carries less than half its
 *    stock (the owner's rule for shopping: at a hub after a hand-in, no vendor trips from the field). Its own money first,
 *    the guild's budget for what it cannot pay (GuildBudget).
 *  - mod-playerbots puts the poisons on (out of combat and in a fight, whenever a weapon has none), so the module
 *    never overwrites a poison or another temporary enchantment; the dungeon preparation (DungeonSupplies) likewise
 *    only coats a weapon that has none, so the two never fight over a weapon.
 *
 * World thread only.
 */

#ifndef MOD_GUILD_ADVENTURE_ROGUE_POISONS_H
#define MOD_GUILD_ADVENTURE_ROGUE_POISONS_H

#include "Define.h"
#include <string>

class Creature;
class Player;

namespace GuildAdventure::Poisons
{
    // True for a rogue of level 20 or more with the setting on and a weapon in a hand.
    bool Uses(Player const* player);

    // True when it carries less than half its stock of a kind it uses and the guild storage holds none of that kind
    // for it: worth a visit to a poison vendor at a hub after a hand-in (OrderService::PlanUpkeep).
    bool WantsTopUp(Player const* player);

    // True when it carries no poison of the kinds it uses at all and a weapon in its hands has no temporary
    // enchantment: worth a poison vendor at any stop in a town, not only after a hand-in (OrderService::PlanPoisonStop).
    bool Bare(Player const* player);

    // True if the vendor sells a poison of a kind the rogue uses that it can use now.
    bool VendorSells(uint32 vendorEntry, Player const* player);

    // Takes what it lacks of its stock out of the guild storage, the best usable rank first, an activity line per
    // take. Returns the doses taken.
    uint32 TakeFromStorage(Player* player);

    // At a vendor it visits: buys what it lacks of its stock, the best rank the vendor sells that its level allows,
    // unless the guild storage holds that kind for it. Returns the log text ("" = nothing bought).
    std::string BuyAt(Player* player, Creature* vendor);

    // For `.ga admin poisons`: the poisons it uses, carries and has on its weapons.
    std::string Describe(Player const* player);
}

#endif
