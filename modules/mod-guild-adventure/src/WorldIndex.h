/*
 * WorldIndex: read-only lookups over the world database that the order planner needs (the design log section 2.4,
 * docs/orders.md "World index").
 *
 * Built once at startup (and on ".ga reload") from the core's in-memory ObjectMgr data plus two loot queries:
 *   - spawn points of every creature and game object on the open-world maps, by entry (normal phase only, without
 *     spawns that exist only during a game event);
 *   - quest starters and enders by quest (creature or game object entries);
 *   - the quests of each zone (quest_template.QuestSortID, areas folded into their zone);
 *   - the creatures and game objects that drop each quest-required item;
 *   - service NPCs (vendors, repairers, innkeepers, flight masters, class trainers) with their faction;
 *   - one hub position per zone and team: the densest cluster of quest givers of that zone's quests that are not
 *     hostile to the team. Travel orders walk to it.
 *
 * The zone of a single spawn point is not in the spawn data, so it is looked up in the map's terrain on demand and
 * cached (GetSpawnZone). Everything runs on the world thread, outside map updates (invariant I5 of
 * docs/take-control.md).
 *
 * Regions: the walkable land masses of the game world. Between regions bots take ships and zeppelins
 * (ga_transport_route) and portals: a travel link is one leg of a transport's path from a dock in one region to the next
 * dock in another, or an area trigger that teleports from one open-world map to another (docs/orders.md "Travel").
 */

#ifndef MOD_GUILD_ADVENTURE_WORLD_INDEX_H
#define MOD_GUILD_ADVENTURE_WORLD_INDEX_H

#include "ObjectGuid.h"
#include "SharedDefines.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;
struct ItemTemplate;

namespace GuildAdventure
{
    enum class Region : uint8
    {
        None,
        EasternKingdoms,  // map 0 without islands
        Kalimdor,         // map 1 without Teldrassil
        Teldrassil,       // Teldrassil and Darnassus (map 1, an island; the top of the world tree)
        RuttheranVillage, // the foot of the world tree with the docks; Darnassus' portal leads down there
        SardorIsle,       // Feathermoon Stronghold's island off the Feralas coast (map 1)
        AzuremystIsles,   // Azuremyst, Bloodmyst, the Exodar (map 530)
        EversongIsles,    // Eversong Woods, Ghostlands, Silvermoon, Isle of Quel'Danas (map 530)
        Outland,          // the rest of map 530
        Northrend         // map 571
    };

    char const* ToString(Region region);
    // Region of a position. The land masses are told apart by their coordinates (a ship in the Veiled Sea or a bot
    // on a beach belongs to the island next to it, whatever zone the water is), Rut'theran Village from the top of
    // the world tree by height.
    Region GetRegionAt(uint32 mapId, float x, float y, float z);
    Region GetRegion(Player const* player);
    // Region of a zone template (the region of its hub; zones without one by their zone id).
    // The region of the team's own hub of the zone (a zone's hubs can lie in two regions: Feralas has Feathermoon on
    // Sardor Isle for the Alliance and Camp Mojache on the mainland for the Horde).
    Region GetZoneRegion(uint32 mapId, uint32 zoneId, TeamId team);
    // Region of a zone by its id alone (zone templates without a hub).
    Region GetRegion(uint32 mapId, uint32 zoneId);

    struct SpawnPoint
    {
        ObjectGuid::LowType spawnId = 0;
        uint32 entry = 0;
        uint16 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        bool gameObject = false;
    };

    enum class ServiceType : uint8
    {
        Vendor,        // any vendor (sells junk, buys food and water)
        Repair,        // vendor that repairs
        Innkeeper,
        FlightMaster,
        ClassTrainer,
        FoodVendor,    // sells food and drink
        SpiritHealer   // only ghosts see and use it (keep it last: ServiceTypeCount)
    };
    constexpr std::size_t ServiceTypeCount = std::size_t(ServiceType::SpiritHealer) + 1;

    // A teleporter inside a map: an area trigger and where it puts whoever steps into it.
    struct LocalPortal
    {
        float x = 0.0f, y = 0.0f, z = 0.0f, radius = 0.0f;
        float targetX = 0.0f, targetY = 0.0f, targetZ = 0.0f, targetO = 0.0f;
    };

