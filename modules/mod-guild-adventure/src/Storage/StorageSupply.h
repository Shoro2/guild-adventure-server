/*
 * StorageSupply: AI-played guild characters take what they need out of the guild storage (docs/storage.md
 * "Supplies for the bots", the design log section 7, DQ3: [quote left out]; DQ11, the owner, 2026-11-13: [quote left out]).
 *
 *   - Consumables: one piece at the moment it is used, never a stock. Food and drink in a rest, a bandage in a rest
 *     for a character with First Aid, the best healing or mana potion in a fight when the bot is low (TakeOne, called
 *     by the rest and the fight steps of OrderService); the best scroll of the bot's main stat and an elixir of each
 *     kind while it works on a task with fights (UseBuffs). Food and drink the bot carries are used up first (what
 *     it bought at a vendor); a potion or bandage is the better of what it carries and what the storage has. A mage
 *     that conjures its water or food takes none of that kind.
 *   - Equipment: a weapon or armour piece in the storage that mod-playerbots rates as an upgrade for the bot (the same
 *     rating that makes it equip loot) is taken and put on. What it wore before stays in its bags and goes into the
 *     storage with the next bag sweep, unless the bot still has a use for it.
 *   - Ammunition for a hunter (Ammunition.h).
 *
 * Storing is unchanged: the loot filter already keeps what a bot will use itself (S3) and sends the rest to the storage.
 *
 * World thread only, except Has and Conjures (any thread: the vendor hook asks them).
 */

#ifndef MOD_GUILD_ADVENTURE_STORAGE_SUPPLY_H
#define MOD_GUILD_ADVENTURE_STORAGE_SUPPLY_H

#include "Define.h"
#include <string>

class Item;
class Player;

namespace GuildAdventure::StorageSupply
{
    // The consumables a bot takes one piece of at the moment it uses it.
    enum class Kind : uint8
    {
        Food,
        Drink,
        HealingPotion,
        ManaPotion,
        Bandage
    };

    // Takes equipment upgrades and a hunter's ammunition for the character out of its guild's storage (about once a
    // minute, out of combat). Returns what it took, for the log ("" = nothing).
    std::string Step(Player* bot);

    // The piece of that kind the bot uses now, in its bags: what it carries, or one piece taken out of its guild's
    // storage for this use when the bot carries none (food, drink) or the storage has a better kind (potions,
    // bandages). A piece taken out is a line of the activity log with `purpose` ("to eat"). nullptr: neither has one.
    Item* TakeOne(Player* bot, Kind kind, char const* purpose);

    // Whether the guild storage holds a piece of that kind the character can use and may take (its settings; not a
    // mage for what it conjures). Bags do not count. Answers are kept for some seconds.
    bool Has(Player* bot, Kind kind);
    // Has(bot, Kind::Drink): the planner's walk to a vendor for drink waits for that.
    bool HasDrink(Player* bot);

    // A mage that knows Conjure Water (Kind::Drink) or Conjure Food (Kind::Food): it makes that kind itself, buys
    // none and takes none out of the storage (the owner, 2026-11-13: [quote left out]).
    bool Conjures(Player const* bot, Kind kind);

    // Buffs for a bot that works on a task with fights: the best scroll of its main stat and the best battle and
    // guardian elixir that help it, one piece each out of the guild storage when it lacks the effect (through
    // DungeonSupplies::Use, which writes the log line per piece). Not in a fight, not mounted, not in a dungeon (a
    // dungeon group uses what its leader ticked). Returns what it used ("" = nothing).
    std::string UseBuffs(Player* bot);
}

#endif
