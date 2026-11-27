/*
 * Farming: where an item can be farmed (the order FARM, docs/orders.md "Farm orders").
 *
 * The owner, 2026-11-13: the guild leader picks an item and a bot farms it on and on (cloth, a rare bind-on-equip piece).
 * For an item with many sources the bot looks for groups of creatures in an area it can cover on foot, not for single
 * creatures in the middle of nowhere; only creatures at least four levels below the bot; and when there is nothing it
 * can farm the item from, the leader is told.
 *
 * This file holds the knowledge, OrderFarm.cpp (part of OrderService) the behaviour:
 *
 *   - the creature and reference loot tables of the world database, read once at startup (about 120,000 rows);
 *   - Yields(item): what each kind of creature drops of the item per kill, on average, through its own loot table and
 *     the reference tables it rolls on (world drops: a green of a level band sits in a reference table hundreds of
 *     kinds of creature roll on with a small chance). Quest-only rows do not count;
 *   - FindGrounds(player, item): the hunting grounds for one bot. A target is a spawn of a creature the planner
 *     grinds (WorldIndex::GetMobSpawns: no elites, no NPCs, with loot), that the bot's side may attack, that drops the
 *     item and whose level lies at least Farm.LevelsBelow under the bot's. A ground is a spawn with every other
 *     target within Farm.GroundRadius yards of it; it needs Farm.MinSpawns of them, unless no place has that many
 *     (an item only a few creatures drop). Grounds are ranked by what a round over them yields and how far they
 *     are; the risk model (Risk::JudgeSpot) has the last word, as for every place the planner sends a bot to;
 *   - when there is no ground: why (FarmSearch::reason), for the notice the leader gets.
 *
 *   - with no target on the bot's land mass it names the best ground of another one the bot's side can travel to
 *     (ships, zeppelins, portals): the bot changes continent when it must (the owner, 2026-11-13).
 *
 * World thread only. Other sources than creature loot (skinning, mining and herb nodes, fishing, chests, vendors,
 * recipes) are not looked at (the design log section 7 "Open" FM1).
 */

#ifndef MOD_GUILD_ADVENTURE_FARMING_H
#define MOD_GUILD_ADVENTURE_FARMING_H

#include "Define.h"
#include "SharedDefines.h"
#include "WorldIndex.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;
struct ItemTemplate;

namespace GuildAdventure
{
    struct BotRuntime;

    // Why a bot has no hunting ground for an item.
    enum class FarmProblem : uint8
    {
        None,          // it has one
        UnknownItem,
        NoCreatures,   // no creature drops it (it is gathered, fished, bought, crafted, a quest's)
        NotGrindable,  // only elites, dungeon creatures or creatures the bot's side may not attack drop it
        TooHigh,       // every creature that drops it is above the bot's level minus the margin
        Elsewhere,     // the creatures it can farm it from live on another land mass: it travels there
                       // (FarmSearch::voyage), or cannot (no ship, zeppelin or portal for its side)
        Waiting,       // every ground in reach is emptied: the creatures are not back yet
        Refused        // every ground in reach is set aside or the risk model keeps the bot from it
    };

    // A hunting ground: its middle (a real spawn, so the planner can set it aside), what stands there.
    struct FarmGround
    {
        SpawnPoint spot;
        uint32 spawns = 0;         // targets within the radius
        uint32 kinds = 0;          // kinds of creature among them
        float yield = 0.0f;        // items a round over all of them brings, on average
        float distance = 0.0f;     // from the bot, yards
        uint8 minLevel = 0;        // of the creatures there
        uint8 maxLevel = 0;
        std::string name;          // the kind most of them are
        bool usable = false;       // the risk model lets the bot work there now
        std::string verdict;       // not usable: why
    };

    struct FarmSearch
    {
        FarmProblem problem = FarmProblem::UnknownItem;
        std::vector<FarmGround> grounds;   // best first; the first one passed the risk model
        uint32 kinds = 0;                  // kinds of creature that drop the item at all
        uint32 grindKinds = 0;             // ... that a bot of this side can grind in the open world
        uint32 validKinds = 0;             // ... at or below the level limit
        uint32 validSpawns = 0;            // their spawns on the bot's map ...
        uint32 localSpawns = 0;            // ... and of those the ones on its land mass
        uint8 levelLimit = 0;              // the bot's level minus the margin
        uint8 lowestLevel = 0;             // TooHigh: the lowest level of a creature it could grind for it ...
        std::string lowestName;            // ... and its name
        std::string elsewhere;             // Elsewhere: where ("Kalimdor")
        bool voyage = false;               // Elsewhere: the bot can travel there; the best ground over there:
        SpawnPoint voyageSpot;
        Region voyageRegion = Region::None;
        std::string voyageName;            // the kind of creature at it
        uint32 voyageSpawns = 0;
        uint32 voyageLegs = 0;             // ships, zeppelins and portals on the way
        uint32 waitMs = 0;                 // Waiting: until the first emptied ground counts again
        std::string reason;                // one sentence for the leader ("" when there is a ground)
    };