    struct ServicePoint
    {
        SpawnPoint spawn;
        uint32 npcFlags = 0;
        uint32 factionTemplate = 0;
        uint8 trainerClass = 0;    // class trainers: the class they teach
        std::string name;
    };

    // A spawn of an ordinary hostile or neutral creature a bot can grind on (no NPC flags, not elite, not a critter
    // or trigger, drops loot).
    struct MobSpawn
    {
        SpawnPoint spawn;
        uint8 minLevel = 0;
        uint8 maxLevel = 0;
        uint32 factionTemplate = 0;
        bool elite = false; // an elite, rare elite or boss (only among the strong spawns)
        bool rare = false;  // a rare or rare elite: seldom alive, and at one of several places
        bool flees = false; // it runs for help when it is nearly dead (WorldIndex::FleesForHelp)
        bool patrol = false; // a point of the creature's patrol path, not where it spawns (WorldIndex::GetPatrolPoints)
        float round = 0.0f;  // ... and how long that path is, in yards
    };

    // A dock: where a ship or zeppelin stops (a stop frame of its path).
    struct TransportStop
    {
        uint16 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // One leg of a ship or zeppelin route between two regions, or a portal (an area trigger that teleports between
    // open-world maps, e.g. the Dark Portal).
    struct TravelLink
    {
        uint32 transportEntry = 0;     // ship / zeppelin (0 for a portal)
        uint32 areaTrigger = 0;        // portal (0 for a ship)
        uint32 gameObject = 0;         // a teleporter players click (an Orb of Translocation): its entry; walked to like a portal
        float triggerRadius = 0.0f;
        char faction = 'C';            // 'A', 'H', 'C'
        std::string name;              // the ship's name
        TransportStop from;
        TransportStop to;
        Region fromRegion = Region::None;
        Region toRegion = Region::None;
        std::string fromName;          // dock area names for "why" lines
        std::string toName;
    };

    struct ZoneHub
    {
        uint16 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        std::string name;          // the NPC the hub was found at, for "why" lines
    };
}

class WorldIndex
{
public:
    static WorldIndex& Instance();

    // World thread. Rebuilds everything from ObjectMgr and the world database; call after
    // GuildAdventureMgr::LoadTemplates (hubs are computed for the template zones).
    void Build();

    [[nodiscard]] std::vector<GuildAdventure::SpawnPoint> const& GetCreatureSpawns(uint32 entry) const;
    [[nodiscard]] std::vector<GuildAdventure::SpawnPoint> const& GetGameObjectSpawns(uint32 entry) const;

    // Quests whose quest-log zone (QuestSortID) is the zone.
    [[nodiscard]] std::vector<uint32> const& GetZoneQuests(uint32 zoneId) const;
    // The zone a quest belongs to (QuestSortID folded to its zone), 0 for class, profession and holiday quests.
    [[nodiscard]] uint32 GetQuestZone(uint32 questId) const;

