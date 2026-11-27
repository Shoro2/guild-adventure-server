/*
 * Dungeons: what a dungeon run needs to know about a dungeon (docs/orders.md "Dungeon runs", package K6).
 *
 * For every five-player dungeon of the readiness list (Readiness.h: the client's dungeon list within the level cap):
 *   - its entrance: the area trigger in the open world that leads into it, and where that puts a character inside
 *     (world tables areatrigger and areatrigger_teleport);
 *   - its bosses, in the order of the dungeon's encounters (DungeonEncounter.dbc through instance_encounters), each
 *     with the place it spawns at. A boss that is not spawned (it is summoned by an event) is not listed.
 *
 * A dungeon is "supported" when it has an entrance and at least one boss; a map with wings (the Scarlet Monastery,
 * Dire Maul, Maraudon) gives each wing its entrance and bosses. The classic raids are plans too (raid = true, docs/orders.md
 * "Raid runs"): an entrance that lies inside another dungeon (Blackwing Lair's, in Upper Blackrock Spire) counts with that
 * dungeon's own entrance as the place outside.
 *
 * World thread only.
 */

#ifndef MOD_GUILD_ADVENTURE_DUNGEONS_H
#define MOD_GUILD_ADVENTURE_DUNGEONS_H

#include "Define.h"
#include <string>
#include <vector>

namespace GuildAdventure
{
    struct DungeonBoss
    {
        uint32 entry = 0;
        std::string name;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        uint32 spawnId = 0;        // its spawn (a boss that walks or flies a round is found by it wherever it is)
        bool last = false;         // the encounter that ends the dungeon for the dungeon finder
        uint32 lastDungeon = 0;    // the dungeon it ends (LFGDungeons.dbc; a map with wings has one per wing)
        // A boss that an event brings (ga_dungeon_event_template): x, y, z are the place of the NPC that starts the
        // event when the group's leader picks its gossip option; `entry` is the boss that comes then.
        uint32 talkEntry = 0;
        uint32 talkOption = 0;
        bool follow = false;       // the NPC walks off: the group stays with it
        // The way the NPC walks (its waypoints): the group walks it once before it starts the event, so that the
        // NPC, which walks ahead of the group, does not run into creatures nobody has cleared.
        struct Point { float x = 0.0f, y = 0.0f, z = 0.0f; };
        std::vector<Point> route;
        uint32 timeout = 0;        // seconds the group gives the event before it goes on
        // Game objects the group uses, in this order, before it can fight a boss that is spawned but sleeps or stands
        // behind a door (Uldaman: the Altar of the Keepers opens the way, the Altar of Archaedas wakes him).
        std::vector<uint32> useObjects;
        // An area trigger whose script starts the boss's event (Upper Blackrock Spire's stadium: the waves, then Rend
        // Blackhand on Gyth). A client reports it when its player walks in; a bot has no client, so the leader walks
        // into it and reports it the client's way. 0: none.
        uint32 areaTrigger = 0;
        // Creatures that hold a boss back: it comes, or its way opens, only when none of them lives within `yards` of a
        // place (the SmartAI conditions "no living creature X within N yards" of the boss's own events and of the objects
        // of useObjects that work by themselves: Timmy the Cruel appears when the Scarlets of the square are dead,
        // Immol'thar's force field drops when the guardians of the five pylons are dead). The group clears them first.
        struct Clear
        {
            float x = 0.0f, y = 0.0f, z = 0.0f, yards = 0.0f;
            std::vector<uint32> entries;
        };
        std::vector<Clear> clearFirst;
        // A boss that is not there until a game object is used, maybe several times (ga_dungeon_event_template with
        // npc_entry 0 and a boss that has no spawn: the gong of Razorfen Downs brings two waves of spiders and then
        // Tuten'kash). x, y, z are the object's place. 0: none.
        uint32 summonObject = 0;
        // A stop that must come right before the boss of this entry (a hand-listed boss with its `before_boss`, an event
        // stop): the order of the way (OrderService::BossOrder) keeps it there. 0: it is free.
        uint32 before = 0;
        // How many times the leader picks talkOption: an NPC whose option opens another menu before the one that starts the
        // event (Majordomo Executus in his lair: "Tell me more", "What else", then the challenge), 1 for most.
        uint8 talkSteps = 1;
        // A boss the instance brings by itself, at a place of its own (ga_dungeon_event_template with npc_entry 0, no
        // object and x, y, z: Majordomo Executus comes when the seven rune bosses of Molten Core are dead). It has no
        // spawn; the group goes to its place after the bosses it comes after and waits up to `timeout` for it there.
        bool summoned = false;
        // An event stop: a talk to an NPC or an object to use.
        [[nodiscard]] bool IsEvent() const { return talkEntry || summonObject; }
    };

    struct DungeonPlan
    {
        uint32 id = 0;             // LFGDungeons.dbc (Readiness.h)
        uint32 mapId = 0;
        std::string name;
        bool supported = false;
        std::string why;           // why not, when it is not
        bool raid = false;         // a raid (Readiness.h): its group is a raid group of up to `size` characters
        uint8 size = 5;
        uint32 outMap = 0;         // the entrance in the open world
        float outX = 0.0f, outY = 0.0f, outZ = 0.0f;
        float inX = 0.0f, inY = 0.0f, inZ = 0.0f, inO = 0.0f; // where it leads
        std::vector<DungeonBoss> bosses;
    };
}

class Dungeons
{
public:
    static Dungeons& Instance();

    // After Readiness::Load.
    void Load();

    [[nodiscard]] GuildAdventure::DungeonPlan const* GetPlan(uint32 id) const;
    [[nodiscard]] std::vector<GuildAdventure::DungeonPlan> const& GetPlans() const { return _plans; }
    // For ".ga admin dungeon": every plan in a line, or one with its entrance and bosses.
    [[nodiscard]] std::vector<std::string> Describe(uint32 id) const;

private:
    Dungeons() = default;

    std::vector<GuildAdventure::DungeonPlan> _plans;
};

#define sDungeons Dungeons::Instance()

#endif
