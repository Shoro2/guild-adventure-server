/*
 * mod-guild-adventure: craft orders of a guild (docs/orders.md "Craft orders").
 *
 * The owner, 2026-11-13: the guild leader sees to it that the storage holds what the bots need (wands for the casters),
 * and has items made on purpose: he picks an item and a count, a bot that knows a recipe for it makes it on its next
 * town trip from the guild storage's materials, and the result goes into the guild storage.
 *
 * An order is "make <count> x <item>" of a guild. A bot on a town trip (a vendor visit, a hand-in at a hub; also in
 * profession mode) that knows a recipe for an open order starts on it when the materials are there (its bags and the
 * guild storage), the tool the recipe asks for too (it makes the tool first when it can), and what a vendor sells of
 * the missing materials is bought at a vendor visit, within the guild's budget. What it made leaves its bags for the
 * guild storage when the crafting ends.
 *
 * The owner, 2026-11-13, later: [quote left out]. Whatever a character the AI plays
 * makes with a recipe (crafting for skill at a hub, the profession mode, the Crafting sub-tab, a quest item for
 * another member) leaves its bags for the guild storage when its crafting has ended; what stays with it: a tool, an
 * item an open quest of its own asks for, and a piece of gear it puts on. And: [quote left out] (DisenchantStored, on by default for craft orders).
 *
 * World thread only, except OnCreated (the map thread of the character that made something).
 */

#ifndef MOD_GUILD_ADVENTURE_CRAFT_ORDERS_H
#define MOD_GUILD_ADVENTURE_CRAFT_ORDERS_H

#include "Define.h"
#include "ObjectGuid.h"
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class Creature;
class Player;
class SpellInfo;

namespace GuildAdventure
{
    struct AdventureGuild;
    struct GuildBot;

    struct CraftOrder
    {
        uint32 id = 0;
        uint32 item = 0;
        uint32 amount = 0;         // how many the leader asked for
        uint32 made = 0;           // how many went into the storage for it
        uint32 createdAt = 0;      // unix time
    };

    // An order as the addon and the commands show it.
    struct CraftOrderView
    {
        CraftOrder order;
        std::string itemName;
        std::string note;          // who makes it, or what it waits for
    };

    // An item characters of the guild that are in the world can make.
    struct Craftable
    {
        uint32 item = 0;
        std::string name;
        uint32 itemLevel = 0;
        uint32 itemClass = 0;
        uint32 itemSubClass = 0;
        std::string crafters;      // "Priestholy, Magefrost"
    };

    // What CraftOrders::DisenchantStored did.
    struct DisenchantResult
    {
        uint32 done = 0;                       // how many items were disenchanted
        ObjectGuid enchanter;                  // who did it (empty: nobody could)
        std::string enchanterName;
        std::map<uint32, uint32> materials;    // what went into the guild storage for them: item -> count
        std::string text;                      // a line for the chat: what came of it, or why nothing did
    };
}

class CraftOrders
{
public:
    static CraftOrders& Instance();

    // Reads the orders (startup).
    void Load();

    // "Make count x item" for the guild. False with a code (CRAFT_ORDER_INVALID, CRAFT_ORDER_LIMIT) and a text.
    bool Add(GuildAdventure::AdventureGuild const& guild, uint32 item, uint32 count, std::string& code, std::string& text);
    bool Remove(GuildAdventure::AdventureGuild const& guild, uint32 id, std::string& text);
    // The guild has an order of the item that is not made yet.
    [[nodiscard]] bool HasOpen(uint32 owner, uint32 item) const;

    // The guild's orders with who makes them or what they wait for.
    [[nodiscard]] std::vector<GuildAdventure::CraftOrderView> Describe(GuildAdventure::AdventureGuild const& guild) const;
    // What the guild's characters that are in the world can make (the recipes they know that create an item).
    [[nodiscard]] std::vector<GuildAdventure::Craftable> Craftables(GuildAdventure::AdventureGuild const& guild) const;

    // A town trip: the bot starts on an open order it knows a recipe for (the tool first when it lacks it). True when
    // a craft was started; `text` says what.
    bool Work(GuildAdventure::AdventureGuild& guild, Player* player, std::string& text);
    // Every planner step: what the bot made (for an order, or for anything else) leaves its bags for the guild
    // storage once its crafting ends.
    void Step(GuildAdventure::AdventureGuild& guild, Player* player);
    // A character made `count` x `item` with a spell (PlayerScript::OnPlayerCreateItem; any thread).
    void OnCreated(Player* player, uint32 item, uint32 count);

