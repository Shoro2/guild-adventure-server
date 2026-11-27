/*
 * ShoppingList: the guild storage's shopping list (the owner, 2026-11-14: [quote left out]; docs/storage.md "Shopping list").
 *
 * Per guild the owner sets a minimum stock for items vendors sell (the Storage tab's Shopping list view). Whenever an
 * AI-played guild character visits a vendor anyway (the planner's vendor visit: selling, repairing, restocking, a
 * material or a poison), it looks at the list: for every item this vendor sells and the guild storage holds fewer of
 * than its minimum, it buys what is missing, paid from the WoW guild bank, and the goods go straight into the guild
 * storage (a line in the activity log and in Recent loot, outcome "Bought"). No trip is made for the list.
 *
 * Bounds: a guild-wide budget per vendor visit (Shopping.MaxPerVisit) and per day (Shopping.MaxPerDay); what the guild
 * bank cannot pay is skipped with a log line, never paid from the bot's own gold unless Shopping.FromPurse says so.
 * Purchases happen on the world thread one after the other, and the storage counts each at once, so two bots at the
 * same vendor never both buy the whole shortfall.
 *
 * World thread only (the planner, the protocol and the commands run there).
 */

#ifndef MOD_GUILD_ADVENTURE_SHOPPING_LIST_H
#define MOD_GUILD_ADVENTURE_SHOPPING_LIST_H

#include "Define.h"
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class Creature;
class Player;
struct ItemTemplate;

namespace GuildAdventure
{
    struct ShoppingEntry
    {
        uint32 entry = 0;
        uint32 min = 0;            // the guild storage is filled up to this many
        bool enabled = true;
    };

    // An item some vendor sells for gold (the search of the Shopping list view).
    struct ShoppingItem
    {
        uint32 entry = 0;
        std::string name;
        uint32 quality = 0;
        uint32 requiredLevel = 0;
        uint32 price = 0;          // copper for one bundle at a vendor (before a reputation discount)
        uint32 bundle = 1;         // items one purchase gives (BuyCount)
        bool alliance = false;     // a vendor the Alliance can trade with sells it
        bool horde = false;
    };
}

class ShoppingList
{
public:
    static ShoppingList& Instance();

    // Startup (world thread, after the guilds): the lists, today's spending, and the index of what vendors sell.
    void Load();

    // --- the list (protocol and commands) ---
    std::vector<GuildAdventure::ShoppingEntry> Get(uint32 owner) const;
    // Error codes: NOT_SOLD (no vendor of the guild's side sells it for gold), NOT_STORED (the guild storage does not
    // take it: quest items, soulbound, ammunition), INVALID (minimum above 100000), LIMIT (MaxEntries).
    bool Set(uint32 owner, uint32 team, GuildAdventure::ShoppingEntry entry, std::string& error);
    bool Delete(uint32 owner, uint32 entry);
    static constexpr std::size_t MaxEntries = 60;
    static constexpr uint32 MaxMinimum = 100000;

    // Why an item cannot be on the list (nullptr = it can).
    char const* RefuseReason(ItemTemplate const* proto, uint32 team) const;
    // Items vendors sell whose name contains `text` (or whose id it is), best match first.
    std::vector<GuildAdventure::ShoppingItem> Search(std::string const& text, uint32 team, std::size_t max) const;
    GuildAdventure::ShoppingItem const* FindItem(uint32 entry) const;

    // A guild character at a vendor: buys what the list misses and this vendor sells. Returns a summary for the
    // activity log ("" = nothing bought); `skipped` gets why something was not bought (the bank, a budget).
    std::string BuyAt(Player* player, Creature* vendor, std::string* skipped = nullptr);
    // Test aid (.ga admin shopping buy): BuyAt at the vendor the character stands at.
    std::string BuyHere(Player* player);
    // While a purchase for the list runs on this thread: the vendor hooks that hold a bot's own food and drink
    // purchases back (Ammunition.cpp: not while the guild storage has some for it) let it through, it buys for the storage.
    static bool IsBuying();

    // Copper the guild spent on its list today (server days), and the day's budget.
    uint64 SpentToday(uint32 owner);

private:
    ShoppingList() = default;

    struct Day
    {
        uint32 day = 0;
        uint64 spent = 0;
    };

    void Save(uint32 owner, GuildAdventure::ShoppingEntry const& entry) const;
    Day& Today(uint32 owner);
    // A line about something not bought, at most once per guild and item in half an hour.
    bool MayNote(uint32 owner, uint32 entry, char const* what);

    std::unordered_map<uint32, std::map<uint32, GuildAdventure::ShoppingEntry>> _lists;   // by owner account
    std::unordered_map<uint32, Day> _days;                                                 // by owner account
    std::unordered_map<uint32, GuildAdventure::ShoppingItem> _vendorItems;                 // by item entry
    std::map<std::pair<uint32, std::string>, uint32> _notes;                                // (owner, what) -> time
};

#define sShoppingList ShoppingList::Instance()

#endif