    // Spawn points of the creatures and game objects that start / end the quest.
    [[nodiscard]] std::vector<GuildAdventure::SpawnPoint> GetQuestStarters(uint32 questId) const;
    [[nodiscard]] std::vector<GuildAdventure::SpawnPoint> GetQuestEnders(uint32 questId) const;
    // Spawn points that can drop an item quests ask for: creature and game object loot, directly or through one
    // reference table; rows flagged for the quest (QuestRequired) at any chance, ordinary loot from 10 % on.
    [[nodiscard]] std::vector<GuildAdventure::SpawnPoint> GetItemSources(uint32 itemId) const;
    // The chance, in percent, that a source hands the item over (its loot row's; 0: not known). GetItemSources leaves
    // out a source that has the item less than a fifth as often as the best one.
    [[nodiscard]] float GetItemSourceChance(uint32 itemId, uint32 entry, bool gameObject) const;
    // The creature entries GetItemSources keeps for an item (no spawns: cheap).
    [[nodiscard]] std::vector<uint32> GetItemSourceCreatures(uint32 itemId) const;
    // Whether creatures drop the item often enough to hunt for it (quest items and the materials of profession
    // recipes are indexed). Cheap, unlike GetItemSources.
    [[nodiscard]] bool HasCreatureSources(uint32 itemId) const;
    // Spawn points of the vendors that sell an item quests ask for.
    [[nodiscard]] std::vector<GuildAdventure::SpawnPoint> GetItemVendors(uint32 itemId) const;
    // True for an item whose loot (item_loot_template) holds an item quests ask for, or that a quest hands out (its
    // start item or a source item): a container the bot opens. The sources of a container of the first kind count as
    // sources of what is inside (GetItemSources).
    [[nodiscard]] bool IsQuestContainer(uint32 itemId) const { return _questContainers.count(itemId) != 0; }
    // True for a creature whose script makes it run for help when it is nearly dead (SmartAI "flee for assist":
    // troggs, kobolds, murlocs, gnolls ...): it comes back with the nearest of its kind.
    [[nodiscard]] bool FleesForHelp(uint32 creatureEntry) const { return _fleeingEntries.count(creatureEntry) != 0; }
    // .. early in a fight, not only nearly dead: on a timer of the fight, or at half its health or more (the Mossflayer
    // Scouts of the Eastern Plaguelands run 4 s into every fight). Such a creature surely fetches its helper.
    [[nodiscard]] bool FleesForHelpEarly(uint32 creatureEntry) const { return _earlyFleeingEntries.count(creatureEntry) != 0; }
    // Whether a creature's spawn exists only while a game event runs (a holiday, the Scourge invasion): the 3125 Wild
    // Turkeys of Pilgrim's Bounty, for one, stand beside every starting area while it lasts.
    [[nodiscard]] bool IsEventSpawn(ObjectGuid::LowType spawnId) const { return spawnId && _eventCreatureSpawns.count(spawnId) != 0; }
    // The yards within which a creature's script calls every creature of its side for help in a fight (in combat, at a
    // health, on aggro: SMART_ACTION_CALL_FOR_HELP), 0 for none: a Deadwood Den Watcher calls them within 20 yd at half
    // its health, wherever the fight stands.
    [[nodiscard]] float CallForHelpYards(uint32 creatureEntry) const
    {
        auto itr = _callsForHelp.find(creatureEntry);
        return itr == _callsForHelp.end() ? 0.0f : itr->second;
    }
    // True for a creature whose script stuns its foe every few seconds of a fight (a spell with a stun aura on a timer of
    // at most 12 s): the Frayfeather Stagwings' Swoop, every 3-5 s. A bot that has not fought one judges it dearer.
    [[nodiscard]] bool StunsOften(uint32 creatureEntry) const { return _stunningEntries.count(creatureEntry) != 0; }
    // A creature whose SmartAI script casts a spell that knocks its foe back (a Stitched Horror's Mighty Blow).
    [[nodiscard]] bool KnocksBack(uint32 creatureEntry) const { return _knockbackEntries.count(creatureEntry) != 0; }
    // A creature a run does not get away from: its script casts a damage spell of 20 yd's reach or more every few
    // seconds in a fight (it hits a runner as long as the runner is in its reach), or a spell that roots (a net).
    [[nodiscard]] bool HoldsARunner(uint32 creatureEntry) const { return _runHolders.count(creatureEntry) != 0; }
    // The creature a creature's SmartAI script summons (a warlock's Succubus or Imp, out of combat; adds it calls in a
    // fight), 0 for none: such minions have no spawn of their own, and a camp of warlocks is twice as many creatures
    // as its spawns (a druid died opening the Bloodsail Charts among the Bloodsail Warlocks' succubi, 2026-11-14).
    [[nodiscard]] uint32 MinionOf(uint32 creatureEntry) const;
    // The hostile creature a creature's SmartAI script summons when it dies, and how many: a Tortured Druid or Sentinel
    // of Silithus brings a Hive'Ashi Drone (level 57-58) out of its corpse. {0, 0} for none.
    [[nodiscard]] std::pair<uint32, uint8> DeathSummonOf(uint32 creatureEntry) const;
    // The hostile creature a creature's SmartAI script summons in a fight (in combat, at a health, on aggro) and how
    // many stand by it at a time, whatever its type: a Centipaar Swarmer of Tanaris casts Silithid Swarm every one to
    // three seconds of a fight (creatures of no type, which MinionOf leaves out). {0, 0} for none.
    [[nodiscard]] std::pair<uint32, uint8> CombatSummonOf(uint32 creatureEntry) const;
    // The quests that reward an item other quests ask for (RewardItem1-4).
    [[nodiscard]] std::vector<uint32> const& GetQuestsRewarding(uint32 itemId) const;

