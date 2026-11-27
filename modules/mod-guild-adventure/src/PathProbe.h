/*
 * PathProbe: what the core's path finder returns between two points, and whether that path stays on the ground
 * (docs/orders.md "Paths through the ground").
 *
 * The core moves a server-driven player (a bot) along the PathGenerator's result, and the result is not always a
 * walkable path. The case the owner saw on 2026-11-12: a bot on the hill above the Fargodeep Mine attacks a kobold in the
 * tunnel under it (playerbots takes targets up to 5 yards above or below, and the line of sight check does not know the
 * terrain). The path finder gets as near as the navmesh allows, which is the spot right above the kobold, and the last
 * step is a path of two points: the point movement turns that into a straight line to the destination, down through
 * the rock. Other ways to the same result: no navmesh polygon at the start or the end (a straight line that still
 * counts as a normal path for a player), or a start polygon found on another level.
 *
 * The probe samples the movement every yard and looks for a floor under it: a stretch without one, or a step from one
 * level to another without a slope, means the path leaves the ground.
 *
 * World thread or the map thread of the source's map.
 */

#ifndef MOD_GUILD_ADVENTURE_PATH_PROBE_H
#define MOD_GUILD_ADVENTURE_PATH_PROBE_H

#include "Define.h"
#include <string>
#include <vector>

class Map;
class PathGenerator;
class Unit;
class WorldObject;

namespace GuildAdventure
{
    struct PathReport
    {
        bool valid = false;            // the path finder ran
        uint32 type = 0;               // PathType flags
        std::size_t points = 0;
        float length = 0.0f;           // along the movement
        float straight = 0.0f;         // start to end, 3D
        float offGround = 0.0f;        // yards of the movement with no floor under the feet
        float throughSolid = 0.0f;     // of those, yards deep below the surface (through rock or a building); the rest
                                       // runs through the air (off a ledge, down a slope the navmesh does not cover)
        uint32 breaks = 0;             // steps from one level to another without a slope
        float worstX = 0.0f, worstY = 0.0f, worstZ = 0.0f; // where the movement is farthest from a floor
        float worstGap = 0.0f;
        float endX = 0.0f, endY = 0.0f, endZ = 0.0f; // where the movement ends
        bool straightLine = false;     // the point movement does not follow a path but runs a straight line

        // True when a bot should not be moved along this path: more than a step through solid ground, or farther
        // through the air than a jump off a ledge.
        [[nodiscard]] bool LeavesTheGround() const { return throughSolid > 3.0f || offGround - throughSolid > 25.0f; }
    };

    // Creates the grids of the source's map along the straight line between two points (their terrain, collision and
    // navmesh tile; no creatures) where they do not exist yet. The navmesh of a grid that was not created answers no for
    // every way into it, and after a restart only the grids around the characters in the world exist: a level-60 mage
    // that entered the world 160 yd from a grid's edge found every road node beyond it out of reach ("cut off from every
    // road") and was rescued onto Acherus above it (round 200). Lines longer than 3000 yd only at their ends. Grids are
    // not unloaded by the core once created. World thread or the map thread of the source's map.
    void EnsureNavmesh(WorldObject const* source, float x, float y, float destX, float destY);

    // The path from (x, y, z) to (destX, destY, destZ) on the source's map, as the point movement of `source` would
    // compute it, sampled along its whole length.
    PathReport ProbePath(WorldObject const* source, float x, float y, float z, float destX, float destY, float destZ);

    // The movement the core is about to start for `unit` (core patch CP-10): along `path` when `usesPath`, else a
    // straight line to the destination. A plain navmesh path is sampled at its ends only, anything else all the way.
    PathReport ProbeMovement(Unit const* unit, PathGenerator const& path, bool usesPath, float destX, float destY, float destZ);

    std::string DescribePathType(uint32 type);

    // True when the path finder leads a walker from one spot to the other. Its search covers a limited stretch, so a far
    // target gives an "incomplete" path whether or not there is a way: the walk is followed path by path, as NewRpg's
    // long-distance walk does, until one path arrives (true) or one makes no headway (false: a cliff, a mountain range,
    // a spot off the navmesh). `length` gets the yards walked. A few path searches; for decisions, not for every tick.
    // The answers are remembered (docs/performance.md PF-2, GuildAdventure.Planner.RouteCache): by map, the walker's
    // kind and both ends rounded to 8 yd, ten minutes for a yes, three for a no.
    bool WalkReaches(WorldObject const* source, float x, float y, float z, float destX, float destY, float destZ, float* length = nullptr);
    std::size_t WalkCacheSize();
    void ClearWalkCache();

    // True when one path of the path finder from where `source` stands ends at the spot on its own floor (within a few
    // yards, its height included). For a spot near by: a town's houses have floors one above the other, and a path to a
    // spot on an upper floor that no stair leads to ends on the floor below it.
    bool PathEndsAt(WorldObject const* source, float destX, float destY, float destZ);

    // The walkable ground of the map's navmesh nearest to a point, within `across` yd sideways and `upDown` yd up or
    // down (the grid there is created first). The points mod-playerbots recorded for its travel node graph lie under or
    // over that ground in places: the road up to Thunder Bluff's Hunter Rise 3 yd under the ramp's surface.
    bool NavmeshGroundNear(WorldObject const* source, float x, float y, float z, float across, float upDown, float& groundX,
        float& groundY, float& groundZ);

    // The point about `ahead` yards along the path finder's path from (x, y, z) towards a destination (its last point
    // when the path is shorter). False when it has no path worth walking: none, a straight line, fewer than 15 yd.
    bool PathPointAhead(WorldObject const* source, float x, float y, float z, float destX, float destY, float destZ, float ahead,
        float& outX, float& outY, float& outZ);

    // Where the navmesh begins for a walker at (x, y, z) who wants to go towards a far point: the first point of the
    // path finder's path. False when it has no path at all. A spot more than a few yards from that point is off the
    // navmesh (the top platform of a zeppelin tower: the mesh begins on the landing below).
    bool NavmeshStart(WorldObject const* source, float x, float y, float z, float towardX, float towardY, float towardZ, float& outX,
        float& outY, float& outZ);

    // In water (swimming or under it) and under water, by the map. Player::IsInWater() is set from the client's
    // movement packets only, so for a character the server moves (a bot) it always says no.
    bool IsSwimming(Unit const* unit);
    bool IsDiving(Unit const* unit);
    // The height to ask the map's water at for a point at (x, y, z): a few yards under the floor of a lake or the sea
    // (where a fight's straight lines under water can end) the core counts no water at all, so it is asked at the
    // floor instead. (A shaman 1.9 yd inside the bed of Blackwood Lake, 20 yd under its surface, sat down to drink there
    // and was rescued as standing off the walkable ground, round 169.)
    float WaterProbeZ(Map const* map, float x, float y, float z);
    std::string DescribeShort(PathReport const& report);
    std::vector<std::string> Describe(PathReport const& report);
}

#endif
