/*
 * GroundGuard implementation. See GroundGuard.h.
 */

#include "GroundGuard.h"
#include "BotDriver.h"
#include "GuildAdventureSettings.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "PathProbe.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "StringFormat.h"
#include "Timer.h"

using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";

    // Whether the path finder has walkable ground at a point. The core looks for a polygon of the navmesh within 3 yd of
    // it across (and 50 yd up or down); for a character without one at either end it builds no way but a straight line
    // (PathGenerator::BuildPolyPath: a player "can fly" there), and with no navmesh tile at all the same. A polygon more
    // than 7 yd away (the path finder's "far from the navmesh") is no ground at the point either: the foot of a rock
    // under its face.
    bool HasNavmeshAt(Player* player, float x, float y, float z)
    {
        EnsureNavmesh(player, x, y, x, y);
        PathGenerator probe(player);
        if (!probe.CalculatePath(x, y, z, x, y, z, false))
            return false;
        if (probe.GetPathType() & (PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH | PATHFIND_FARFROMPOLY))
            return false;
        // And near the navmesh's surface, not only near a polygon: the core takes a polygon within 7 yd for one the point
        // lies on, and a point on the rock beside Light's Hope 3 to 6 yd above the slope at its foot counted as walkable
        // ground; a chase climbed onto it from there and the bot was rescued from its top again, round 161.
        constexpr float SurfaceYards = 3.0f;
        G3D::Vector3 const& surface = probe.GetActualEndPosition();
        float const dx = surface.x - x, dy = surface.y - y, dz = surface.z - z;
        return dx * dx + dy * dy + dz * dz <= SurfaceYards * SurfaceYards;
    }

    // A walk from walkable ground to a point that has none, not in the water: the bot stands off the navmesh where it
    // ends, nothing leads it on from there, and the rescue takes it off again. (A Noxious Plaguebat hangs on a rock beside
    // Light's Hope whose top the navmesh does not cover; for a character the core turns every walk to it into a straight
    // line, which from the hill above or the rock's foot runs on the ground and so passed the other rules: bots stood on
    // the rock and were rescued from it again and again, rounds 151 to 153.)
    bool EndsOffTheNavmesh(Player* player, float x, float y, float z)
    {
        if (player->GetMap()->IsInWater(player->GetPhaseMask(), x, y, z, player->GetCollisionHeight()))
            return false;
        return !HasNavmeshAt(player, x, y, z) &&
            HasNavmeshAt(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
    }
    // How far under a floor a way may end: the core puts a swimmer a hair under a lake's floor (2 yd until round 183:
    // a step that ended 1.9 yd under it passed, and the bot went on into the ground from there).
    constexpr float UnderFloorYards = 1.0f;

    // How far a swimmer's way ends under the floor of the water: out of the water there (the core counts no water under
    // the ground), no floor under the end (the terrain is the lowest floor), and the top of the ground at least 2 yd
    // above it (0 = it does not). A lake's floor rises toward its shore, and a straight swim to a creature on the slope,
    // or a chase beside it, ends inside the ground: Shamanheal fought Plague Ravagers in Lake Mereldar and stood 2.6 and
    // 3.5 yd under its floor afterwards, rescued twice from one spot, round 154. The 3 yd through solid ground a swim may
    // run (SwimmerSolid) and the steps under 3 yd that are not looked at let it through.
    float UnderTheFloor(Player* player, float x, float y, float z)
    {
        Map const* map = player->GetMap();
        uint32 const phaseMask = player->GetPhaseMask();
        // (In water only above the terrain: the core counts water down to 2 yd under the ground, so a swim that ended
        // under a lake's floor still "was in water", round 184.)
        float const terrain = map->GetGridHeight(x, y);
        bool const underTerrain = terrain > INVALID_HEIGHT && z < terrain - UnderFloorYards;
        if (!underTerrain && map->IsInWater(phaseMask, x, y, z, player->GetCollisionHeight()))
            return 0.0f;
        float const top = map->GetHeight(phaseMask, x, y, MAX_HEIGHT, true, MAX_HEIGHT);
        if (top <= INVALID_HEIGHT || z > top - UnderFloorYards)
            return 0.0f;
        return map->GetHeight(phaseMask, x, y, z + 2.0f, true, 100.0f) <= INVALID_HEIGHT ? top - z : 0.0f;
    }

    // How far a point lies under the terrain with no floor under it (0 = it does not): a walker that ends there stands
    // inside the ground. The terrain first (cheap); only a point below it asks the map for a floor (a cave's).
    float UnderTheGround(Player* player, float x, float y, float z)
    {
        Map const* map = player->GetMap();
        float const terrain = map->GetGridHeight(x, y);
        if (terrain <= INVALID_HEIGHT || z > terrain - UnderFloorYards)
            return 0.0f;
        uint32 const phaseMask = player->GetPhaseMask();
        return map->GetHeight(phaseMask, x, y, z + 2.0f, true, 100.0f) <= INVALID_HEIGHT ? terrain - z : 0.0f;
    }

    // Refusals older than this do not count any more.
    constexpr uint32 RefusalWindowMs = MINUTE * IN_MILLISECONDS;
    // One log line per character at most this often.
    constexpr uint32 LogGapMs = 30 * IN_MILLISECONDS;
    // A movement shorter than this is a step on the spot (turning, a small correction).
    constexpr float MinDistance = 3.0f;
    // A swimmer's movement may run this far through solid ground (LeavesTheGround's own limit for it).
    constexpr float SwimmerSolid = 3.0f;
}

