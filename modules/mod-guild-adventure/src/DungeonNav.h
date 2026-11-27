/*
 * DungeonNav: the whole navmesh of a dungeon or raid map, loaded from the mmaps folder by the module itself, and walks
 * on it of any length (docs/orders.md "Walks of a dungeon group on the whole navmesh").
 *
 * The core's path finder (PathGenerator) stops after 148 polygons and only sees the tiles of loaded grids. A dungeon's
 * ways are long and wind: Shadowfang Keep's way from its entrance to Odo the Blindwatcher, 23 yd above it, is 620 yd
 * through the courtyard and the stables. The core's path towards Odo ends below him, the module's walk check
 * (WalkReaches, which wants every leg to get 15 yd nearer) found no way, and the group was put on his floor by a
 * teleport. Players walk that way; with this the leader walks it too: its walks inside a dungeon go to a point some
 * yards along the whole way (OrderService::ViaDungeonPath), and "no way on foot" means this finds no way.
 *
 * Called on the world thread only (the planner); the query is its own (65535 nodes), not the map's.
 */

#ifndef GUILD_ADVENTURE_DUNGEON_NAV_H
#define GUILD_ADVENTURE_DUNGEON_NAV_H

#include "Define.h"
#include <source_location>
#include <string>
#include <vector>

class Map;

namespace GuildAdventure
{
    struct NavPoint
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        bool drop = false; // this point is reached by a drop from the one before it (down a ledge, as players jump)
        bool swim = false; // ... and that drop is a swim across water to a shore at about the same height
    };

    class DungeonNav
    {
    public:
        static DungeonNav& Instance();

        // The walk on the map's navmesh from one place to another with the filter the core gives bots (ground and
        // water, not steep ground, lava or slime). A way may go down from one part of the navmesh to another that no
        // walk joins it to, where a ledge ends 2 to 12 yd above the part below (a drop: players jump down there; with
        // `map` only drops whose fall is clear of walls). False when the navmesh has no way there. `corners` are the
        // straight path's corners, the first and last at the ends, a drop's landing marked; `length` the walk's yards.
        bool FindWay(uint32 mapId, float fromX, float fromY, float fromZ, float toX, float toY, float toZ,
            std::vector<NavPoint>* corners = nullptr, float* length = nullptr, Map const* map = nullptr, std::string* why = nullptr,
            std::source_location where = std::source_location::current());

        // The point `ahead` yards along the walk from `from` to `to` (the end when the walk is shorter). A drop on the
        // way before that: the point is its landing (marked `drop`) and `dropTop` the edge it is jumped from. False
        // without a way.
        bool PointAlong(uint32 mapId, float fromX, float fromY, float fromZ, float toX, float toY, float toZ, float ahead,
            NavPoint& point, float* length = nullptr, NavPoint* dropTop = nullptr, Map const* map = nullptr);

        // A drop or swim from about `top` to about `land` that did not get a character there (it stopped on something
        // the navmesh does not show above the landing): no way uses it again until the server restarts. The number of
        // drops taken out.
        uint32 BlockDrop(uint32 mapId, float topX, float topY, float topZ, float landX, float landY, float landZ);

        // The point of the navmesh (the bots' filter) nearest to (x, y, z) within `radius` yd of it: where a character
        // can stand in a sphere around a point (an area trigger's middle in the air). False without one.
        bool NearestOnMesh(uint32 mapId, float x, float y, float z, float radius, NavPoint& point);

        // Points every `step` yd on the navmesh's surface along the straight line from `from` to `to` (a leg of a way:
        // the line lies on the navmesh), each at the surface's height, the last at `to` (or where the surface ends). A
        // walk along them follows the ground without the core's path finder. False when `from` has no navmesh.
        bool SurfaceLine(uint32 mapId, float fromX, float fromY, float fromZ, float toX, float toY, float toZ, float step,
            std::vector<NavPoint>& points);

    private:
        DungeonNav() = default;
        bool FindWayLocked(uint32 mapId, float fromX, float fromY, float fromZ, float toX, float toY, float toZ,
            std::vector<NavPoint>* corners, float* length, Map const* map, std::string* why);
    };
}

#define sDungeonNav GuildAdventure::DungeonNav::Instance()

#endif
