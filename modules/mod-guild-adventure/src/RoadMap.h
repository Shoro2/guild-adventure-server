/*
 * RoadMap: walking routes over mod-playerbots' travel node graph (docs/orders.md "Routes").
 *
 * NewRpg's long-distance walk follows the navmesh towards its destination as far as one search gets: it knows neither
 * roads nor what lives on the way, so a level-11 bot sent from Elwynn Forest to Westfall cut through the corner of
 * Duskwood and died to level-20 wolves, and long walks end at city walls and cliffs. mod-playerbots ships a graph for
 * exactly this (its playerbots database, tables playerbots_travelnode, _link and _path): 3781 nodes (towns, inns,
 * flight masters, every named area), 13885 walking links between neighbours, and for each link the walked path as
 * points a few yards apart. The module reads that graph and plans over it:
 *
 *   - Danger: for every link and team, the highest level of creatures hostile to that team within 45 yards of its path
 *     (elites count four levels higher), computed at load from the world's spawns.
 *   - Route: the cheapest chain of links from a node near the start to a node near the destination. A link costs its
 *     length, times a factor that grows steeply with how far its creatures are above the walker, so a route through a
 *     zone far above the bot is taken only when the way around is many times longer.
 *
 * The order planner walks a route by handing NewRpg one point after another, a short way ahead of the bot
 * (OrderService::WalkTo), so each step is a walk the navmesh search covers.
 *
 * Load() runs on the world thread (startup, ".ga reload"); FindRoute is const and may be called from the world thread
 * while no Load() runs.
 */

#ifndef MOD_GUILD_ADVENTURE_ROAD_MAP_H
#define MOD_GUILD_ADVENTURE_ROAD_MAP_H

#include "SharedDefines.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GuildAdventure
{
    struct RoutePoint
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    struct RoadRoute
    {
        bool found = false;
        uint32 mapId = 0;
        std::vector<RoutePoint> points;    // from the first node to the last one, a few yards apart
        float length = 0.0f;               // yards, with the walks to the first and from the last node
        float direct = 0.0f;               // yards as the crow flies
        uint8 danger = 0;                  // highest level of hostile creatures along it (0 = none)
        uint8 dangerBefore = 0;            // the same without the last EndStretch yards before the destination
        // The biggest group of hostile creatures standing right on the way, and the highest level among them. With a
        // crowd check (FindRoute) only groups the walker cannot take count; the group it stands in at the start does
        // not (it has to get out of it).
        uint8 crowd = 0;
        uint8 crowdLevel = 0;
        float crowdDistance = 0.0f;        // how far from the start that group stands (as the crow flies)
        // That group is a camp the way leads through the middle of (Risk::CampAtPoint), which the walker cannot take:
        // `crowd` is how many creatures make it, `crowdLevel` their middle level.
        bool camp = false;
        // No node near the start had a walk the path finder proves: the walk to the first node is a guess (FindRoute).
        bool startGuessed = false;
        uint32 nodes = 0;                  // nodes it passes
        std::string via;                   // their names, for log lines
        // Rides on a lift (an elevator, a static transport the travel node graph rides): the route point the bot boards
        // at (the next point is the stop it leaves at) and the lift's game object entry.
        struct Lift
        {
            uint32 at = 0;
            uint32 entry = 0;
        };
        std::vector<Lift> lifts;
    };
}

class RoadMap
{
public:
    static RoadMap& Instance();

    // Reads the graph from the playerbots database and computes the danger of its links.
    void Load();

    // The route for a walker of that team and level. `found` is false when there is no node near the start or the
    // destination, or no chain of links between them.
    // `legWalkable` (optional) is asked about the walk off the graph: from the start to a first node, from a last node
    // to the destination. A node it refuses is not used as that end of the route.
    // `avoided` (optional) says whether a spot is one the walker keeps away from (where it kept dying): a link or a
    // walk off the graph through such a spot costs many times its length, so the route goes around when it can.
    // `canTake` (optional) says whether the walker can fight a group of `count` creatures of up to `level` at once: a
    // link with a group it cannot take costs many times its length, so the route goes around when it can.
    using LegCheck = std::function<bool(GuildAdventure::RoutePoint const& from, GuildAdventure::RoutePoint const& to)>;
    using AvoidCheck = std::function<bool(float x, float y)>;
    using CrowdCheck = std::function<bool(uint8 count, uint8 level)>;
    [[nodiscard]] GuildAdventure::RoadRoute FindRoute(uint32 mapId, float fromX, float fromY, float fromZ, float toX, float toY,
        float toZ, TeamId team, uint8 level, LegCheck const& legWalkable = {}, AvoidCheck const& avoided = {},
        CrowdCheck const& canTake = {}, float startStretch = CrowdStartStretch) const;
    // The stretch at the start of a walk whose groups do not count against it (the walker stands among them).
    static constexpr float CrowdStartStretch = 120.0f;
    // ... for a walker that stands outside the reach of every group there (FindRoute).
    static constexpr float OutsideStartStretch = 20.0f;
    // ... and for a walker that stands in a camp and has to get out of it (`startStretch`): a troll village is wider
    // than the first stretch, and every link out of its node passes more of the village.
    static constexpr float CampOutStretch = 300.0f;