    [[nodiscard]] std::vector<GuildAdventure::ServicePoint> const& GetServices(GuildAdventure::ServiceType type) const;

    [[nodiscard]] GuildAdventure::ZoneHub const* GetHub(uint32 zoneId, TeamId team) const;

    // Grind candidates on an open-world map.
    [[nodiscard]] std::vector<GuildAdventure::MobSpawn> const& GetMobSpawns(uint32 mapId) const;
    // Elites, rares, bosses and creatures without loot (the other side's guards): no targets for a bot alone (they
    // are not among the mob spawns), but they stand in camps and beside quest targets, and the risk model counts them.
    [[nodiscard]] std::vector<GuildAdventure::MobSpawn> const& GetStrongSpawns(uint32 mapId) const;
    // Whether a trade good is something creatures are hunted for (cloth, meat, elemental things): not what a gathering
    // profession brings, disenchanting gives, or a gem.
    static bool IsHuntMaterial(ItemTemplate const* proto);
    // Spawns of creatures hostile to the character within `radius` yards of a point, whatever their level (the risk
    // verdict leaves out creatures far below a character: they still come when it stands there for minutes). For a
    // place to stand still at: a fire to cook at, an anvil, water to fish in.
    [[nodiscard]] uint32 CountHostileSpawns(Player const* player, uint32 mapId, float x, float y, float z, float radius) const;
    // Where the creatures of both lists walk when they patrol: a point every few yards of each one's path
    // (MobSpawn::patrol; spawn.spawnId says whose). A spot can be far from every spawn and on a patrol's way.
    [[nodiscard]] std::vector<GuildAdventure::MobSpawn> const& GetPatrolPoints(uint32 mapId) const;

    // The first leg of the shortest ship / zeppelin route from one region to another the team may take, from the dock
    // nearest to (mapId, x, y) among the equally short ones; nullptr if there is none. Docks whose region is not known
    // yet (their map was not loaded) are resolved on the way.
    // The destination (destMap, destX, destY) counts too: the arrival dock nearer to it wins.
    [[nodiscard]] GuildAdventure::TravelLink const* NextTravelLink(GuildAdventure::Region from, GuildAdventure::Region to,
        TeamId team, uint32 mapId, float x, float y, uint32 destMap, float destX, float destY) const;
    // Number of legs of that route (0 = same region, -1 = unreachable).
    [[nodiscard]] int32 CountTravelLegs(GuildAdventure::Region from, GuildAdventure::Region to, TeamId team) const;
    // A region no zone within the server's level cap lies in (Outland and Northrend at a cap of 60): a route may end
    // there, never pass through it.
    [[nodiscard]] bool IsRegionBeyondCap(GuildAdventure::Region region) const;
    [[nodiscard]] std::vector<GuildAdventure::TravelLink> const& GetTravelLinks() const { return _links; }

    // A teleporter next to a position: an area trigger within `range` that teleports to another spot of the same map
    // (the way in and out of the sanctum of Stormwind's wizard tower). False when there is none.
    [[nodiscard]] bool FindLocalPortal(uint32 mapId, float x, float y, float z, float range, float& targetX, float& targetY,
        float& targetZ, float& targetO) const;
    // The teleporter that leads to a place: an area trigger of the map that puts its user within `range` of the place
    // and at about its height (`height`), from somewhere else (not one of the long ones travels use). False when there
    // is none.
    [[nodiscard]] bool FindLocalPortalInto(uint32 mapId, float x, float y, float z, float range, float height,
        GuildAdventure::LocalPortal& portal) const;
    // The teleporter nearest to a position (its area trigger within `range`), of the same kind.
    [[nodiscard]] bool FindLocalPortalFrom(uint32 mapId, float x, float y, float z, float range,
        GuildAdventure::LocalPortal& portal) const;

    // Zone and area of a spawn point from the map's terrain (cached). 0 when its map is not loaded.
    [[nodiscard]] uint32 GetSpawnZone(GuildAdventure::SpawnPoint const& spawn) const;
    [[nodiscard]] uint32 GetSpawnArea(GuildAdventure::SpawnPoint const& spawn) const;
    // Region of a spawn point.
    [[nodiscard]] GuildAdventure::Region GetSpawnRegion(GuildAdventure::SpawnPoint const& spawn) const;