    // A row of the item picker (GADV FARM_ITEMS).
    struct FarmItem
    {
        uint32 item = 0;
        std::string name;
        uint8 quality = 0;
        uint16 itemLevel = 0;
        uint8 itemClass = 0;
        uint8 lowestLevel = 0;     // the lowest creature a bot of this side can grind for it
        uint32 kinds = 0;          // how many kinds of creature of that side's reach drop it
    };
}

class Farming
{
public:
    static Farming& Instance();

    // Startup and ".ga reload" (world thread, after WorldIndex::Build): the loot tables and the creatures bots grind.
    void Load();

    // What one kill of each kind of creature brings of the item on average (creature entry -> items per kill), through
    // its loot table and the reference tables it rolls on. Computed once per item.
    std::unordered_map<uint32, float> const& Yields(uint32 item);
    // The same for one kind of creature (0 when it does not drop the item).
    float YieldOf(uint32 creatureEntry, uint32 item);

    // Whether the order FARM can be given for the item at all: it exists and a creature bots can grind in the open
    // world drops it for this side. `error` says why not (a sentence for the leader).
    bool CheckItem(uint32 item, TeamId team, std::string& error);

    // The hunting grounds of one bot for an item, by the bot's settings (Farm.*), its place and the planner's state
    // (places set aside, grounds emptied), best first, each with the risk model's verdict; at most `limit`. With a
    // usable one among them `problem` is None and it is grounds[0] (with limit 1 the search stops at it).
    GuildAdventure::FarmSearch FindGrounds(Player const* player, GuildAdventure::BotRuntime const& rt, uint32 item,
        std::size_t limit = 5);

    // The item picker: items whose name holds `text` (or the item of that id) and that a bot of that side can farm,
    // the best matches first, at most `limit`.
    std::vector<GuildAdventure::FarmItem> Search(std::string const& text, TeamId team, std::size_t limit);

    // For ".ga admin farm": the sources of an item and the grounds of a character, spelled out.
    std::vector<std::string> Describe(Player const* player, GuildAdventure::BotRuntime const& rt, uint32 item);

    // True for a guild character the AI plays whose order is to farm this item (the loot filter sends it into the
    // guild storage).
    static bool IsOrderItem(ObjectGuid character, uint32 item);

private:
    Farming() = default;

    struct LootRow
    {
        uint32 item = 0;
        uint32 reference = 0;      // > 0: a roll on that reference table (item is unused)
        float chance = 0.0f;       // percent; 0 inside a group: an equal share of what the group has left
        uint8 group = 0;
        uint8 minCount = 1;
        uint8 maxCount = 1;
        bool quest = false;        // drops only for a character on the quest
    };
    using LootTable = std::vector<LootRow>;

    // A creature the planner grinds (WorldIndex::GetMobSpawns), by entry.
    struct GrindKind
    {
        uint8 minLevel = 0;
        uint8 maxLevel = 0;
        uint32 factionTemplate = 0;
        std::string name;
    };

    // Items of that kind one processing of a loot table gives on average, as the core rolls it (LootTemplate::Process):
    // rows without a group each by their own chance, each group one of its rows, a reference row the table it names.
    float TableYield(std::unordered_map<uint32, LootTable> const& store, uint32 id, uint32 item, uint8 depth) const;
    // The chance of a row inside its group.
    static float GroupShare(LootTable const& table, LootRow const& row);

    std::unordered_map<uint32, LootTable> _creatureLoot;                  // by loot id
    std::unordered_map<uint32, LootTable> _referenceLoot;                 // by reference id
    std::unordered_map<uint32, std::vector<uint32>> _lootsOfItem;         // item -> creature loot ids with a direct row
    std::unordered_map<uint32, std::vector<uint32>> _refsOfItem;          // item -> reference tables with a direct row
    std::unordered_map<uint32, std::vector<uint32>> _lootsOfRef;          // reference -> creature loot ids rolling on it
    std::unordered_map<uint32, std::vector<uint32>> _refsOfRef;           // reference -> reference tables rolling on it
    std::unordered_map<uint32, std::vector<uint32>> _creaturesOfLoot;     // loot id -> creature entries
    std::unordered_map<uint32, GrindKind> _grindKinds;                    // creature entry -> what the planner grinds
    std::unordered_map<uint32, std::unordered_map<uint32, float>> _yields; // item -> (creature entry -> items per kill)
    std::unordered_set<uint32> _droppedItems;                             // items some loot or reference table holds
};

#define sFarming Farming::Instance()

#endif