    // Disenchants up to `count` of a kind of item that lies in the guild storage: `enchanter` (a guild character in
    // the world with the enchanting skill the item asks for; nullptr = the guild's best, BestEnchanter) does it, the
    // materials go into the guild storage, and every item leaves a line in the activity log that names `purpose`
    // ("for a craft order", "on the leader's word"). The result says how many, by whom and what it gave, or (text)
    // why nothing was done. No "would somebody wear it" check and no line in the storage's loot log: both are the
    // caller's.
    GuildAdventure::DisenchantResult DisenchantStored(GuildAdventure::AdventureGuild const& guild, uint32 item, int32 randomProperty,
        uint32 count, char const* purpose, Player* enchanter = nullptr);
    // Whether the item can be disenchanted at all (weapons and armour with a disenchant table), and the Enchanting
    // skill that takes (0 for an item that cannot be).
    static bool CanDisenchant(uint32 item);
    static uint32 DisenchantSkill(uint32 item);
    // The guild's character in the world with the highest Enchanting skill (nullptr: nobody of them has the
    // profession); `skill` gets its skill.
    static Player* BestEnchanter(GuildAdventure::AdventureGuild const& guild, uint32* skill = nullptr);
    // Whether disenchanting the item can give `material` (the world's disenchant tables, read at startup), and
    // whether disenchanting gives the material at all.
    [[nodiscard]] bool DisenchantGives(uint32 item, uint32 material) const;
    // ... and how likely, in percent (0: never).
    [[nodiscard]] float DisenchantChance(uint32 item, uint32 material) const;
    [[nodiscard]] bool IsDisenchantProduct(uint32 material) const { return _disenchantProducts.count(material) != 0; }
    // Bot setting Professions.DisenchantForOrders: `player`, an enchanter, turns green and blue weapons and armour of
    // the guild storage nobody of the guild would wear into their materials, when disenchanting them can give one
    // of `wanted`; at most three per call. `purpose` goes into each log line. Returns how many.
    uint32 DisenchantForMaterials(GuildAdventure::AdventureGuild const& guild, Player* player, std::vector<uint32> const& wanted,
        char const* purpose);
    // Surplus crafted gear (the owner, 2026-11-13: [quote left out]). How many copies of a kind of item in the guild storage
    // an enchanter may disenchant as surplus, and why: a weapon or armour piece a profession recipe makes
    // - that nobody of `members` (the guild's characters in the world) can put on: every copy, rank 2;
    // - that lies there more often than members would wear it: the copies beyond that, one always stays, rank 1.
    // Rank 0 and no copy otherwise (not crafted, not to be disenchanted, the only copy of something wearable).
    struct Surplus
    {
        uint32 rank = 0;
        uint32 copies = 0;
    };
    static Surplus SurplusOf(std::vector<Player*> const& members, uint32 item, int32 randomProperty, uint32 amount);
    // Bot setting Professions.DisenchantSurplus: `player`, an enchanter, disenchants surplus green and blue pieces of
    // the guild storage its skill is enough for, rank 2 first, at most three per call (each in the activity log).
    uint32 DisenchantSurplus(GuildAdventure::AdventureGuild const& guild, Player* player);
    // At a vendor: the materials open orders lack that this vendor sells are bought (the guild's budget). The text for
    // the activity log, empty when nothing was bought.
    std::string BuyAt(GuildAdventure::AdventureGuild const& guild, Player* player, Creature* vendor);

    // A caster (mage, priest, warlock) with nothing in its ranged slot.
    static bool LacksWand(Player const* player);
    // "<crafter> can make <wand>" for the best wand a guild character in the world can make that `caster` could
    // use at its level; empty when nobody can.
    [[nodiscard]] std::string WandMaker(GuildAdventure::AdventureGuild const& guild, Player const* caster) const;

    // Lines for ".ga craftorders".
    [[nodiscard]] std::vector<std::string> Lines(GuildAdventure::AdventureGuild const& guild) const;

private:
    CraftOrders() = default;

    struct Job
    {
        uint32 orderId = 0;        // 0: a tool the bot makes for itself
        uint32 item = 0;
        uint32 before = 0;         // how many of the item the bot had when it began
    };

    struct Created
    {
        ObjectGuid crafter;
        uint32 item = 0;
        uint32 count = 0;
        time_t at = 0;
    };

    // The job of a crafter whose crafting has ended: an order's items into the storage. Returns the job's item.
    uint32 FinishJob(GuildAdventure::AdventureGuild& guild, Player* player);
    // What the character made beside its job leaves its bags for the guild storage.
    void StoreCrafted(GuildAdventure::AdventureGuild& guild, Player* player, uint32 jobItem);
    // Once a minute (`now`: at once): weapons, armour and jewellery the character made itself and carries in its bags
    // without wearing them go into the guild storage, after it has put on what it wants of them. Of a tool it made,
    // one stays.
    void StoreUnworn(GuildAdventure::AdventureGuild& guild, Player* player, bool now);
    void Save(uint32 owner, GuildAdventure::CraftOrder const& order) const;
    void Erase(uint32 id) const;
    [[nodiscard]] bool InWork(uint32 orderId) const;
    // The characters of the guild that are in the world (the leader and the roster bots).
    static std::vector<Player*> Members(GuildAdventure::AdventureGuild const& guild);
    // The recipe for `item` this character knows, nullptr when none.
    static SpellInfo const* KnownRecipe(Player const* player, uint32 item);
    // What the recipe lacks for one cast ("1 x Simple Wood, 1 x Lesser Magic Essence"), empty when nothing.
    static std::string MissingMaterials(Player* player, SpellInfo const* spell);
    // The tool (a totem item or a totem category) the recipe asks for and the character lacks: its name, empty when
    // it has what it needs. `toolRecipe` gets a recipe the character knows that makes such a tool, if any.
    static std::string MissingTool(Player* player, SpellInfo const* spell, SpellInfo const** toolRecipe);
    // Bot setting Professions.DisenchantForOrders: an enchanter turns green and blue weapons and armour of the guild
    // storage nobody of the guild would wear into their materials when an order's recipe lacks dust or essences.
    bool DisenchantForOrder(GuildAdventure::AdventureGuild const& guild, Player* player, SpellInfo const* spell);

    std::map<uint32, std::vector<GuildAdventure::CraftOrder>> _orders; // by owner account
    std::unordered_map<ObjectGuid, Job> _jobs;                          // by crafter
    std::map<uint32, std::map<uint32, float>> _disenchantGives;        // disenchant table -> its materials and their chances
    std::set<uint32> _disenchantProducts;                               // every material a disenchant table holds
    std::mutex _createdLock;
    std::vector<Created> _created;                                      // what characters made, until their next step
    std::unordered_map<ObjectGuid, time_t> _unwornLook;                 // by character: its next look at self-made gear in its bags
    uint32 _nextId = 1;
};

#define sCraftOrders CraftOrders::Instance()

void AddGuildAdventureCraftOrderScripts();

#endif