    // True if the creature or game object faction is not hostile to the player's faction.
    [[nodiscard]] static bool IsFriendlyFaction(uint32 factionTemplateId, Player const* player);
    [[nodiscard]] static bool IsFriendlyFaction(uint32 factionTemplateId, TeamId team);
    // True if the player may attack creatures of that faction (hostile or neutral, not friendly).
    [[nodiscard]] static bool IsAttackableFaction(uint32 factionTemplateId, Player const* player);

    [[nodiscard]] std::size_t GetSpawnCount() const { return _spawnCount; }

private:
    WorldIndex() = default;

    void IndexSpawns();
    void IndexQuests();
    void IndexItemSources();
    void IndexServices();
    void IndexMobs();
    void IndexTransports();
    void ComputeHubs();
    // Region of every dock whose map is loaded by now (lazy: transports' maps exist once the server runs).
    void ResolveLinkRegions() const;

    std::vector<GuildAdventure::SpawnPoint> SpawnsOf(std::vector<std::pair<uint32, bool>> const& entries) const;

    std::unordered_map<uint32, std::vector<GuildAdventure::SpawnPoint>> _creatureSpawns;   // by creature entry
    std::unordered_map<uint32, std::vector<GuildAdventure::SpawnPoint>> _goSpawns;         // by game object entry
    std::unordered_map<uint32, std::vector<std::pair<uint32, bool>>> _questStarters;       // quest -> (entry, is GO)
    std::unordered_map<uint32, std::vector<std::pair<uint32, bool>>> _questEnders;
    std::unordered_map<uint32, std::vector<uint32>> _zoneQuests;
    std::unordered_map<uint32, uint32> _questZone;
    std::unordered_map<uint32, std::vector<std::pair<uint32, bool>>> _itemSources;         // item -> (entry, is GO)
    std::unordered_map<uint64, float> _itemSourceChance;                                   // (item, entry, is GO) -> percent
    std::unordered_map<uint32, std::vector<std::pair<uint32, bool>>> _itemVendors;         // item -> (vendor entry, false)
    std::unordered_set<uint32> _questContainers;                                           // items to open
    std::unordered_set<uint32> _fleeingEntries;                                            // creatures that run for help
    std::unordered_set<uint32> _earlyFleeingEntries;
    std::unordered_set<ObjectGuid::LowType> _eventCreatureSpawns;
    std::unordered_set<uint32> _knockbackEntries;                                         // creatures that knock back
    std::unordered_map<uint32, float> _callsForHelp;                                       // creature -> yards it calls within
    std::unordered_set<uint32> _stunningEntries;                                          // creatures that stun every few s
    std::unordered_set<uint32> _runHolders;                                               // casters and netters (HoldsARunner)
    std::unordered_map<uint32, uint32> _minions;                                            // creature -> the creature it summons
    std::unordered_map<uint32, std::pair<uint32, uint8>> _deathSummons;                  // creature -> what its death brings, how many
    std::unordered_map<uint32, std::pair<uint32, uint8>> _combatSummons;                 // creature -> what it summons in a fight
    std::unordered_map<uint32, std::vector<uint32>> _itemRewardQuests;                     // item -> quests rewarding it
    std::vector<GuildAdventure::ServicePoint> _services[GuildAdventure::ServiceTypeCount];
    std::unordered_map<uint64, GuildAdventure::ZoneHub> _hubs;                             // (zone << 1) | team
    std::unordered_map<uint32, std::vector<GuildAdventure::MobSpawn>> _mobSpawns;          // by map
    std::unordered_map<uint32, std::vector<GuildAdventure::MobSpawn>> _strongSpawns;       // by map
    std::unordered_map<uint32, std::vector<GuildAdventure::MobSpawn>> _patrolPoints;       // by map
    mutable std::vector<GuildAdventure::TravelLink> _links;
    mutable std::unordered_map<uint64, std::pair<uint32, uint32>> _spawnZones;             // (spawnId << 1) | GO -> zone, area
    void LookupSpawn(GuildAdventure::SpawnPoint const& spawn, uint32& zone, uint32& area) const;
    std::size_t _spawnCount = 0;
};

#define sWorldIndex WorldIndex::Instance()

#endif
