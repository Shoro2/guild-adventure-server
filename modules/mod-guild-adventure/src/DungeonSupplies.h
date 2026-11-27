/*
 * mod-guild-adventure: what a dungeon group uses out of the guild storage during a run (docs/orders.md "The dungeon run
 * window", stage C; the owner, 2026-11-13: [quote left out],
 * [quote left out]).
 *
 * The guild leader ticks kinds of items in the run window's preparation step; only those are used, by the bots of
 * that group, for that order. A bot takes one piece out of the guild storage when it would help it and it does not
 * have its effect yet: an elixir or flask (one battle and one guardian elixir at a time), a scroll, a stone or oil
 * for its weapon (a temporary enchantment), a scope for a hunter's ranged weapon that has none. Every piece taken
 * is a line of the activity log.
 */

#ifndef MOD_GUILD_ADVENTURE_DUNGEON_SUPPLIES_H
#define MOD_GUILD_ADVENTURE_DUNGEON_SUPPLIES_H

#include "Define.h"
#include <string>
#include <vector>

class Player;
struct ItemTemplate;
class SpellInfo;

namespace GuildAdventure
{
    enum class SupplyKind : uint8
    {
        None = 0,
        Elixir = 1,     // elixirs and flasks
        Scroll = 2,     // scrolls that leave a buff
        WeaponCoat = 3, // sharpening stones, weightstones, oils: a temporary enchantment of a weapon
        Scope = 4       // a scope: a lasting enchantment of a bow, gun or crossbow
    };

    struct SupplyItem
    {
        uint32 entry = 0;
        SupplyKind kind = SupplyKind::None;
        uint32 amount = 0;         // in the guild storage now
    };

    namespace DungeonSupplies
    {
        // What an item is for a run (None: nothing a run uses); `spell` gets its use spell.
        SupplyKind Classify(ItemTemplate const* proto, SpellInfo const** spell = nullptr);
        // What the guild storage of `owner` holds for a run.
        std::vector<SupplyItem> List(uint32 owner);
        // Uses what `allowed` lists (item entries) for the bot, out of its guild's storage: every piece that helps it
        // and whose effect it lacks. Not in a fight. Returns what it used, for the log ("" = nothing). `purpose` ends
        // the log line of each piece (the levelling bots use the same routine: StorageSupply::UseBuffs).
        // `role` (0 damage, 1 tank, 2 healer; -1 = no choice): for a dungeon run the character uses only what fits it
        // best: of the elixirs one battle and one guardian elixir (or a flask), of the scrolls one, each rated by the
        // stat weights of its role, as its gear is; stones, oils and scopes by its weapons, the strongest first.
        std::string Use(Player* bot, std::vector<uint32> const& allowed, char const* purpose = "for its dungeon run", int8 role = -1);
        // Whether the buff of an elixir or scroll does something for the way the bot fights.
        bool Helps(Player* bot, SpellInfo const* spell);
    }
}

#endif