    // The highest level of creatures hostile to the team within reach of the straight line between two points
    // (0 = none).
    // `stopBefore`: the last yards of the line that are left out (what lives at the destination is judged by whoever
    // goes there to fight it).
    // `swimmersByHeight`: the heights are only used to tell whether the line is in the water (what only swims counts
    // then); distances stay flat. For a walk over ground whose height between its ends is not known: a pier, a bridge.
    [[nodiscard]] uint8 DangerAlong(uint32 mapId, float fromX, float fromY, float toX, float toY, TeamId team,
        float stopBefore = 0.0f, float fromZ = NoHeight, float toZ = NoHeight, bool swimmersByHeight = false) const;
    static constexpr float NoHeight = -100000.0f;
    // A town of the other side: a place where three or more of its people stand together (innkeeper, flight master,
    // traders, trainers, guards: creatures with a service or the guard flag whose faction is hostile to this team and
    // not to the other), and the ground around them. Closed for ways at every level: DangerAt answers TownDanger
    // there, GuardAt too, so routes, walks, flights' landings, rescues and retreats keep out with the rules they have.
    // (The lone flight master of the other side in a neutral town is no town.)
    [[nodiscard]] bool InHostileTown(uint32 mapId, float x, float y, TeamId team) const;
    static constexpr uint8 TownDanger = 250;
    // For log lines: "level-46 creatures", or "a town of the other side" for TownDanger.
    [[nodiscard]] static std::string DangerName(uint8 danger);
    // The biggest group of hostile creatures that stand right on a straight line (within a few yards of one point of
    // it: they all come at once), and the highest level among them. Levels do not matter here: four creatures ten
    // levels below a badly geared walker are a lost fight (a level-21 mage and four level-10 gnolls, live).
    // `skipStart`: the first yards of the line that are left out.
    [[nodiscard]] uint8 CrowdAlong(uint32 mapId, float fromX, float fromY, float toX, float toY, TeamId team, uint8& level,
        float fromZ = NoHeight, float toZ = NoHeight, float skipStart = 0.0f) const;
    // Whether a group of `count` creatures whose strongest is of `crowdLevel` counts for a walker of `walkerLevel` at all
    // (the risk model then says whether it can take it): from Walks.GroupFrom creatures not more than three levels under
    // it, and from four not more than eight under it (four or more creatures five to eight levels below a bot cost it
    // 43-46 % of its health, nine deaths in 105 fights; the seventeen Theramore Sentries of Sentry Point, level 35-36,
    // killed a level-40 druid on its way past them, 2026-11-26, and counted as no group for it).
    [[nodiscard]] static bool GroupCounts(uint8 count, uint8 crowdLevel, int32 walkerLevel);
    // The weakest hostile creature that spawns within `radius` of a place, and how many do (`count`). 0 without any.
    // What a place is like for someone who gets there: a place whose weakest creature is above the visitor is no place
    // to be sent to.
    [[nodiscard]] uint8 WeakestAround(uint32 mapId, float x, float y, float radius, TeamId team, uint32& count) const;
    // The stretch before a destination that RoadRoute::dangerBefore leaves out.
    static constexpr float EndStretch = 60.0f;

