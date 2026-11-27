/*
 * GroundGuard: AI-played characters do not walk through the ground (docs/orders.md "Paths through the ground").
 *
 * Core patch CP-10 asks before every point movement of a player whether it may start. For a character the AI plays the
 * guard probes the movement (PathProbe) and cancels it when it leaves the ground: the bot stays where it is instead of
 * running a straight line through a hill into the mine underneath. The order planner reads the refusals and sends the
 * bot somewhere else (OrderService::UpdateBot).
 *
 * The hook runs on map threads; the refusal list is guarded by a mutex.
 */

#ifndef MOD_GUILD_ADVENTURE_GROUND_GUARD_H
#define MOD_GUILD_ADVENTURE_GROUND_GUARD_H

#include "PathProbe.h"
#include "ObjectGuid.h"
#include <atomic>
#include <mutex>
#include <unordered_map>

class PathGenerator;
class Player;
class SpellInfo;

namespace GuildAdventure
{
    // The movements of one character the guard cancelled lately.
    struct GroundRefusals
    {
        uint32 count = 0;              // within the window
        uint32 firstAt = 0;            // getMSTime()
        uint32 lastAt = 0;
        uint32 loggedAt = 0;           // getMSTime() of the last activity log line about it
        uint32 mapId = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f; // destination of the last one
        float solid = 0.0f;            // yards of the last one through solid ground ...
        float air = 0.0f;              // ... and through the air
    };
}

namespace GuildAdventure
{
    // While the ground check is off for a character (the way out of a dead end), a movement may still run only this far
    // through solid ground and through the air.
    constexpr float GroundPassSolid = 12.0f;
    constexpr float GroundPassAir = 40.0f;
}

class GroundGuard
{
public:
    static GroundGuard& Instance();

    // Map thread (core patch CP-10). False cancels the movement.
    bool MayMove(Player* player, PathGenerator const& path, bool usesPath, float x, float y, float z);
    // For code that moves an AI-played character with MotionMaster::MovePoint itself (a flight from a fight): would
    // the guard cancel a point movement to (x, y, z)? Computes the path the core would use; nothing is recorded. The
    // character's map thread, or the world thread outside map updates.
    [[nodiscard]] bool WouldCancel(Player* player, float x, float y, float z) const;
    // Whether a point is walkable ground for the path finder (a navmesh polygon at its surface), or water a bot swims in:
    // a walk to a point without either is refused (EndsOffTheNavmesh), so no task should lead there.
    static bool HasGroundAt(Player* player, float x, float y, float z);
    // Map thread (the spell check): a charge or leap of an AI-played character (Charge, Intercept, Feral Charge) moves
    // it too, and the core starts it with no point movement the guard is asked about. False refuses the spell: it would
    // end where the path finder has no walkable ground.
    bool MayLeap(Player* player, SpellInfo const* spell, float x, float y, float z, char const* targetName);

    // World thread: the refusals of a character within the last `windowMs`, and forgets them when `take` is set.
    GuildAdventure::GroundRefusals GetRefusals(ObjectGuid guid, uint32 windowMs, bool take);
    void Forget(ObjectGuid guid);
    // World thread: the character's movements are not checked for this long (the planner's way out for a bot that is
    // stuck on a spot the navmesh does not connect to anything).
    void AllowFor(ObjectGuid guid, uint32 durationMs);
    // Any thread: no movement of the character is checked at all for this long (a place the navmesh does not cover and
    // the collision data takes for solid ground, where a raid's mechanic sends it: C'Thun's stomach).
    void FreeFor(ObjectGuid guid, uint32 durationMs);
    // The last movement of the character the guard let through ("12 s ago from (..) to (..): normal, 14 points"), for
    // the log line of a bot that is found under the ground. Empty when there was none.
    [[nodiscard]] std::string DescribeLastPassed(ObjectGuid guid);

    [[nodiscard]] uint64 GetChecked() const { return _checked; }
    [[nodiscard]] uint64 GetRefused() const { return _refused; }

private:
    GroundGuard() = default;

    std::mutex _lock;
    std::unordered_map<ObjectGuid, GuildAdventure::GroundRefusals> _refusals;
    std::unordered_map<ObjectGuid, uint32> _passUntil; // getMSTime()
    std::unordered_map<ObjectGuid, uint32> _freeUntil; // getMSTime()
    std::unordered_map<ObjectGuid, uint32> _leapLoggedAt; // getMSTime()

    // The last movement the guard let through, per character: for the log line of a bot found under the ground.
    struct Passed
    {
        uint32 at = 0;                 // getMSTime()
        float fromX = 0.0f, fromY = 0.0f, fromZ = 0.0f;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        GuildAdventure::PathReport report;
        bool byPass = false;           // it left the ground and was let through because the check was off
    };
    std::unordered_map<ObjectGuid, Passed> _passed;
    std::atomic<uint64> _checked{0};
    std::atomic<uint64> _refused{0};
};

#define sGroundGuard GroundGuard::Instance()

#endif