GroundGuard& GroundGuard::Instance()
{
    static GroundGuard instance;
    return instance;
}

bool GroundGuard::MayMove(Player* player, PathGenerator const& path, bool usesPath, float x, float y, float z)
{
    if (!GetSettings().pathsStayOnGround || !player || !player->IsInWorld())
        return true;
    // Only characters the AI plays. Not on a ship or zeppelin (the deck is no floor of the map), not flying or dead
    // (a ghost may take the short way).
    if (!BotDriver::GetAI(player) || player->GetTransport() || !player->IsAlive() || player->IsInFlight() || player->IsFlying() ||
        player->GetVehicle())
        return true;
    // The grids on the way are created for the next movement (this one's path is computed already: with a grid that did
    // not exist it is a straight line, which the checks below refuse).
    EnsureNavmesh(player, player->GetPositionX(), player->GetPositionY(), x, y);
    // A swimmer leaves no ground by swimming, whatever is or is not under its feet; only a way through solid ground
    // counts for it: the straight line from the sea floor to a place ashore runs into the rising slope and ends under
    // the map (a bot 28 yd under an islet off Auberdine).
    bool const swimming = IsSwimming(player);
    // (And a walker: a step that ends inside the ground, deeper than it starts, is no way. A priest fought Blighted
    // Surges in Lake Mereldar, its swim ended a little under the lake's sloping floor, where the core counts no water,
    // and its next steps, a walker's of under 3 yd, went on into the shore: it was rescued 3.4 yd under the ground,
    // round 183. Up to 1 yd under the floor passes: the core puts a swimmer there.)
    // (Where the movement ends: a plain navmesh path is walkable ground by construction, its mesh may lie a little under
    // the terrain on a slope; a straight line ends where it was sent.)
    float underFloor = swimming ? UnderTheFloor(player, x, y, z) : 0.0f;
    if (!swimming && !(usesPath && path.GetPathType() == PATHFIND_NORMAL))
    {
        G3D::Vector3 end(x, y, z);
        if (usesPath && path.GetPath().size() >= 2)
            end = path.GetPath().back();
        if (float const deep = UnderTheGround(player, end.x, end.y, end.z);
            deep > 0.0f && deep > UnderTheGround(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ()) - 0.25f)
            underFloor = deep;
    }
    // A short step up onto a place without navmesh is looked at too: a chase climbed the face of the rock beside Light's
    // Hope in steps of 3 yd and less toward the plaguebat on its top, round 154.
    // (Steps of half a yard up count: a chase moves in small ones.)
    // (And from a place without navmesh too: once the first step is off it, the next ones started there and passed. A
    // hunter's chase of a Living Decay at a Rich Thorium Vein in the Eastern Plaguelands climbed 17 yd up the slope
    // beside the vein so, round 179.)
    // (And a straight line of any length from a place without navmesh: EndsOffTheNavmesh asks only for a walk that
    // starts on it, so once a bot stood on the rock's slope beside Light's Hope (off the navmesh, 6 yd above its foot)
    // an 8 yd straight line up onto its top passed every rule and it was rescued from there, round 221. Up onto ground
    // the navmesh does not cover is no way from anywhere.)
    constexpr float StepRiseYards = 0.5f;
    float const distance = player->GetExactDist(x, y, z);
    bool const climbsOffMesh = !swimming && (distance < MinDistance || !usesPath) && z - player->GetPositionZ() >= StepRiseYards &&
        !player->GetMap()->IsInWater(player->GetPhaseMask(), x, y, z, player->GetCollisionHeight()) && !HasNavmeshAt(player, x, y, z);
    if (!underFloor && !climbsOffMesh && distance < MinDistance)
        return true;
    uint32 const now = getMSTime();
    bool pass = false;
    {
        std::lock_guard lock(_lock);
        if (auto const free = _freeUntil.find(player->GetGUID()); free != _freeUntil.end())
        {
            if (now < free->second)
                return true;
            _freeUntil.erase(free);
        }
        auto itr = _passUntil.find(player->GetGUID());
        if (itr != _passUntil.end())
        {
            if (now < itr->second)
                pass = true;
            else
                _passUntil.erase(itr);
        }
    }

    ++_checked;
    PathReport const report = ProbeMovement(player, path, usesPath, x, y, z);
    // Up through the air: the air a walk may pass is a drop, a way down. A straight line that rises to a place above
    // over the air is no way at all: seven bots walked "9 to 23 yd through the air" onto a rock beside Light's Hope where
    // a plaguebat hangs, 10 yd above where they stood and off the navmesh, and were rescued from it one after the
    // other, round 151.
    constexpr float ClimbRiseYards = 3.0f;
    constexpr float ClimbAirYards = 5.0f;
    bool const climbsAir = !swimming && report.valid && !usesPath && z - player->GetPositionZ() >= ClimbRiseYards &&
        report.offGround - report.throughSolid >= ClimbAirYards;
    bool const offMesh = climbsOffMesh || (!swimming && report.valid && !usesPath && distance >= MinDistance &&
        EndsOffTheNavmesh(player, x, y, z));
    // A jump down into deep water inside a dungeon: the way a player leaves a ledge above a pool (the Wailing Caverns'
    // plateau of Lord Serpentis and Verdan the Everliving lies 28 yd above the water of the cave below it, and no walk
    // leads back down; DungeonNav's drops into water). Straight down through the air only, into water at its end.
    // (Onto ground a drop of up to 30 yd: the plateau falls 27 to 29 yd onto the floor of the cave below, the one way off
    // it.)
    constexpr float WaterDropYards = 55.0f;
    constexpr float GroundDropYards = 30.0f;
    float const fall = player->GetPositionZ() - z;
    // (And across water to a shore at about the same height: DungeonNav's swims, the island of Lady Sarevess.)
    constexpr float SwimRiseYards = 5.0f;
    float const midX = (player->GetPositionX() + x) / 2.0f, midY = (player->GetPositionY() + y) / 2.0f;
    bool const swimsAcross = !usesPath && report.valid && player->GetMap()->IsDungeon() && report.throughSolid <= 1.0f &&
        std::fabs(fall) <= SwimRiseYards && player->GetExactDist2d(x, y) >= 4.0f &&
        player->GetMap()->IsInWater(player->GetPhaseMask(), midX, midY, std::min(z, player->GetPositionZ()) - 0.5f,
            player->GetCollisionHeight());
    bool const intoWater = swimsAcross || (!swimming && !usesPath && report.valid && player->GetMap()->IsDungeon() &&
        report.throughSolid <= 1.0f && fall >= 2.0f && (fall <= GroundDropYards || (fall <= WaterDropYards &&
        player->GetMap()->IsInWater(player->GetPhaseMask(), x, y, z + 0.5f, player->GetCollisionHeight()))));
    bool const leaves = underFloor > 0.0f || (report.valid && !intoWater &&
        (swimming ? report.throughSolid > SwimmerSolid : (report.LeavesTheGround() || climbsAir || offMesh)));
    // A drop the bot could not climb back from: the walk jumps down a ledge, and from below no way leads back to where
    // it stands. (The guild leader dropped into the Searing Gorge's Cauldron, whose floor only the scaffold lifts join
    // to the rim, fought there and was taken out by a rescue; a target down there is not worth the jump.)
    constexpr float DropAir = 6.0f;
    constexpr float DungeonDropYards = 25.0f;
    float dropX = 0.0f, dropY = 0.0f, dropZ = INVALID_HEIGHT;
    bool noWayBack = false;
    // (A way back to where the bot stands is only asked for where it stands on walkable ground: from below no path leads
    // to a place the navmesh does not cover, so off the navmesh every way down was refused, and the way down is the way
    // out. The hunter of round 179 stood on the slope beside the vein, its step down 13 yd to the floor refused for "no
    // way back up", and was rescued.)
    int startOnMesh = -1;
    auto standsOnMesh = [&]()
    {
        if (startOnMesh < 0)
            startOnMesh = HasNavmeshAt(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ()) ? 1 : 0;
        return startOnMesh == 1;
    };
    if (!leaves && !swimming && !pass && !intoWater && report.valid && report.offGround - report.throughSolid >= DropAir && standsOnMesh())
    {
        dropX = report.worstX;
        dropY = report.worstY;
        dropZ = player->GetMap()->GetHeight(player->GetPhaseMask(), dropX, dropY, report.worstZ, true, 60.0f);
        noWayBack = dropZ > INVALID_HEIGHT && player->GetPositionZ() - dropZ >= DropAir &&
            !WalkReaches(player, dropX, dropY, dropZ, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
        // Inside a dungeon a drop of up to 25 yd is a way on: its levels are joined that way, and the run leads the group
        // on from below (Dire Maul West, 2026-11-21: the group stood 15 minutes on top of Immol'thar's force field, 18
        // yd above the way to Prince Tortheldrin, every step down refused).
        if (noWayBack && player->GetMap()->IsDungeon() && player->GetPositionZ() - dropZ <= DungeonDropYards)
            noWayBack = false;
    }
    // A climb the bot could not walk back down from: the movement ends some yards above it on ground the path finder
    // joins to nothing below (a rock top). A paladin grinding in the Eastern Plaguelands fought a Cannibal Ghoul that
    // wanders at the foot of a rock beside Terrordale, ended on the rock's top 7 yd above, and stood there cut off from
    // every road until the rescue took it down, round 174: from the top every path is "incomplete" and ends at its edge.
    constexpr float ClimbNoWayBackYards = 2.5f;
    bool climbNoWayBack = false;
    // (Where the movement ends: a path that ends below the rock's top leads nowhere new.)
    if (!leaves && !noWayBack && !swimming && !pass && report.valid && !player->GetMap()->IsDungeon() &&
        report.endZ - player->GetPositionZ() >= ClimbNoWayBackYards && !(usesPath && path.GetPathType() == PATHFIND_NORMAL) &&
        standsOnMesh())
        climbNoWayBack = !WalkReaches(player, report.endX, report.endY, report.endZ, player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ());
    // With the check off (the way out of a dead end) a short cut passes: through a wall's corner, off a cliff. A walk
    // through a hill does not: it ends under the map.
    if ((!leaves && !noWayBack && !climbNoWayBack) ||
        (pass && !underFloor && report.throughSolid <= GroundPassSolid && report.offGround - report.throughSolid <= GroundPassAir))
    {
        std::lock_guard lock(_lock);
        Passed& passed = _passed[player->GetGUID()];
        passed.at = now;
        passed.fromX = player->GetPositionX();
        passed.fromY = player->GetPositionY();
        passed.fromZ = player->GetPositionZ();
        passed.x = x;
        passed.y = y;
        passed.z = z;
        passed.report = report;
        passed.byPass = leaves;
        return true;
    }

    ++_refused;
    bool log = false;
    uint32 count = 0;
    {
        std::lock_guard lock(_lock);
        GroundRefusals& refusals = _refusals[player->GetGUID()];
        if (!refusals.count || getMSTimeDiff(refusals.firstAt, now) > RefusalWindowMs)
        {
            refusals.count = 0;
            refusals.firstAt = now;
        }
        if (!refusals.loggedAt || getMSTimeDiff(refusals.loggedAt, now) > LogGapMs)
        {
            log = true;
            refusals.loggedAt = now;
        }
        count = ++refusals.count;
        refusals.lastAt = now;
        refusals.mapId = player->GetMapId();
        refusals.x = x;
        refusals.y = y;
        refusals.z = z;
        refusals.solid = report.throughSolid;
        refusals.air = report.offGround - report.throughSolid;
    }
    // NewRpg walking to a quest giver it passes would try again for ever: it gives that giver up now. (Not for a
    // fight's chase, which asks the same hook since core patch CP-13.)
    bool const chase = player->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE;
    bool const gaveUp = !chase && BotDriver::GiveUpQuestGiverAt(player, x, y, z);
    if ((log || gaveUp) && underFloor > 0.0f)
        LOG_INFO(ActivityLog, "activity: {} does not {} from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f}){}: the way ends {:.1f} yd "
            "under the {} ({}){}", player->GetName(), swimming ? "swim" : "walk", player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ(), x, y, z, chase ? " (its chase in a fight)" : "", underFloor,
            swimming ? "floor of the water" : "ground", DescribeShort(report), gaveUp ? "; it leaves the quest giver there alone" : "");
    else if ((log || gaveUp) && noWayBack)
        LOG_INFO(ActivityLog, "activity: {} does not walk from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f}): the way drops {:.0f} yd "
            "at ({:.0f}, {:.0f}) and no way leads back up from there{}", player->GetName(), player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ(), x, y, z, player->GetPositionZ() - dropZ, dropX, dropY, gaveUp ? "; it leaves the quest giver there alone" : "");
    else if ((log || gaveUp) && climbNoWayBack)
        LOG_INFO(ActivityLog, "activity: {} does not walk from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f}){}: it would climb {:.0f} yd "
            "onto ground from which no way leads back down{}", player->GetName(), player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ(), x, y, z, chase ? " (its chase in a fight)" : "", report.endZ - player->GetPositionZ(),
            gaveUp ? "; it leaves the quest giver there alone" : "");
    else if ((log || gaveUp) && offMesh && !report.LeavesTheGround() && !climbsAir)
        LOG_INFO(ActivityLog, "activity: {} does not walk from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f}){}: the path finder has no "
            "walkable ground there (a straight line of {:.0f} yd onto it){}", player->GetName(), player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ(), x, y, z, chase ? " (its chase in a fight)" : "", player->GetExactDist(x, y, z),
            gaveUp ? "; it leaves the quest giver there alone" : "");
    else if (log || gaveUp)
        LOG_INFO(ActivityLog, "activity: {} does not walk from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f}){}: the way leaves the "
            "ground ({}){}", player->GetName(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), x, y, z,
            chase ? " (its chase in a fight)" : "", DescribeShort(report), gaveUp ? "; it leaves the quest giver there alone" : "");
    else if (GetSettings().debug)
        LOG_INFO("module", "mod-guild-adventure: ground guard: {} refused again ({} in a row): {}", player->GetName(), count,
            DescribeShort(report));
    return false;
}

bool GroundGuard::MayLeap(Player* player, SpellInfo const* spell, float x, float y, float z, char const* targetName)
{
    if (!GetSettings().pathsStayOnGround || !player || !spell || !player->IsInWorld())
        return true;
    if (!BotDriver::GetAI(player) || player->GetTransport() || !player->IsAlive() || player->IsInFlight() || player->IsFlying() ||
        player->GetVehicle() || IsSwimming(player))
        return true;
    if (!EndsOffTheNavmesh(player, x, y, z))
        return true;
    ++_refused;
    uint32 const now = getMSTime();
    bool log = false;
    {
        std::lock_guard lock(_lock);
        uint32& loggedAt = _leapLoggedAt[player->GetGUID()];
        if (!loggedAt || getMSTimeDiff(loggedAt, now) > LogGapMs)
        {
            loggedAt = now;
            log = true;
        }
    }
    if (log)
        LOG_INFO(ActivityLog, "activity: {} does not cast {} at {} ({:.0f}, {:.0f}, {:.0f}): the path finder has no walkable ground there "
            "(it would leap {:.0f} yd onto it)", player->GetName(), spell->SpellName[0], targetName, x, y, z, player->GetExactDist(x, y, z));
    return false;
}

bool GroundGuard::HasGroundAt(Player* player, float x, float y, float z)
{
    if (player->GetMap()->IsInWater(player->GetPhaseMask(), x, y, z, player->GetCollisionHeight()))
        return true;
    return HasNavmeshAt(player, x, y, z);
}

bool GroundGuard::WouldCancel(Player* player, float x, float y, float z) const
{
    if (!GetSettings().pathsStayOnGround || !player || !player->IsInWorld() || !player->FindMap())
        return false;
    EnsureNavmesh(player, player->GetPositionX(), player->GetPositionY(), x, y);
    PathGenerator path(player);
    bool const result = path.CalculatePath(x, y, z, false);
    bool const usesPath = result && !(path.GetPathType() & PATHFIND_NOPATH) && path.GetPath().size() > 2;
    PathReport const report = ProbeMovement(player, path, usesPath, x, y, z);
    // (Up through the air as in MayMove.)
    bool const climbsAir = !usesPath && ((z - player->GetPositionZ() >= 3.0f && report.offGround - report.throughSolid >= 5.0f) ||
        EndsOffTheNavmesh(player, x, y, z));
    if (IsSwimming(player) && UnderTheFloor(player, x, y, z) > 0.0f)
        return true;
    return report.valid && (report.LeavesTheGround() || (climbsAir && !IsSwimming(player)));
}

GroundRefusals GroundGuard::GetRefusals(ObjectGuid guid, uint32 windowMs, bool take)
{
    std::lock_guard lock(_lock);
    auto itr = _refusals.find(guid);
    if (itr == _refusals.end())
        return {};
    GroundRefusals result = itr->second;
    if (getMSTimeDiff(result.lastAt, getMSTime()) > windowMs)
        result.count = 0;
    if (take)
        itr->second.count = 0;
    return result;
}

std::string GroundGuard::DescribeLastPassed(ObjectGuid guid)
{
    std::lock_guard lock(_lock);
    auto itr = _passed.find(guid);
    if (itr == _passed.end())
        return std::string();
    Passed const& passed = itr->second;
    return Acore::StringFormat("{} s ago from ({:.0f}, {:.0f}, {:.0f}) to ({:.0f}, {:.0f}, {:.0f}): {}{}",
        getMSTimeDiff(passed.at, getMSTime()) / IN_MILLISECONDS, passed.fromX, passed.fromY, passed.fromZ, passed.x, passed.y, passed.z,
        passed.report.valid ? DescribeShort(passed.report) : "no path", passed.byPass ? ", let through with the ground check off" : "");
}

void GroundGuard::Forget(ObjectGuid guid)
{
    std::lock_guard lock(_lock);
    _refusals.erase(guid);
    _passUntil.erase(guid);
    _freeUntil.erase(guid);
}

void GroundGuard::FreeFor(ObjectGuid guid, uint32 durationMs)
{
    std::lock_guard lock(_lock);
    _freeUntil[guid] = getMSTime() + durationMs;
    _refusals.erase(guid);
}

void GroundGuard::AllowFor(ObjectGuid guid, uint32 durationMs)
{
    std::lock_guard lock(_lock);
    _passUntil[guid] = getMSTime() + durationMs;
    _refusals.erase(guid);
}

// ---------------------------------------------------------------------------------------------------------------
// Script
// ---------------------------------------------------------------------------------------------------------------

class GuildAdventureGroundPlayerScript : public PlayerScript
{
public:
    GuildAdventureGroundPlayerScript() : PlayerScript("GuildAdventureGroundPlayerScript", {
        PLAYERHOOK_ON_CAN_START_POINT_MOVEMENT
    }) { }

    // Core patch CP-10: before a point movement with a generated path starts; map thread.
    bool OnPlayerCanStartPointMovement(Player* player, PathGenerator const& path, bool usesPath, float x, float y, float z) override
    {
        return sGroundGuard.MayMove(player, path, usesPath, x, y, z);
    }
};

// A charge is a movement the core starts by itself (Spell::EffectCharge, MotionMaster::MoveCharge): no point movement asks
// the guard about it, and core patch CP-13 covers the chase only. Since walks onto the rock beside Light's Hope were refused
// (round 153), only warriors and a feral druid got onto its top, by Charge at a Noxious Plaguebat hanging there: Sheesh,
// Cleaveyo, Lilwar, Baumi three times in an afternoon, each rescued from it, round 163.
class GuildAdventureGroundSpellScript : public AllSpellScript
{
public:
    GuildAdventureGroundSpellScript() : AllSpellScript("GuildAdventureGroundSpellScript", { ALLSPELLHOOK_ON_SPELL_CHECK_CAST }) { }

    void OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& res) override
    {
        if (res != SPELL_CAST_OK || !spell)
            return;
        Player* player = spell->GetCaster() ? spell->GetCaster()->ToPlayer() : nullptr;
        SpellInfo const* info = spell->GetSpellInfo();
        if (!player || !info || !(info->HasEffect(SPELL_EFFECT_CHARGE) || info->HasEffect(SPELL_EFFECT_CHARGE_DEST) ||
            info->HasEffect(SPELL_EFFECT_JUMP) || info->HasEffect(SPELL_EFFECT_JUMP_DEST)))
            return;
        if (Unit const* target = spell->m_targets.GetUnitTarget())
        {
            if (!sGroundGuard.MayLeap(player, info, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(),
                target->GetName().c_str()))
                res = SPELL_FAILED_NOPATH;
        }
        else if (spell->m_targets.HasDst())
        {
            WorldLocation const* dest = spell->m_targets.GetDstPos();
            if (dest && !sGroundGuard.MayLeap(player, info, dest->GetPositionX(), dest->GetPositionY(), dest->GetPositionZ(), "a place"))
                res = SPELL_FAILED_NOPATH;
        }
    }
};

void AddGuildAdventureGroundScripts()
{
    new GuildAdventureGroundPlayerScript();
    new GuildAdventureGroundSpellScript();
}