    // The node of the graph nearest to a position, within `range` and at least `minDistance` away: a spot that is
    // on the ground and has walking links. False when there is none.
    // With a team and a level: not a node a hostile creature of about that level or above stands on (GuardCounts),
    // nor one in the middle of a camp that holds for a walker of that level (Risk::CampAtPoint).
    [[nodiscard]] bool FindNearestNode(uint32 mapId, float x, float y, float minDistance, float range, GuildAdventure::RoutePoint& spot,
        std::string& name, TeamId team = TEAM_NEUTRAL, uint8 level = 0) const;
    // Many nodes of the graph are named places, and playerbots recorded some of them standing on a creature's spawn:
    // "Arathi Highlands Witherbark Village" on a Witherbark Witch Doctor (34), "Arathi Highlands Boulderfist Hall" on a
    // Boulderfist Magus (37), "Badlands The Makers Terrace" on a Shadowforge Surveyor in the dig before Uldaman. A
    // route over such a node leads the walker onto that creature and what stands with it.
    // The highest level of a hostile creature that stands within its reach of the node at a position (elites count
    // higher; no patrols); 0: none, or no node there.
    [[nodiscard]] uint8 NodeGuard(uint32 mapId, float x, float y, TeamId team) const;
    // Whether the node of a ship or zeppelin lies within `yards` of a point (the point is on its pier, or near it).
    [[nodiscard]] bool IsNearTransportNode(uint32 mapId, float x, float y, float yards) const;
    // Whether that creature counts for a walker: it is not more than GuardLevelsBelow under it.
    static constexpr int32 GuardLevelsBelow = 2;
    [[nodiscard]] static bool GuardCounts(uint8 guard, uint8 level) { return guard && int32(guard) >= int32(level) - GuardLevelsBelow; }
    // Whether every link of the node at a position passes a group the walker cannot take: a node inside a camp (the
    // troll village of Arathi, an ogre hall). No place to go to for safety, and no start of a way out.
    [[nodiscard]] bool IsNodeAmongGroups(uint32 mapId, float x, float y, TeamId team, uint8 level, CrowdCheck const& canTake) const;
    // The group a walker at a point stands in (the same count as on the way, CrowdAt): how many, and the highest level.
    [[nodiscard]] uint8 GroupAtPoint(uint32 mapId, float x, float y, float z, TeamId team, uint8& level) const
    {
        return CrowdAt(mapId, x, y, z, team, level);
    }

    // Takes the nodes within `radius` of a spot out of the graph until the next Load: the ground guard refused the walk
    // to one of them (a node the navmesh does not reach). Returns their names, empty when there was none.
    // Only nodes within `heightYards` of `z` (a lift's stops stand one above the other: a refused step to the stop on a
    // rim must not take the one on the floor below out).
    std::string BlockNodesNear(uint32 mapId, float x, float y, float z, float radius, float heightYards = 12.0f);

    [[nodiscard]] std::size_t GetNodeCount() const { return _nodes.size(); }
    [[nodiscard]] std::size_t GetLinkCount() const { return _links.size(); }

private:
    RoadMap() = default;

    struct Node
    {
        uint32 id = 0;
        uint32 mapId = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        std::string name;
        bool blocked = false;              // not for routes or rescues: a dungeon entrance, the far side of a teleporter,
                                           // or a node a bot could not walk to
        uint8 guard[2] = { 0, 0 };         // by team: the highest level of a hostile creature standing on the node
        // The node of a ship or zeppelin: it lies where the ship floats, at the water's height, and nobody walks
        // there (the walker goes over the pier and aboard). On the way to it creatures that only swim do not count.
        bool transport = false;
        // An upper stop of a lift: it hangs where the lift's platform is when it is up, over the shaft when it is down.
        // No end of a route and no place to put a bot (a mage's routes from the Barrens' rim beside the Great Lift began
        // at such a stop, the walk "79 yd through the air" down the shaft; the rescue put it there, round 183).
        bool liftTop = false;
        // On an island of a few nodes that no walking link leaves (Acherus above the Eastern Plaguelands): the same.
        bool island = false;
        bool liftStop = false;             // a stop of a lift: the platform's place (BlockNodesNear leaves it)
        // By team: the camp the node lies in the middle of (Risk::CampAtPoint) holds for walkers up to this level
        // (0: none), how many creatures make it, their middle level.
        uint8 campWalker[2] = { 0, 0 };
        uint8 campCount[2] = { 0, 0 };
        uint8 campLevel[2] = { 0, 0 };
        mutable int8 dungeon = -1;         // 1: it lies on a dungeon's ground (looked up when first asked), 0: not
        std::vector<uint32> links;         // indices into _links, leaving this node
    };
    struct Link
    {
        uint32 from = 0, to = 0;           // indices into _nodes
        float distance = 0.0f;
        float swim = 0.0f;
        uint8 danger[2] = { 0, 0 };        // by TeamId
        uint8 crowd[2] = { 0, 0 };         // most hostile creatures within CrowdRange of one point of the path
        uint8 crowdLevel[2] = { 0, 0 };    // the highest level among those
        // By team: the strongest camp the path leads through the middle of (as Node::campWalker), and where.
        uint8 campWalker[2] = { 0, 0 };
        uint8 campCount[2] = { 0, 0 };
        uint8 campLevel[2] = { 0, 0 };
        float campX[2] = { 0.0f, 0.0f };
        float campY[2] = { 0.0f, 0.0f };
        uint32 firstPoint = 0, pointCount = 0; // into _points
        uint32 lift = 0;                   // a ride on a lift: its game object's entry (the two points are its stops)
    };
    struct Hostile
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        uint8 level = 0;
        uint8 teams = 0;                   // bit 0: hostile to the Alliance, bit 1: to the Horde
        bool patrol = false;               // a point of a patrol's path, not a spawn
        float reach = 0.0f;                // a way this close to it passes it (its aggro range with what it wanders)
        float wander = 0.0f;               // what of its random walk counts (the share the reach has): it meets others that far
        bool paired = false;               // another hostile creature stands next to it: they come together
        bool water = false;                // it only swims: no danger for a way that is not in the water
        uint32 faction = 0;                // its faction template: the soldiers of one side call one another (CrowdAt)
    };

    void IndexHostiles();
    // With a height (`z` below NoHeight is none) the distance to a creature counts in three dimensions: what lives on
    // the far side of a ridge or in a mine below the way is not on the way.
    // `waterZ`: without `z`, the height that says whether the point is in the water (for creatures that only swim).
    [[nodiscard]] uint8 DangerAt(uint32 mapId, float x, float y, TeamId team, float z = NoHeight, float waterZ = NoHeight) const;
    [[nodiscard]] uint8 CrowdAt(uint32 mapId, float x, float y, float z, TeamId team, uint8& level) const;
    [[nodiscard]] uint8 GuardAt(uint32 mapId, float x, float y, float z, TeamId team) const;
public:
    // The strongest hostile creature (a spawn or a point of a patrol's way; land creatures only) within `radius` of
    // a point, as the danger map counts it (elites higher, long rounds lower); spawns more than 30 yd above or below
    // `z` do not count. 0: none.
    [[nodiscard]] uint8 StrongestNear(uint32 mapId, float x, float y, float z, TeamId team, float radius) const;
private:
    // Whether a node lies on the ground of a dungeon: the tunnels before an instance portal (the Deadmines under the
    // Dagger Hills, where a rescue put a level-17 character among elite miners), the open parts of Maraudon or Dire
    // Maul. By the name of its zone or area, which is the dungeon's. Not for routes, not for rescues.
    [[nodiscard]] bool OnDungeonGround(Node const& node) const;

    std::vector<Node> _nodes;
    std::vector<Link> _links;
    std::vector<GuildAdventure::RoutePoint> _points;
    std::unordered_map<uint64, std::vector<Hostile>> _hostiles; // (map, cell) -> creatures
    std::unordered_set<uint64> _townCells[2];                   // by team: (map, small cell) in a town of the other side
    uint32 _patrolPoints = 0;           // points of patrol paths among them
    // The ways of hostile creatures on patrol (IndexHostiles), and the points of those that walk through the middle of
    // a camp (Load): such a patrol carries its camp along its way.
    struct PatrolWay
    {
        uint32 mapId = 0;
        uint8 teams = 0;
        float reach = 0.0f;
        std::vector<GuildAdventure::RoutePoint> points; // its spawn, then the nodes of its path
    };
    struct CampPatrol
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float reach = 0.0f;
        uint8 team = 0;                    // TeamId the camp is hostile to
        uint8 walker = 0, count = 0, level = 0; // the camp, as Risk::CampAt
        float campX = 0.0f, campY = 0.0f;   // the camp's middle
    };
    std::vector<PatrolWay> _patrolWays;
    std::unordered_map<uint64, std::vector<CampPatrol>> _campPatrols; // (map, cell) -> points
};

#define sRoadMap RoadMap::Instance()

#endif
