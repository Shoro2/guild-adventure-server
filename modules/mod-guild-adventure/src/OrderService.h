/*
 * OrderService: guild bots act on their orders (milestone M4, the design log section 2.4, design in docs/orders.md).
 *
 * Once per second, for every AI-driven guild bot, the service
 *   1. reconciles the bot with its order: strategy profile, playerbots master and party (only FOLLOW has a master
 *      and sits in the owner's party; every other order plays on its own);
 *   2. for AUTO and LEVEL_ZONE: picks the zone (AUTO: best zone for the level; LEVEL_ZONE: the ordered zone,
 *      auto-advance when out-levelled), keeps the bot supplied (vendor/repair, class trainer), travels to the zone and
 *      then runs a WAQ-style quest planner (a port of wowbot335's WaqTaskState/TaskRegister design): pick up nearby
 *      quests first, then work turn-ins and objectives nearest first, grind when nothing is left;
 *   3. for IDLE: walks to the nearest inn and holds there; for FOLLOW: follows the character the owner plays;
 *   4. writes the "why" line the roster shows and records events (arrival in a zone, zone out-levelled, auto-advance).
 *
 * The bots execute through mod-playerbots (BotDriver): NewRpg moves, talks to NPCs, works quest spots; the "grind"
 * strategy fights. Travel is on foot inside a walkable region, by flight path, and between regions by ship, zeppelin
 * and portal; the only position changes the service itself makes are the short hops between pier and deck, a ghost's
 * step to the spirit healer and the teleport of a portal (docs/orders.md "Travel", the design log section 7, O5).
 *
 * World thread only (WorldScript::OnUpdate, after the guild bot holder's update).
 */

#ifndef MOD_GUILD_ADVENTURE_ORDER_SERVICE_H
#define MOD_GUILD_ADVENTURE_ORDER_SERVICE_H

#include "BotDriver.h"
#include "ObjectGuid.h"
#include "Perf.h"
#include "QuestInteraction.h"
#include "QuestEvents.h"
#include "RoadMap.h"
#include "Timer.h"
#include "WorldIndex.h"
#include <array>
#include <functional>
#include <map>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GuildAdventure
{
    struct NavPoint;
}

class MotionTransport;
class Creature;
class GameObject;
class Object;
class Player;
class Transport;
class Quest;

namespace GuildAdventure
{
    struct AdventureGuild;
    struct BotOrder;
    struct DungeonBoss;
    struct DungeonPlan;
    struct GuildBot;
    struct ZoneTemplate;

    enum class TaskKind : uint8
    {
        None,
        Travel,     // walk to the hub of the target zone
        Pickup,     // accept quests at a quest giver
        Objective,  // work one objective of a quest in the log
        TurnIn,     // hand in a completed quest
        Grind,      // fight around a spot that fits the bot's level
        Vendor,     // sell, repair, buy food and water
        Train,      // learn new class spells
        SafeSpot,   // IDLE: walk to the nearest inn
        Hold,       // stand still (IDLE at the inn, level cap reached, nothing reachable)
        Voyage,     // take a ship or zeppelin to the next region
        Discover,   // talk to a flight master nearby to learn its flight path
        Interact,   // use a quest item, cast spells on a quest NPC, use a quest object, explore (QuestInteraction)
        Escort,     // stay with the NPC of an escort quest until the quest completes or fails
        Event,      // a quest that completes through a script event: a talk, an event at its giver, an NPC to lead
        Fish        // walk to the water and fish: for a quest's fish, or for the skill (Fishing.cpp)
    };

    enum class VoyagePhase : uint8
    {
        ToDock,     // walk to the dock
        Wait,       // wait on the pier for the ship
        Board,      // step on board while it is docked
        Ride,       // on board
        Disembark   // step onto the pier at the other dock
    };

    char const* ToString(VoyagePhase phase);

    char const* ToString(TaskKind kind);

    // Errands (OrderErrands.cpp): the travel to an ender ends this close to it, where the turn-in task takes over; an
    // errand is given up after this many stuck travels or deadly ways.
    constexpr float ErrandArriveDistance = 200.0f;
    constexpr uint8 ErrandStuckLimit = 2;

    struct BotTask
    {
        TaskKind kind = TaskKind::None;
        uint32 questId = 0;
        int32 objective = -1;          // Objective: 0-3 creature/GO, 4-9 item
        uint32 auraWaitSince = 0;      // Interact: since when the bot waits for the aura of a creature it stands near
        uint32 fetchItem = 0;          // Objective: fetches this item for the objective's interaction (a key from the
                                       // creatures that drop it); the task ends once the bot has it
        bool hasSpot = false;          // a target position is known
        SpawnPoint spot;               // NPC, game object or position
        std::string targetName;        // NPC or zone name for the "why" line
        uint32 startedAt = 0;          // getMSTime()
        uint32 nearSince = 0;          // getMSTime() when the bot first got close to the target, 0 = not yet
        uint32 lastAssert = 0;         // getMSTime() of the last NewRpg status this task set
        uint32 deaths = 0;             // deaths during this task
        bool unreachable = false;      // the NPC task ended before the bot got to the NPC
        bool restock = false;          // Vendor: the trip is for food and drink
        bool repair = false;           // Vendor: the trip is (also) for a repair
        bool sell = false;             // Vendor: the trip is (also) to sell loot for bag space
        bool seenTarget = false;       // NPC task: the NPC was in sight at least once (else it was not there)
        uint32 directSince = 0;        // NPC task: getMSTime() since the planner walks the bot to the NPC itself
        bool anyDock = false;          // Voyage: the bot was found on board without a voyage; it goes ashore at the next dock
        bool shoreLeave = false;       // Voyage: ... and that is what this voyage is (it stays so when the stop is known)
        bool flightAfterRefusal = false; // Travel: a flight was asked for after its way on foot was refused
        bool flew = false;          // a flight was booked for this task (a journey that never flew does not hold the next one back)
        bool otherLevelLooked = false; // Interact: the way to a spot on another level near by was asked for (own steps)
        uint32 buyStepInSince = 0;     // Vendor: since when it steps within the core's interaction distance to buy
        uint32 progressAt = 0;         // Objective: getMSTime() of the last progress (a kill, a fight, an item) at this spawn
        uint32 progressMark = 0;       // Objective: what that progress looked like (counters added up)
        uint32 emptySince = 0;         // Objective: getMSTime() since no target of the objective is in sight at the spawn
        uint32 fightsSeen = 0;         // Objective: the bot's count of fights at the task's last look (0: not looked yet)
        uint32 busyAt = 0;             // Objective: getMSTime() the bot last fought or rested
        ObjectGuid lootCorpse;         // Objective: the corpse of its kill it walks to for the objective's item
        uint32 lootSince = 0;          // Objective: getMSTime() since it walks to that corpse
        uint32 lootAssert = 0;         // Objective: getMSTime() of the last walk set towards it
        std::vector<ObjectGuid> lootLeft; // Objective: corpses it gave up on (not reached in time)
        uint32 nextStalk = 0;          // Objective: getMSTime() of the next look for a target the bot cannot see (stealth)
        bool stalking = false;         // Objective: the planner walks the bot onto such a target
        bool stalkRefused = false;     // Objective: a hidden target was not walked onto (said once per task)
        uint32 nextNudge = 0;          // Objective: getMSTime() from which the planner may turn the bot to a target in sight again
        bool errand = false;           // Travel: to the spot of an errand (BotRuntime::errandQuest), not to the target zone
        bool groundPass = false;       // the ground guard was switched off once for this task (the bot was stuck)
        bool approach = false;         // Objective: the planner walks the bot to the spot first (a route), then NewRpg works
        float approachDone = 0.0f;     // ... until it is this near (0: the usual distance)
        bool tookOver = false;         // Objective: NewRpg did not set off, the planner took the walk over once
        bool levelLooked = false;      // Objective: looked once whether its spot lies on another level no path reaches
        bool journeyLooked = false;    // a far spot of the task: a flight was looked for once (StartJourney, longWay)
        bool otherLevel = false;       // .. and it does
        bool questArea = false;        // Objective: at "the quest area" (no spawn: hasSpot is false, spot is the quest's point)
        bool flightTried = false;      // Travel: a flight was looked for since the task began
        char const* noFlightWhy = nullptr; // ... and why none was found (for the log line of a refused way)
        bool crowdChecked = false;     // Objective: the straight walk to the spot was looked at for a group on it
        bool hasFlightVia = false;     // Travel / Voyage: walk to this flight master first, the flight is booked there
        SpawnPoint flightVia;

        // The walk of the task over the road map (OrderService::WalkTo, OrderRoutes.cpp).
        RoadRoute route;
        bool routePlanned = false;     // a route was looked for (found or not) for routeDest
        bool routeBlocked = false;     // a step of the route had no way on the ground: the rest of the task walks straight
        bool blockedProbed = false;    // ... and whether a way to the destination was looked for since (at blockedX/Y)
        float blockedX = 0.0f, blockedY = 0.0f;
        bool straightTried = false;    // Objective: the road map's way was far around, the path finder was asked for a straight one
        float walkX = 0.0f, walkY = 0.0f; // where WalkTo last sent NewRpg (a route point or the destination)
        // A walk NewRpg does not get on with is walked by the planner itself (OrderService::OwnStep).
        bool ownSteps = false;
        uint32 stallSince = 0;         // getMSTime() since WalkTo has seen the bot at (stallX, stallY)
        uint32 stallSeen = 0;          // getMSTime() of WalkTo's last look
        float stallX = 0.0f, stallY = 0.0f;
        uint32 ownStepAt = 0;          // getMSTime() of the last own step
        float ownFromX = 0.0f, ownFromY = 0.0f; // where the own steps began
        uint8 ownRounds = 0;           // how often this task's walk went over to own steps
        uint16 ownMade = 0;            // own steps that started a movement
        uint16 ownFailed = 0;          // own steps that found no way
        uint8 ownFailStreak = 0;       // ... in a row
        std::string ownNote;           // what the last own step without a way found
        // A swim to a target no way on the ground leads to, through the water (OrderService::StepSwim).
        struct SwimPoint
        {
            float x = 0.0f, y = 0.0f, z = 0.0f;
            bool walk = false;         // a walk on the ground to the shore (the path finder), not a swim
        };
        bool swimArmed = false;        // a step towards the target was refused for having no way on the ground
        // A target on another level that the path finder reaches from a road node only: that node first (SetAsideOtherLevel,
        // ViaRoadNode).
        bool viaTried = false;
        bool viaActive = false;
        float viaX = 0.0f, viaY = 0.0f, viaZ = 0.0f;
        std::string viaName;
        // A target on another level of the open world that the core's path finder and the road map have no way to, but
        // the navmesh has (SetAsideOtherLevel): its walk follows the navmesh's way like a walk inside a dungeon.
        bool openTried = false;
        bool openWay = false;
        // The navmesh is asked once per task, and again from a place 40 yd or more from the last try, three times at
        // most: a try from where the task began found no way, and the bot stood 230 yd from there on a mesa of Thousand
        // Needles above its Incendia Agave, cut off and rescued, while the navmesh had a way of 1322 yd down from that
        // mesa (Saelanya, 29, "Sacred Fire", round 256).
        float openTriedX = 0.0f, openTriedY = 0.0f;
        uint8 openTries = 0;
        bool OpenTryDue(float x, float y)
        {
            constexpr float AgainYards = 40.0f;
            constexpr uint8 MaxTries = 3;
            if (openWay || (openTried && (openTries >= MaxTries ||
                    (x - openTriedX) * (x - openTriedX) + (y - openTriedY) * (y - openTriedY) < AgainYards * AgainYards)))
                return false;
            openTried = true;
            ++openTries;
            openTriedX = x;
            openTriedY = y;
            return true;
        }
        uint8 openRefusals = 0;     // refused steps let pass while it walks the navmesh's way (SetAsideOtherLevel)
        uint32 navRefusedAt = 0;    // ... the last of them: the leg is walked along the navmesh's surface (ViaDungeonPath)
        // Since when the bot has not moved a yard while the next point of its navmesh way lay a few yards off
        // (OrderService::ViaDungeonPath walks it there straight after a while).
        uint32 navStillSince = 0;
        float navStillX = 0.0f, navStillY = 0.0f;
        // The yards left along the navmesh's way when its next point was last found (the stuck rule measures by it).
        float navLeft = 0.0f;
        // Standing a few yards from the edge of a drop or swim of the dungeon run's way without getting to it (since when,
        // where): the drop is taken from where it stands (StepDungeonWay).
        uint32 dropStallSince = 0;
        float dropStallX = 0.0f, dropStallY = 0.0f;
        // A walk inside a dungeon follows the whole navmesh (DungeonNav): its next point some yards along the way, and when
        // that point was found (OrderService::ViaDungeonPath).
        bool navActive = false;
        float navX = 0.0f, navY = 0.0f, navZ = 0.0f;
        float navToX = 0.0f, navToY = 0.0f, navToZ = 0.0f;
        uint32 navAt = 0;
        uint32 navSplineUntil = 0;     // getMSTime(): it walks the navmesh's surface to the next point (ViaDungeonPath)
        float navSplineX = 0.0f, navSplineY = 0.0f; // the point it walks to so
        // The nearest it came to its next point and when (the walk along the surface follows when it gets no nearer).
        float navBestDist = 0.0f;
        uint32 navBestAt = 0;
        float navBestForX = 0.0f, navBestForY = 0.0f;
        bool navDrop = false;          // navX/Y/Z is the edge of a drop, navLand* where it lands
        float navLandX = 0.0f, navLandY = 0.0f, navLandZ = 0.0f;
        bool navSwim = false;          // ... and the drop is a swim across water
        bool swimLooked = false;       // the look for a swim was made
        std::vector<SwimPoint> swim;   // the way through the water, the landing last
        std::size_t swimNext = 0;
        uint32 swimMovedAt = 0;        // getMSTime() of the last movement it started
        uint32 swimLegSince = 0;       // getMSTime(): since when it swims to swimNext ...
        float swimLegBest = 0.0f;      // ... and the nearest it came
        float ownStepX = 0.0f, ownStepY = 0.0f, ownStepZ = 0.0f; // where it leads
        // A crossing of a gap in the path finder's ground (ga_path_bridge, OrderService::BridgeGap).
        uint8 bridges = 0;             // how many this task crossed
        bool bridgeLooked = false;     // the task went over to own steps once to look for one before giving up
        bool bridgeSearched = false;   // the list was looked through from where the bot stands (until one is crossed)
        bool bridgeFound = false;
        GuildAdventure::RoutePoint bridgeEdge;   // the point on the bot's side
        GuildAdventure::RoutePoint bridgeBeyond; // the point on the target's side
        // A walk shorter than Roads.MinDistance: whether the path finder has a whole path for it was looked up for this
        // destination (it gets a route when it has none: a flight master on the wall above the bot).
        bool dangerChecked = false;    // NPC task: the walk to the NPC was looked at for creatures far above the bot
        bool hubFocus = false;         // a Travel task of the hub's crafting: the walk to an anvil or forge next to the hub
        bool gatherRound = false;      // a Travel task of the profession mode: the walk to a place where a node spawns,
                                       // to a hunting ground (skinHunt) or to an anvil, a forge, a fire (focus)
        bool skinHunt = false;         // profession mode: a hunt for skins (Travel: the way there; Grind: the hunt)
        bool fishTrip = false;         // profession mode: the journey to water the Fishing skill reaches
        uint32 huntItem = 0;           // profession mode: a hunt for a material creatures drop (Travel: the way, Grind: the hunt)
        uint32 huntCount = 0;          // ... until the bags and the guild storage hold this many of it
        bool fishNear = false;         // ... to a school of fish out in the water: it ends in sight of it
        bool dungeonWalk = false;      // a Travel task of a dungeon run (OrderDungeons.cpp): to the entrance, to a boss
        bool retreat = false;          // a Travel task out of a place too dangerous to stand in (PlanRetreat)
        bool fetchSquad = false;       // a Travel task of a squad leader to a member waiting at its meeting point
        bool journey = false;          // a Travel task to the far spot of a task whose way on foot was refused (StartJourney)
        bool partnerMeet = false;      // a Travel task of a quest partner to its leader (QuestPartners.h, StepPartnerMeet)
        uint32 focus = 0;              // profession mode, Travel: the object to craft at (SpellFocusObject.dbc)
        uint8 grindMin = 0;            // Grind: the levels to fight, when the task sets them (a hunt for skins)
        uint8 grindMax = 0;
        uint32 farmItem = 0;           // the order FARM (OrderFarm.cpp): the item this task is for (Travel: the way to a
                                       // hunting ground; Grind: the hunt, only creatures that drop it are fought)
        float grindAround = 0.0f;      // Grind: how far around the spot targets are picked (0: the usual 70 yd)
        bool shortProbed = false;
        bool shortNoPath = false;
        float shortX = 0.0f, shortY = 0.0f;
        float shortFromX = 0.0f, shortFromY = 0.0f; // where the bot stood at that probe (asked again 40 yd on)
        float routeDestX = 0.0f, routeDestY = 0.0f;
        uint32 routeIndex = 0;         // the last route point the bot passed
        int32 routeTarget = -1;        // the route point NewRpg walks to
        uint32 nextRoutePlan = 0;      // getMSTime()
        uint32 routeStartAt = 0;       // getMSTime() the route was taken (0: the start was checked)
        uint32 straightSince = 0;      // getMSTime() a walk without a route began (0: none, or its start was checked)
        float straightX = 0.0f, straightY = 0.0f;
        bool noWay = false;            // that walk did not get going: the task's stuck rule fires at once
        bool farAround = false;        // Travel: the road is many times the straight line and no straight way exists
        bool townChecked = false;      // WalkTo looked whether this walk leads through a town of the other side
        bool townRoute = false;        // .. with a route or without one
        float townX = 0.0f, townY = 0.0f; // .. to this destination
        bool noFlight = false;         // Travel: the walk to the flight master was refused, no flight for this task
        bool wideStart = false;        // Travel: the bot stands in a camp; its way out is planned without that camp
        uint32 liftAt = 0;             // a ride on a lift of the route: the route point the bot boards at + 1 (0: none)
        uint32 liftEntry = 0;          // .. the lift's game object entry
        uint32 liftSince = 0;          // getMSTime() it began to wait at the stop (or boarded)
        bool liftRiding = false;       // .. it stands on the platform as its passenger
        ObjectGuid liftGuid;           // .. the platform
        float liftToX = 0.0f, liftToY = 0.0f, liftToZ = 0.0f; // .. the stop it leaves at
        // The landing beside the boarding stop of the route's next lift (LiftLanding): the stop is the platform's place,
        // over the shaft when the platform is elsewhere.
        bool liftLandLooked = false;   // .. looked for (at the stop below)
        bool liftLandOk = false;       // .. and found
        float liftLandForX = 0.0f, liftLandForY = 0.0f, liftLandForZ = 0.0f; // .. the stop it was looked for
        float liftLandX = 0.0f, liftLandY = 0.0f, liftLandZ = 0.0f;
        // A journey (Travel, Voyage) waits while a strong creature walks on its next stretch (StepRoamerWait).
        uint32 roamerWaitUntil = 0;    // getMSTime(); 0 = not waiting
        uint32 roamerWaited = 0;       // ms this task has waited so
        uint32 nextRoamerLook = 0;     // getMSTime()
        std::string roamerName;        // what it waits for
        bool surfaced = false;         // a walk that began under water went up to the surface first (or had no need to)
        bool wadeLooked = false;       // a walk that began on a floor under water looked for the way out along the surface (WadeOut)
        uint32 wadeUntil = 0;          // getMSTime(): it swims that way out until then
        // An NPC in a room only a teleporter leads into (ThroughLocalPortal).
        bool portalLooked = false;     // whether such a teleporter was looked for
        bool viaPortal = false;        // there is one, and the bot goes through it
        uint32 portalSince = 0;        // getMSTime() when it set out for the teleporter
        GuildAdventure::LocalPortal portal;
        // The way out of such a room (LeaveLocalRoom).
        bool roomLooked = false;       // whether the bot's place was looked at for this task
        bool leavesRoom = false;       // it stands in such a room and walks to the teleporter out
        GuildAdventure::LocalPortal roomExit;
        uint8 stones = 0;              // stepping stones this task has taken (TakeSteppingStone)
        uint32 stoneUntil = 0;         // getMSTime() until which the bot walks to its stepping stone (0 = none)
        float stoneX = 0.0f, stoneY = 0.0f, stoneZ = 0.0f;
        uint32 grindIdleSince = 0;     // Grind: getMSTime() since nothing to fight was at the spot
        uint32 grindPickSince = 0;     // Grind: getMSTime() since its targets were picked without a fight coming of them
        float routeStartX = 0.0f, routeStartY = 0.0f;
        uint32 buyItem = 0;            // Vendor: a quest item to buy there ...
        uint32 buyCount = 0;           // ... and how many
        // Fish (Fishing.cpp)
        uint32 fishItem = 0;           // the quest's fish (0: fishing for the skill)
        bool fishStand = false;        // the spot on the shore is found (task.spot is it then)
        bool standChecked = false;     // ... and the path finder was asked for a way to it from where the bot stood
        float fishOrientation = 0.0f;  // where the water is from there
        uint32 fishCasts = 0;          // casts that started
        uint32 fishCastLimit = 0;      // casts after which a trip for the skill ends (0: the usual number)
        uint32 fishCaught = 0;         // casts that brought something
        uint32 fishFailed = 0;         // casts in a row that did not start
        uint32 fishCastAt = 0;         // getMSTime() of the last cast
        // Event (OrderEvents.cpp)
        QuestEventPlan event;          // what the quest needs
        bool eventFresh = false;       // Stay / Lead: the quest was taken just now, its event runs
        SpawnPoint eventEnd;           // Lead: where the NPC to bring it to stands
        uint8 eventTalks = 0;          // Talk: talks so far
        int32 eventCount = -1;         // Talk: the objective's count when the task began, or at its last credit (-1: not read yet)
        uint8 eventCredits = 0;        // Talk: credits that came in this task (a credit per partner)
        ObjectGuid eventPartner;       // Talk: whom the bot talked to last
        bool eventEntered = false;     // Enter: the bot has walked into the area trigger
        uint32 eventWaitUntil = 0;     // Talk: getMSTime() until which the bot waits for the credit of its last talk
        uint32 eventSeenAt = 0;        // Lead: getMSTime() the follower was last in sight
        uint32 leadWaitSince = 0;      // Lead: getMSTime() since the bot waits for the follower to catch up
        // Escort
        uint32 escortEntry = 0;        // the NPC to stay with
        bool escortMoved = false;      // the NPC has left its spot (the escort runs)
        uint32 escortSeenAt = 0;       // getMSTime() the NPC was last in sight

        // Interact (docs/orders.md "Quest interactions")
        InteractionPlan plan;
        uint32 actedAt = 0;            // getMSTime() of the last act (its result is checked a few seconds later)
        uint32 lastHave = 0;           // objective progress before the last act
        uint8 misses = 0;              // acts in a row without progress
        uint8 acts = 0;
        uint8 spellIndex = 0;          // CastSpells: the next of `spells` to try on the current target
        std::vector<uint32> spells;
        ObjectGuid actTarget;          // the creature or game object acted on last
        std::vector<ObjectGuid> usedTargets; // acted on without progress, or done (not chosen again by this task)
        // A follower's aid (AidFollower, any task that leads an NPC)
        uint32 aidAt = 0;              // getMSTime() of the last use of the aid or the last opening of its chest
        uint32 aidNeedSince = 0;       // getMSTime() since the follower needs the aid, 0 = it does not
        uint8 aidUses = 0;             // uses since the follower last recovered
        uint8 aidFetches = 0;          // openings of the aid's chest
        bool leading = false;          // Interact: NewRpg walks at the spot (Travel profile): Provoke's lead, a target far above or below
        ObjectGuid approachTarget;     // Interact: the creature or object the last yards go to ...
        uint32 approachSince = 0;      // ... since getMSTime(), 0 = none

        // Voyage
        TravelLink link;
        VoyagePhase voyage = VoyagePhase::ToDock;
        uint32 phaseSince = 0;         // getMSTime()
        ObjectGuid ship;
    };

    struct BotRuntime
    {
        ObjectGuid guid;
        BotTask task;
        uint32 targetZone = 0;
        uint32 zoneChosenAt = 0;       // getMSTime() of the last AUTO zone choice
        uint32 reportedOutleveled = 0; // zone whose "out-levelled" event was recorded
        uint32 lastZone = 0;           // zone the bot was in at the last tick
        uint32 arrivedZone = 0;        // target zone of the last ZONE_CHANGE event (one event per arrival, not per return)
        bool hasLastPosition = false;  // position at the last tick (jump detection)
        uint32 lastMap = 0;
        float lastX = 0.0f;
        float lastY = 0.0f;
        uint32 jumps = 0;              // jumps (teleports) the planner did not cause, since login
        uint8 lastLevel = 0;
        uint8 trainCheckedLevel = 0;   // level at which the class trainer was last checked
        uint32 nextUpkeepCheck = 0;    // getMSTime()
        uint32 nextMemberTrip = 0;     // getMSTime(): a vendor trip for a squad member's full bags not before then
        ObjectGuid vendorVisit;        // a squad member: the vendor its leader has just visited, its own visit there
        ObjectGuid vendorVisitLeader;  // ... and the leader that went there
        uint32 vendorVisitSince = 0;   // getMSTime()
        uint32 vendorVisitFree = 0;    // its free bag slots when the visit began
        uint32 memberTripFree = 0xFFFFFFFF; // its members' fewest free slots when the trip for them began
        uint32 nextRestock = 0;        // getMSTime(): no new trip for food and drink before
        uint32 drinkTripUntil = 0;     // getMSTime(): on its way to a vendor because it has nothing to drink (no rest for mana on the way)
        uint32 nextGearTrip = 0;       // getMSTime(): no new look for a vendor near the bot that sells it an upgrade before
        uint32 nextRecipeTrip = 0;     // getMSTime(): no new look for a vendor near the bot that sells it a recipe before
        uint32 nextGearWarning = 0;    // getMSTime(): when its gear is next compared with its level (0: not yet scheduled)
        uint32 nextNearbyRestock = 0;  // getMSTime(): no new detour to a food vendor close by before
        uint32 nextClassAbilities = 0; // getMSTime(): when the bot's class-quest abilities are next looked at
        uint32 nextPetTalents = 0;     // getMSTime(): when a hunter's pet's free talent points are next looked at
        uint32 nextRuneforge = 0;      // getMSTime(): when a death knight's weapons are next looked at for a rune
        uint32 nextPoisonTopUp = 0;    // getMSTime(): no new visit to a poison vendor for a rogue's poisons before
        uint32 poisonTripSince = 0;    // getMSTime(): a trip to a poison vendor begun and not yet at the vendor (0: none)
        SpawnPoint poisonVendor;       // ... that vendor
        uint32 nextPoisonLook = 0;     // getMSTime(): a bare rogue's next look for a poison vendor in the town it stands in
        SpawnPoint poisonLookAt;       // ... where it looked last (a look again once it has moved)
        uint32 nextSpec = 0;           // getMSTime(): when its talent points are next looked at (SpecService)
        uint32 nextSupply = 0;         // getMSTime(): when the bot next looks into the guild storage for supplies
        uint32 nextRepair = 0;         // getMSTime(): no new repair trip before (the last one could not pay)
        uint32 nextSell = 0;           // getMSTime(): no new selling trip before (the last one freed no space)
        uint32 nextGroupCheck = 0;     // getMSTime()
        uint32 waitUntil = 0;          // getMSTime(): no new plan before (after a failure)
        uint32 nextFlightCheck = 0;    // getMSTime(): when a walking bot next looks for a flight
        uint32 nextJourney = 0;        // getMSTime(): when a refused walk may next become a journey (StartJourney)
        uint32 nextDiscoverCheck = 0;  // getMSTime(): when the bot next looks for an unknown flight master nearby
        // At the server's level cap with no quest left the bot waits at an inn; the zones are looked at again then.
        uint32 capIdleUntil = 0;       // getMSTime()
        // A flight the planner booked (TryFlight): the flight master it leaves from, to notice the landing. With
        // instant flight paths the character is never "in flight" and NewRpg would walk it back to that flight master.
        bool flightBooked = false;
        bool flightAtMaster = false;   // the bot has stood at the flight master since (the flight starts from there)
        uint32 flightBookedAt = 0;     // getMSTime(): when the flight was booked (the core's instant teleport takes a moment)
        uint32 flightMap = 0;
        float flightX = 0.0f;
        float flightY = 0.0f;
        // Errands (OrderErrands.cpp, docs/orders.md "Errands"): a completed quest whose ender is outside the target zone.
        uint32 errandQuest = 0;        // the quest the bot is on its way to hand in, 0 = none
        SpawnPoint errandSpot;         // its ender
        std::string errandName;        // "<NPC> in <zone>" for why lines
        uint32 nextErrandCheck = 0;    // getMSTime()
        uint8 errandStuck = 0;         // stuck travels and deaths on the way of this errand
        // Stranded: where and since when the bot has not moved although its task wants it to walk.
        float strandX = 0.0f, strandY = 0.0f;
        uint32 strandSince = 0;        // getMSTime()
        uint32 fightHopAt = 0;         // OrderService::HopDownToFight: when it was last put beside an enemy it could not walk to,
        uint32 fightHopLook = 0;       // its last look, and since when it stands so
        uint32 fightHopSince = 0;
        uint32 interactQuest = 0;      // an interaction that stands still (CheckStranded): its quest, how far the
        uint32 interactMark = 0;       // quest was when it last got anywhere, and when that was
        uint32 interactMarkAt = 0;
        uint32 underGroundSince = 0;   // getMSTime() the bot was first seen under the ground, 0 = it is not
        uint32 nextUnderGroundCheck = 0;
        uint32 nextOffMeshCheck = 0;   // getMSTime(): when a bot that stands is next checked for standing off the navmesh
        // Air (CheckBreath): since when the bot is under water, and its trip to the surface.
        uint32 diveSince = 0;          // getMSTime(), 0 = not under water (CheckBreath)
        // The breath it has used, as the game counts it: one second a second under water, back ten seconds a second
        // above it (Player::UpdateMirrorTimers), where the dive clock above starts anew at any breath at the surface.
        uint32 breathUsedMs = 0;
        uint32 breathAt = 0;           // getMSTime() of the last count
        uint32 airUntil = 0;           // getMSTime() until which the bot may take to reach the surface, 0 = not on its way
        uint32 airReachedAt = 0;       // getMSTime() it reached the surface
        float airLastZ = 0.0f;         // its height at the last look on the way up (it has to rise)
        bool resumeAfterAir = false;   // it just came back from the surface: its objective task goes on
        uint32 lastRescueAt = 0;       // getMSTime() of the last rescue (the next one near it within half an hour is the hearthstone)
        uint32 lastRescueMap = 0;      // ... and where the bot stood then
        uint32 strayLiftSince = 0;     // getMSTime(): a passenger of a lift without a ride of its own since then (StepStrayLiftPassenger)
        uint32 strayLiftStillSince = 0; // .. its platform has stood at one height since then (at a stop)
        float strayLiftZ = 0.0f;       // .. the platform's height at the last look
        ObjectGuid letGoOf;            // the last creature its AI was made to let go of (no way to it on the ground)
        ObjectGuid reachLooked;        // the last target the check before a pull found a way on the ground to (CheckPull)
        uint32 letGoAt = 0;            // getMSTime() of that
        uint32 runDeathUnfoughtAt = 0; // getMSTime() it died on a run from a creature it had not fought (a keep-away it was
                                       // caught on, a strong creature near): the fight teaches nothing (Escape::OnFightEnd)
        float lastRescueX = 0.0f, lastRescueY = 0.0f;
        uint32 roomExitAt = 0;         // getMSTime() when it last left a room through its teleporter (LeaveLocalRoom)
        // First nodes of routes the bot did not get to for a pack or a creature that stood between it and the node
        // (a road map node inside a camp): the second time within the time the node is no start of its routes.
        struct RefusedStart
        {
            float x = 0.0f, y = 0.0f;
            uint32 until = 0;
            uint8 count = 0;
        };
        std::vector<RefusedStart> refusedStarts;
        // True when this made the node no start of its routes.
        bool NoteRefusedStart(float x, float y, uint32 now);
        [[nodiscard]] bool IsRefusedStart(float x, float y, uint32 now) const;
        uint8 groundStrikes = 0;       // rounds of cancelled movements the planner had no answer for
        uint32 groundStrikeAt = 0;     // getMSTime() of the last one
        std::unordered_map<uint32, uint32> errandSkip; // quest -> getMSTime() until which its hand-in is not tried again
        // Quest events (OrderEvents.cpp): the quests in the log whose event starts when they are taken (stay, lead).
        struct EventQuest
        {
            uint32 takenAt = 0;        // getMSTime() it appeared in the log; 0: it was there when the bot entered the world
            bool announced = false;    // the running task was ended for it once
        };
        std::unordered_map<uint32, EventQuest> eventQuests;
        bool eventLogKnown = false;    // the quest log was looked at once (what is in it then is old)
        std::unordered_map<uint32, uint8> eventRetakes; // quest -> times it was given back to start its event afresh
        // True while the event of a quest taken a moment ago should still be running.
        [[nodiscard]] bool IsEventFresh(uint32 questId) const;
        std::string why;
        std::string whyBeforeAside;    // the why line before a keep-away or a walk around a pack (back when the task goes on)
        bool dead = false;             // dead or a ghost at the last tick
        bool seenAlive = false;        // alive at some tick since it entered the world
        // An objective task NewRpg leaves at once, again and again: which quest, how often, when last.
        uint32 leftQuest = 0;
        uint8 leftCount = 0;
        bool gateRecheck = false;      // a walk was refused as too dangerous: AUTO looks at the zones again before it grinds
        // A quest whose tasks keep ending for what stands in their way (a pack, a pull the bot cannot afford, a fight it
        // left) without any progress: counted per quest; the second in a row leaves the quest alone for a while.
        struct QuestMiss
        {
            uint8 count = 0;
            uint32 at = 0;             // getMSTime() of the first of them
        };
        std::unordered_map<uint32, QuestMiss> questMisses;
        uint32 missQuest = 0;          // the quest of the task that last ended that way (PlanQuests looks at it), 0 = none
        uint32 missMark = 0;           // how far that quest was when the task began
        uint32 taskQuestMark = 0;      // how far the running task's quest was when the task began
        // Travels begun again and again without getting nearer (a pack or a fight on the way each time).
        uint8 errandTravelStarts = 0;
        uint32 errandTravelAt = 0;
        float errandTravelLeft = 0.0f;
        uint8 zoneTravelStarts = 0;
        uint32 zoneTravelAt = 0;
        float zoneTravelLeft = 0.0f;
        float zoneTravelFromX = 0.0f, zoneTravelFromY = 0.0f; // where the first of these starts was
        uint32 zoneTravelZone = 0;
        // AUTO with nothing to do in its zone although the zone still counts quests: the zone counts as empty for a
        // while, so the next pick looks elsewhere.
        uint8 idleHolds = 0;           // holds in a row ("Nothing left to do")
        uint32 zoneDeathZone = 0;      // deaths in one zone within a quarter of an hour (OnDied)
        uint32 zoneDeathAt = 0;
        uint8 zoneDeaths = 0;
        uint32 emptyZone = 0;
        uint32 emptyZoneUntil = 0;     // getMSTime()
        bool emptyZoneDeaths = false;  // the zone counts as empty because the bot died in it twice (no grinding there)
        // Zones the bot stood in and had nothing to do in (zone -> getMSTime() until, the bot's level then): their count
        // from afar, which sent it there, does not send it there again for a while (ChooseZone; kept across a restart).
        mutable std::unordered_map<uint32, std::pair<uint32, uint8>> afarEmpty;
        uint32 nextStruckNote = 0;     // getMSTime(): the next log line "does not grind in a zone that killed it twice"
        // Zones the bot kept dying in (two deaths within a quarter of an hour, OnDied): how often, and at which level.
        // Each time the zone rests twice as long; a level-up starts the count anew.
        std::unordered_map<uint32, std::pair<uint8, uint8>> zoneStrikes;
        uint32 lastCrossingAt = 0;     // getMSTime() when the bot last went ashore from a ship or zeppelin (0 = never)
        // A bot that has gone ashore stays on that side of the sea for an hour, whatever the other side seems to offer.
        static constexpr uint32 SeaCrossRestMs = 60 * 60 * 1000;
        [[nodiscard]] bool CrossedLately() const { return lastCrossingAt && getMSTimeDiff(lastCrossingAt, getMSTime()) < SeaCrossRestMs; }
        uint32 errandCrossing = 0;     // the quest of the errand that crossing was for (its second leg may follow), else 0
        // The way to a ship was refused from where the bot stood: what lies across the sea waits while it is near there
        // (every zone over there leads over the same pier).
        uint32 noZoneSince = 0;        // getMSTime() since the autopilot has no zone it can get to (0: it has one)
        uint32 placeHeldAt = 0;        // getMSTime() of the last "the place holds it" (its zone travels end where they begin)
        uint8 placeHeldTimes = 0;      // ... and how often within a quarter of an hour (the hearthstone only the second time)
        uint32 areaIdleQuest = 0;      // an objective at a quest area: the quest, its mark, and since when the mark stands
        uint32 areaIdleMark = 0;
        uint32 areaIdleSince = 0;
        uint32 seaClosedUntil = 0;     // getMSTime(); 0 = open
        uint32 seaClosedMap = 0;
        float seaClosedX = 0.0f, seaClosedY = 0.0f;
        static constexpr uint32 SeaClosedMs = 15 * 60 * 1000;
        static constexpr float SeaClosedYards = 400.0f;
        void CloseSea(uint32 now, uint32 mapId, float x, float y)
        {
            seaClosedUntil = now + SeaClosedMs;
            seaClosedMap = mapId;
            seaClosedX = x;
            seaClosedY = y;
        }
        [[nodiscard]] bool SeaClosed(uint32 now, uint32 mapId, float x, float y) const
        {
            return seaClosedUntil && now < seaClosedUntil && mapId == seaClosedMap &&
                (x - seaClosedX) * (x - seaClosedX) + (y - seaClosedY) * (y - seaClosedY) < SeaClosedYards * SeaClosedYards;
        }
        // What of this is in ga_bot_wait (SaveBotWaits): a sum over the waits, and when it is looked at next.
        uint64 waitsSaved = 0;
        uint32 nextWaitsSave = 0;
        uint32 handInAt = 0;           // getMSTime() of the last quest hand-in (vendor trips at a hub follow it)
        uint8 hubBatches = 0;          // batches crafted for its professions since that hand-in
        uint32 nextProfessionCheck = 0; // getMSTime() of the next look at what the profession plan has to learn
        uint32 nextStaleQuestCare = 0;  // getMSTime() of the next look at the quest log for grey and failed quests
        uint32 nextSupplyCheck = 0;    // getMSTime() of the next look for quest items the guild can provide
        uint32 nextCraftCheck = 0;     // getMSTime() before which no crafting for skill is tried again
        uint32 lastCraftSkill = 0;     // the profession of the last batch crafted for skill: the other one is next
        uint32 professionFocus = 0;    // profession mode: the profession far behind the others it works on alone
        // Ways refused where it stands (the walk gate, a route, a dock): counted at a place, for the way home.
        uint8 refusalsPending = 0;     // refusals since the last look (ClearTask counts them, it has no position)
        uint8 boxedRefusals = 0;       // refusals within BoxedYards of (boxedX, boxedY) since boxedSince
        uint32 boxedSince = 0;         // getMSTime() of the first of them
        uint32 boxedMap = 0;
        float boxedX = 0.0f, boxedY = 0.0f;
        uint32 nextBoxedNote = 0;      // getMSTime(): the next line about it while the hearthstone is not ready
        uint32 nothingSafeAt = 0;      // getMSTime(): it last found nothing in its zone it could safely get to (RunAdventure)
        uint32 nothingSafeZone = 0;    // ... in which zone
        uint32 nothingSafeMap = 0;     // ... and where it stood then
        float nothingSafeX = 0.0f;
        float nothingSafeY = 0.0f;
        uint32 nothingSafeHearthAt = 0; // getMSTime(): the second such look; the hearthstone if it is still there 3 min later
        uint32 lastFightEndAt = 0;     // getMSTime(): when its last fight ended (a bot that grinds has work: no hearthstone for it)
        uint32 professionFocusPausedUntil = 0; // getMSTime(): ... and until when not, because it could do nothing for it
        uint8 craftPurchases = 0;      // material purchases planned since the last hand-in (the hub's crafting)
        uint32 nextGearCraft = 0;      // getMSTime() before which no gear is crafted for the guild again
        std::string whyBeforeGather;   // the task's why line, shown again after a gather detour
        // The fatigue bar as the game counts it (Player::HandleDrowning): one second a second in dark water, back ten
        // seconds a second out of it, a minute before the damage begins (TrackBreath; CheckStranded rescues at 50 s).
        uint32 fatigueUsedMs = 0;
        uint32 stoneWindowAt = 0;      // getMSTime() the count of stepping stones began at
        uint8 stonesInWindow = 0;      // stepping stones taken since then (a few per ten minutes)
        uint32 gearCraftItem = 0;      // the piece of gear its crafting queue is making (0 = none) ...
        ObjectGuid gearCraftFor;       // ... and the guild character it is meant for
        bool gearCraftRole = false;    // ... for that character's dungeon set (DungeonRoles.h), not to level in
        uint32 gearCraftHad = 0;       // ... and how many of it the bot and the guild storage held before
        uint8 hubGearSteps = 0;        // steps towards gear for the guild since the last hand-in (materials made, purchases)
        uint8 hubFocusTrips = 0;       // walks to an anvil or forge next to the hub since the last hand-in
        std::unordered_set<uint32> gearFailed; // pieces whose cast failed twice: not chosen again (until the server restarts)
        std::unordered_set<uint32> gearFailedOnce; // ... and those whose cast failed once
        ObjectGuid gatherNode;         // the node (or the corpse to skin) a gather detour walks to
        ObjectGuid fishWeapon;         // the weapon the fishing pole replaced in the bot's hand (put back after fishing)
        uint32 nextShoreLook = 0;      // getMSTime(): when the water next to the bot is next looked for (profession mode)
        uint32 nextWaterLook = 0;      // getMSTime(): when far water for its Fishing is next looked for
        uint32 nextHuntLook = 0;       // getMSTime(): when a hunting ground for a material is next looked for
        uint32 professionIdleSince = 0; // getMSTime() since the profession mode has found nothing to craft or gather
        // AUTO: a gathering skill the zone asks more of than the bot has is caught up before it quests on
        // (OrderService::CatchUpGathering). `skill` 0: not catching up.
        struct CatchUp
        {
            uint32 skill = 0;       // SKILL_MINING, SKILL_HERBALISM or SKILL_SKINNING
            uint32 target = 0;      // the skill value the zone's lowest node (or creature) asks for
            uint32 zone = 0;        // the zone it quests in
            uint32 since = 0;       // getMSTime() when the catch-up began
            uint32 startValue = 0;  // the skill then
            uint32 lastValue = 0;   // the skill at its last gain ...
            uint32 lastGainAt = 0;  // ... and when that was
            bool idle = false;      // the last look found nothing to gather
            uint32 idleSince = 0;   // getMSTime() since that is so
        } catchUp;
        uint32 nextCatchUpLook = 0;    // getMSTime(): when the zone's nodes are next held against its gathering skills
        uint32 nextSmeltLook = 0;      // getMSTime(): when a miner catching up next looks at the ore it could smelt
        uint8 catchUpFails = 0;        // catch-ups given up one after the other: the next look comes later each time
        uint32 nextSurplusLook = 0;    // getMSTime(): when an enchanter next looks for surplus crafted gear to disenchant
        uint32 nextCookLook = 0;       // getMSTime(): when a cook next looks for food to make for the guild
        uint32 nextAmmoLook = 0;       // getMSTime(): when an engineer next looks for ammunition to make for the guild
        uint32 nextConsumableLook = 0; // getMSTime(): when a crafter next looks for endgame consumables to make for the guild
        uint32 nextScrollLook = 0;     // getMSTime(): when an enchanter next looks for a scroll to write for the guild's gear
        uint32 nextScrollUse = 0;      // getMSTime(): when it next looks at the guild storage's scrolls for its own gear
        uint32 nextProcessLook = 0;    // getMSTime(): when a crafter next looks for raw materials to process for the guild
        std::unordered_map<uint32, uint32> consumableSkip; // consumable -> getMSTime() until which it is not made (casts failed)
        uint32 consumableItem = 0;     // the consumable its last batch for the guild was, and how many there were then
        uint32 consumableHad = 0;
        uint32 capConsumable = 0;      // the consumable the look at the level cap sent it into the profession mode for,
        uint32 capConsumableHad = 0;   // and how many the guild storage held then (none more at the hand-back: left out)
        uint8 consumableStrikes = 0;   // batches of it in a row after which there were no more of it
        uint32 nextToolLook = 0;       // getMSTime(): its next look at a tool its recipes lack that another profession makes
        uint32 nextCraftSquadLook = 0; // getMSTime(): its next look whether a squad keeps it from its profession mode
        uint32 nextRetreat = 0;        // getMSTime(): no walk out of a place too dangerous to stand in before (PlanRetreat)
        uint32 lootHoldSince = 0;      // getMSTime() since playerbots has something to loot next to the bot (it stays off its mount)
        bool lootWaits = false;        // ... and whether it has, as of this tick
        ObjectGuid lootHoldGuid;       // ... what the last hold was for, and when it ended without the loot
        uint32 lootHoldEndedAt = 0;
        std::unordered_map<ObjectGuid, uint32> lootIgnored; // loot beside the way it held for twice in vain: until when it is left alone
        uint32 nextQuietWalk = 0;      // getMSTime(): no walk to a quiet place to craft at before (PlanQuietPlace)
        uint32 nextUnquietNote = 0;    // getMSTime(): the next log line "does not craft here"
        uint32 unquietHostiles = 0;    // profession mode: what stood around at the last look that did not craft (0: quiet)
        uint8 gatherRefusals = 0;      // ways to places to gather at that the walk gate refused, one after the other
        uint32 gatherPausedUntil = 0;  // getMSTime(): no gathering way is planned before (three refusals in a row)
        uint32 nextHubFishing = 0;     // getMSTime() before which no casts at a hub's water are planned again (AUTO)
        ObjectGuid gatherSkipped;      // a node a detour did not get (not tried again)
        uint32 gatherUntil = 0;        // getMSTime() the detour ends at the latest
        uint32 gatherAssert = 0;       // getMSTime() of the last walk command of the detour
        uint32 nextGatherCheck = 0;    // getMSTime() of the next look for a node
        uint32 nextAmmoTrip = 0;       // getMSTime() before which no vendor trip is made for ammunition again
        uint32 nextAmmoTopUp = 0;      // ... and no visit of a vendor in reach to top its ammunition up
        uint32 leftAt = 0;             // getMSTime()
        std::string whyBeforeDeath;    // the why line shown again once the bot is alive
        // Dying again right where the corpse lies (the mobs that killed the bot are still there) makes the ghost take the
        // spirit healer's resurrection instead of walking back into them.
        uint32 lastDeathAt = 0;        // getMSTime()
        uint32 lastDeathMap = 0;
        float lastDeathX = 0.0f;
        float lastDeathY = 0.0f;
        bool spiritRes = false;

        // Deaths (no wowbot335 counterpart): a quest objective or grind spot the bot keeps dying at is set aside until
        // the bot gains a level or the time passes; a quest set aside twice counts as a failure (A21).
        struct DeathTally
        {
            uint8 count = 0;
            uint32 firstAt = 0;        // getMSTime() of the first death in the window
        };
        struct Danger
        {
            uint8 level = 0;           // the bot's level when it was set aside
            uint32 until = 0;          // getMSTime()
            uint8 strikes = 0;         // how often the quest was set aside since its last failure
        };
        std::unordered_map<uint32, DeathTally> questDeaths;
        std::unordered_map<uint32, Danger> dangerousQuests;
        // Objectives of a quest whose way was "too far around", one after the other (OrderService::StepTask): how many
        // and when the last was. The next spawn is chosen at once, and the targets beyond one cliff are all of them.
        struct FarAround
        {
            uint8 count = 0;
            uint32 at = 0;             // getMSTime()
        };
        std::unordered_map<uint32, FarAround> farAroundQuests;
        [[nodiscard]] bool IsDangerous(uint32 questId, uint8 level) const;

        // Areas the planner keeps away from (grind spots and objective spots): where the bot kept dying (until it gains
        // a level or the time passes) or that it could not get to (until the time passes).
        struct AvoidArea
        {
            uint32 mapId = 0;
            float x = 0.0f;
            float y = 0.0f;
            uint8 level = 0;           // set aside while the bot is at most this level (255: any level)
            uint32 until = 0;          // getMSTime()
            bool deaths = false;       // the bot kept dying there (else it could not get there)
        };
        std::vector<AvoidArea> avoidAreas;
        [[nodiscard]] bool IsInAvoidedArea(uint32 mapId, float x, float y, uint8 level) const;
        // True if the straight way between two points leads into a place that is set aside after deaths or a lost fight
        // (not one the bot merely could not get to), deeper than where the way starts.
        [[nodiscard]] bool CrossesAvoidedArea(uint32 mapId, float fromX, float fromY, float toX, float toY, uint8 level) const;
        void AddAvoidArea(uint32 mapId, float x, float y, uint8 level, uint32 durationMs, bool deaths);

        // Travel that keeps getting stuck on the way to the same zone: the zone is set aside (AUTO picks another one,
        // LEVEL_ZONE waits).
        uint32 travelStuckZone = 0;
        uint8 travelStuckCount = 0;
        std::unordered_map<uint32, uint32> unreachableZones; // zone -> getMSTime() until which it is set aside
        [[nodiscard]] bool IsZoneUnreachable(uint32 zoneId) const;

        // NPCs and spots the bot could not deal with (unreachable, not there): skipped until the time (getMSTime()).
        std::unordered_map<uint64, uint32> avoidSpawns;     // (spawnId << 1) | game object
        [[nodiscard]] bool IsAvoided(SpawnPoint const& spawn) const;
        void Avoid(SpawnPoint const& spawn, uint32 durationMs);
        // Targets on a level neither the bot nor the road map has a way to (OrderService::SetAsideOtherLevel): set aside
        // for hours and kept across a restart (ga_bot_wait kinds 5 to 7: a creature, an object, an area).
        struct LastingAvoid
        {
            uint8 kind = 0;
            uint32 id = 0;
            uint32 until = 0;          // getMSTime()
        };
        std::vector<LastingAvoid> lastingAvoids;

        // Stuck tracker (wowbot335 A20): time is only counted while the bot is not fighting or dead. Two rules: no
        // 10-yard move in two minutes, or (new) no 5 yards closer to the target in three minutes, which also catches a
        // bot that walks back and forth in front of an obstacle.
        float stuckX = 0.0f;
        float stuckY = 0.0f;
        uint32 stuckSince = 0;
        float approachBest = 0.0f;     // nearest distance to the target so far
        uint32 approachKey = 0;        // which measure approachBest is in: 0 the straight line, else the route's
        uint32 approachSince = 0;      // getMSTime() when the bot last got 5 yd closer

        // Rolling rates: (unix time, total xp, money) once a minute.
        struct Sample
        {
            uint32 time = 0;
            uint64 xp = 0;
            uint32 money = 0;
        };
        std::deque<Sample> samples;
        uint32 nextSample = 0;

        // Activity log (logger "module.ga.activity", docs/orders.md "Activity log").
        std::string name;              // the bot's name, for log lines written where no Player is at hand
        uint32 since = 0;              // getMSTime() when the planner first saw the bot in the world (login)
        uint32 nextStatus = 0;         // getMSTime() of the next status line
        bool hasStatusPosition = false;
        uint32 statusMap = 0;          // position at the last status line (how far the bot moved since)
        float statusX = 0.0f;
        float statusY = 0.0f;
        uint32 lastFoeEntry = 0;       // creature the bot last fought (named on its death line)
        uint8 lastFoeLevel = 0;
        uint32 lastFoeAt = 0;          // getMSTime(): when it last fought that creature
        uint32 nextAvoidAoeAssert = 0; // getMSTime(): the next look that mod-playerbots' "avoid aoe" strategy is on
        uint32 groundEffectSince = 0;  // getMSTime(): since when it stands in a hostile ground effect (0: none)
        uint32 groundEffectLogAt = 0;  // getMSTime(): its last activity line about one
        // Passing a hostile ground effect on its way (OrderService::StepGroundEffectAhead): around it, at its edge until it
        // is gone, or through it quickly.
        ObjectGuid groundPassEffect;   // the effect its last decision was about (one activity line per effect and decision)
        uint8 groundPassMode = 0;      // 0 none, 1 around, 2 waits at its edge, 3 through it
        uint32 groundPassUntil = 0;    // getMSTime(): that decision holds until then
        uint32 groundPassLookAt = 0;   // getMSTime(): its next look ahead
        uint8 groundPassLogged = 0;    // the decision about groundPassEffect its last activity line named
        // What OrderService::TrackActivity looked at last: where the bot stood, the start and heading of the stretch of
        // its way it walks, the creature it fought.
        bool hasActivity = false;
        uint32 activityMap = 0;
        uint32 activityAt = 0;         // getMSTime()
        float activityX = 0.0f;
        float activityY = 0.0f;
        float legX = 0.0f;
        float legY = 0.0f;
        float legHeading = 0.0f;
        bool hasLegHeading = false;
        ObjectGuid lastVictim;
        // The creature a quest's event brought that the bot last turned to (OrderService::DefendWard): one line per creature.
        ObjectGuid wardFoeAnnounced;

        // The planner's clock (docs/performance.md PF-1): when it last stepped this character, and the time since the
        // step before (about a second; longer when the planner stretches its period under load). The clocks that stand
        // still while the bot fights or is dead move on by stepGapMs, not by a fixed second.
        uint64 lastStepAt = 0;         // Perf::NowMs()
        uint32 stepGapMs = 1000;
        uint64 fastDue = 0;            // Perf::NowMs(): its next turn in the four-times-a-second loop (0: not phased yet)

        // Rest between fights (docs/orders.md "Rest and provisions"): below the start values the bot stops, eats and
        // drinks and goes on at the end values. The task and its clocks wait meanwhile.
        struct Rest
        {
            bool active = false;
            uint32 startedAt = 0;      // getMSTime()
            uint32 nextUse = 0;        // getMSTime(): no food or drink before (the last one is still being taken)
            uint8 healthStart = 0;     // percent
            uint8 manaStart = 0;
            bool usesMana = false;
            std::string ate;           // name of the food taken ("" = none)
            std::string drank;
            std::string bandage;       // name of the bandage it used on itself ("" = none; one try per rest)
            std::string conjured;      // what a mage conjured in this rest ("water", "water and food"; "" = nothing)
            bool conjuring = false;    // a mage that began to conjure fills its stock up to twice Provisions.ConjureBelow
            bool onlyConjure = false;  // begun only to conjure (nothing else was low)
            std::string whyBefore;     // the task's why line, shown again after the rest
            bool placeChecked = false; // looked for a quieter place once
            uint32 walkUntil = 0;      // getMSTime(): walking to a quieter place until then at the latest (0 = not walking)
            float walkX = 0.0f;
            float walkY = 0.0f;
            float walkZ = 0.0f;
            bool walkFinal = false;    // walked on to the exact place once: it stopped within 4 yd of it, still in the water
            bool forAir = false;       // begun because the bot was under water too long, not for its health or mana
            bool forHealth = false;    // its health was below the leader's threshold at some point of this rest: it eats
            bool forMana = false;      // ... its mana: it drinks
            bool forWalk = false;      // begun before a walk through hostile ground: it rests to WalkWant
            std::string walkNote;      // for the log: ", before a walk past level-N creatures"
            uint8 relocations = 0;     // moved on because something hostile came near
            uint32 nextWatch = 0;      // getMSTime(): next look for something hostile coming near
            std::string place;         // for the log: "walked N yd away from M hostile creatures" ("" = rested where it stood)
        };
        Rest rest;
        // A pull called off because the bot is not rested enough for it (FightEscape CheckPull): it rests until it
        // has this share of its full strength, the next minutes.
        uint32 nextSelfHeal = 0;       // getMSTime(): no heal on itself before (the last one is being cast)
        uint32 nextSelfBuff = 0;       // getMSTime(): the next look at what it should always have on (StepSelfBuff)
        uint32 nextTotemLook = 0;      // getMSTime(): the next look at a shaman's totem strategies (StepSelfBuff)
        uint32 nextPotion = 0;         // getMSTime(): the next look for a potion in a fight (StepPotion)
        uint32 nextConjure = 0;        // getMSTime(): a mage does not conjure before (a cast failed, something hostile was near)
        uint32 autoShotIdleSince = 0;  // getMSTime(): its Auto Shot runs out of any fight since (StopLeftoverAutoShot)
        // The walk to the task's target leads past hostile creatures of about the bot's level (looked at every few
        // seconds, StepRest): it sets out rested. Once per target.
        uint32 nextWalkLook = 0;       // getMSTime()
        uint8 walkDanger = 0;          // the highest level of hostile creatures along the way (0 = none that matter)
        uint64 walkRestedFor = 0;      // the target it has rested for already
        uint32 nextStepBack = 0;       // getMSTime(): no look for a step back from a rooted foe before
        float restWant = 0.0f;
        uint32 restWantUntil = 0;      // getMSTime()
        // The gate of a dungeon group before a boss or a big pull (OrderService::DungeonGate, package P1) asks the member to
        // rest up to this health and mana (0: not asked), for a few seconds at a time while it waits.
        uint8 gateHealth = 0;
        uint8 gateMana = 0;
        uint32 gateUntil = 0;          // getMSTime()
        // It entered the world with hostile creatures around it (it logged out in a camp): before anything else it
        // walks to a quiet place, as a rest does.
        bool loginRetreat = false;
        // ... and for the seconds creatures do not see it after such a login no task leads it anywhere (UpdateBot).
        uint32 loginGraceUntil = 0;    // getMSTime()
        // Out of a camp first (FightEscape.h, Escape::Step): the next look, and the task it has looked at already.
        uint32 nextCampLeave = 0;      // getMSTime()
        uint64 campLeaveTask = 0;
        // The last yards to a place where hostile creatures stand are walked (Escape::WalksLastYards).
        bool onFoot = false;
        uint64 onFootTask = 0;         // the task target `onFootHostile` was looked up for
        bool onFootHostile = false;    // creatures that are no friends of the bot have their spawns around that target
        ObjectGuid pullNoteTarget;     // the target of the last "does not pull" line, and when (no line per second)
        uint32 pullNoteAt = 0;
        // Where it let go of a pull: until it has walked away from there (or the time is over) it pulls nothing
        // by itself (playerbots would pick the same target again).
        bool pullCheckOn = false;      // set by the planner each tick: the bot chooses its own fights (not a follower)
        ObjectGuid pullOkTarget;       // the last target the pull check let pass, when, and how many foes it expected
        uint32 pullOkAt = 0;           // getMSTime()
        uint32 pullOkSince = 0;        // getMSTime(): since when the pull check lets that target pass (the bot does not get to it?)
        uint8 pullOkFoes = 0;
        uint8 pullOkMaybe = 0;         // ... and how many more it thought possible (creatures that walk about near it)
        ObjectGuid petLeadTarget;      // the target its pet was sent at before the bot's own first cast (once per target)
        uint8 detours = 0;             // walks around a pack since detoursSince (three in a row: the task ends instead)
        uint32 detoursSince = 0;       // getMSTime()
        float pullOkX = 0.0f;          // where that target stood
        float pullOkY = 0.0f;
        uint32 pullHoldUntil = 0;      // getMSTime()
        // The last pull called off would have been affordable with one more guild character beside the bot (CheckPull):
        // the quest it was for can wait for a quest partner (StepPullCheck).
        bool pullPartnerHelps = false;
        uint32 pullHoldMap = 0;
        float pullHoldX = 0.0f;
        float pullHoldY = 0.0f;
        // A careful pull in progress (FightEscape.h "Careful pulls"): the bot brings one creature to a place away
        // from the others before the fight. While it is active the bot is the pull's.
        struct CarefulPull
        {
            bool active = false;
            uint8 phase = 0;           // 1 to its place, 2 the opener, 3 into the creature's sight, 4 back, 5 waiting for it
            bool ranged = false;       // opens with a ranged spell or weapon (else: into the creature's sight and back)
            bool forGroup = false;     // asked for by another caller (Escape::BeginPull: a dungeon run's tank)
            ObjectGuid target;
            uint32 spell = 0;
            uint32 mapId = 0;
            uint32 startedAt = 0;      // getMSTime()
            uint32 phaseAt = 0;        // getMSTime()
            uint32 nextAct = 0;        // getMSTime(): the next cast or movement order of the phase
            float pullX = 0.0f;        // where it stands when it pulls
            float pullY = 0.0f;
            float pullZ = 0.0f;
            float fightX = 0.0f;       // where the fight shall be
            float fightY = 0.0f;
            float fightZ = 0.0f;
            float waitFrom = 0.0f;     // how far the creature was when the bot began to wait for it
            uint16 castResult = 0;     // the core's result of the last try of the opener (SpellCastResult)
            uint8 casts = 0;           // tries of the opener
            uint32 lastCastAt = 0;     // when the last try began (a debug line names what ended it)
            bool staleCleared = false; // stale movement flags were cleared before an opener (one line per pull)
            uint32 moveWaitSince = 0;  // since when an opener with a cast time waits for its creature to stand
            uint8 steps = 0;           // steps nearer after the core called the opener out of range
            uint32 inSightSince = 0;   // getMSTime(): since when the bot has stood inside the creature's sight without it coming
            uint8 lures = 0;           // a creature that stood and cast: 1 the bot stepped out of its sight, 2 out of its spells' reach
        };
        CarefulPull pull;
        ObjectGuid pullFailedTarget;   // a careful pull of this creature did not work: not tried again before ...
        uint32 pullFailedUntil = 0;    // getMSTime()
        uint32 nextPullSwitch = 0;     // getMSTime(): when it next looks for a creature of the kind that stands alone
        uint32 underWaterSince = 0;    // getMSTime(): since when the bot has its head under water (0 = it has not)
        uint32 nextDryLook = 0;        // getMSTime(): in the water with a rest due and no dry ground found: no new search before
        uint32 nextDryNote = 0;        // getMSTime(): the log line for that, not before
        uint32 restNoPlaceUntil = 0;   // getMSTime(): a rest is due and there is no place to take it (in the water): no pull before
        uint32 restNoPlaceNoteAt = 0;  // getMSTime() of the pull check's line about it

        // The fight under way, for its line in the activity log. Health and mana at its start are the last values seen
        // out of combat (the planner looks once a second).
        struct Fight
        {
            bool active = false;
            uint32 startedAt = 0;      // getMSTime()
            uint8 healthStart = 0;     // percent
            uint8 manaStart = 0;
            uint8 healthLow = 100;     // lowest health seen
            uint8 foes = 0;            // most attackers at the same time
            uint32 foeEntry = 0;       // the highest-level creature fought
            uint32 firstKind = 0;      // the kind of the first creature that came into the fight ...
            bool oneKind = true;       // ... and whether every one that came was of that kind
            uint8 foeLevel = 0;
            uint8 water = 0;           // where it fought: 1 = in water, 2 = under water (the deepest seen)
            uint8 form = 0;            // its shapeshift form when the fight began (ShapeshiftForm)
            uint8 targetHealth = 100;  // health of what it attacked last, percent
            uint8 expected = 0;        // its own pull: the foes the pull check expected (0 = the bot was attacked)
            uint8 possible = 0;        // ... and how many more it thought possible
            uint8 selfHeals = 0;       // heals it cast on itself (StepSelfHeal)
            std::string potion;        // the potion it drank (StepPotion; the game allows one per fight)
            uint8 stepsBack = 0;       // runs to range from a rooted foe (Escape::StepBack)
            bool defended = false;     // what protects the bot was cast (or it has nothing ready) for this fight
            bool wand = false;         // it shot its wand in place of a filler spell (StepWand)
            uint8 interrupts = 0;      // foes' spellcasts it interrupted (Escape::StepInterrupt)
            std::string defence;       // ... its name, for the fight line
            std::string idleNote;      // twelve seconds in and the foes have lost nothing: what the bot was doing, for the fight line
            // How the fight goes, measured (FightEscape MeasuredNeed): the foes' health when each joined and now, in
            // percent of one foe; the seconds every foe has been on the bot, added up.
            std::vector<ObjectGuid> seen;
            std::vector<std::pair<uint32, uint8>> kinds; // entry and level of each creature in `seen`, in the same order
            float unitsJoined = 0.0f;
            float unitsNow = 0.0f;
            float attackerSeconds = 0.0f;
            uint32 firstLossAt = 0;    // getMSTime() when the foes had first lost 3 points of health (0 = not yet)
            // The escape's rule that finishes a nearly dead target first (Escape::Step): the target, its lowest health
            // and when it reached it; the rule holds only while that health keeps falling.
            ObjectGuid finishGuid;
            float finishLow = 101.0f;
            uint32 finishLowAt = 0;
            bool finishLapsed = false;
            float budgetStart = 1.0f;  // what the bot had when the fight began (Escape::BudgetOf: health, mana at half weight)
            uint32 measuredAt = 0;     // getMSTime() of the last sample
            uint32 debugAt = 0;        // getMSTime() of the last detail line (GuildAdventure.Debug)
            // A fight in which nothing happens (TrackFight): since when the bot's health, the foes' health and its
            // place have been what they are; `stale`: the module ended it for that.
            uint32 quietSince = 0;
            uint8 quietHealth = 0;
            std::size_t quietSeen = 0;  // creatures that had joined the fight then
            std::vector<std::pair<ObjectGuid, float>> foeLows;  // the lowest health each foe has been at, percent
            uint32 foeLossAt = 0;      // getMSTime() when a foe last reached a new low (or the fight began)
            uint8 foeLossHealth = 0;   // the bot's highest health since then, percent
            float quietX = 0.0f;
            float quietY = 0.0f;
            bool stale = false;
        };
        Fight fight;
        // Creatures the module ended a fight with because they did not die (TrackFight: held at one health point
        // by a script, or immune): the pull check leaves them alone until the time given.
        std::unordered_map<ObjectGuid, uint32> undyingFoes;
        bool syncStrategies = false;   // a level-up: look at the combat strategies again once the fight is over
        uint8 calmHealth = 100;        // health and mana when last seen out of combat
        uint8 calmMana = 100;

        // Counters for the status line (since the last one) and for `.ga bot plan` (since the login).
        struct Tally
        {
            uint32 fights = 0;
            uint32 crowded = 0;        // fights against two or more foes at once
            uint32 weakStarts = 0;     // fights begun below half health
            uint32 rests = 0;
            uint32 restSeconds = 0;
            uint32 deaths = 0;
            uint32 escapes = 0;        // fights left in time (FightEscape.h)
            uint32 foodUsed = 0;       // pieces of food eaten in rests (and bandages)
            uint32 drinkUsed = 0;      // pieces of drink
            // How active it is (OrderService::TrackActivity; the owner, 2026-11-25: [quote left out]).
            uint32 stoodMs = 0;        // stood still out of a fight, a rest and a cast
            uint32 stoodWalkMs = 0;    // of that with a task that walks, its target more than 10 yd away, nothing waited for
            float walkedYards = 0.0f;  // yards it went on its own (teleports, flights and ships left out)
            uint32 turnBacks = 0;      // stretches of 15 yd out of a fight that turned back on the one before (> 150 degrees)
            uint32 tasksBegun = 0;     // tasks begun (holds not counted)
            uint32 tasksQuick = 0;     // of them ended within 10 s
            uint32 targetSwitches = 0; // in a fight it turned to another creature while the one before was alive
            // stoodMs by the task it had then (index: TaskKind), the last one for a wait on purpose (away from a
            // creature, for one on its way, the planner's own wait): what the status line names as "stood most while".
            static constexpr std::size_t StoodWaiting = 16;
            std::array<uint32, StoodWaiting + 1> stoodByKindMs{};
        };
        Tally window;                  // since the last status line
        Tally total;                   // since the login

        // Leaving a fight the bot would lose (FightEscape.h).
        struct Escape
        {
            bool active = false;
            uint8 leaveTicks = 0;      // ticks in a row the estimate said "leave"
            uint32 startedAt = 0;      // getMSTime()
            uint32 nextLegAt = 0;
            uint32 legs = 0;
            uint32 mapId = 0;          // where the bot left the fight
            float fromX = 0.0f;
            float fromY = 0.0f;
            float legX = 0.0f;         // the end of the current leg
            float legY = 0.0f;
            float heading = 0.0f;
            uint8 healthStart = 0;     // percent
            uint8 foes = 0;
            uint32 foeEntry = 0;
            uint8 foeLevel = 0;
            bool petWasDefensive = false;
            uint8 helpsCast = 0;       // bit per class spell that helps to get away, once it was cast
            uint8 helpsNoted = 0;      // (debug) bit per such spell whose refusal was logged
            bool vanished = false;     // it cast what drops the fight (Vanish, Feign Death): nothing on a foe after that
            bool getawayCast = false;  // a spell that makes the run cheap went out (Sprint, a nova, a fear ...)
            uint8 firstPast = 0;       // the creatures the first leg leads past (0: an open way)
            bool pack = false;         // keeping away from a pack on its way (aside), not from one strong roamer
            bool camp = false;         // ... and found itself in the middle of a camp: it goes on until it is out of it
            bool leaving = false;      // it stood in the middle of a camp with nothing to do there: out of it first, its task (if any) goes on afterwards
            bool waiting = false;      // clear of a roamer that is still on its way near by: it stands until that has moved on
            uint8 reruns = 0;          // ... and how often it had to run again while it waited
            bool detour = false;       // ... by walking around it (to legX / legY / legZ), the task goes on afterwards
            bool standing = false;     // ... one strong creature that keeps to its place (no round): the task goes on afterwards
            float legZ = 0.0f;
            uint32 clearSince = 0;     // getMSTime(): out of combat since then, but not yet clear of the place
            bool onTrail = false;      // at least one leg went back along the bot's trail
            std::string legNote;       // how the first leg was chosen, for the log (what the leg rule refused)
            std::string helps;         // the class spells it cast for the run, for the log
            std::string heldBy;        // the short stuns and roots it waited out on the run, for the log
            bool fromAside = false;    // a run that began as a keep-away the bot was caught on (looked at again on the way)
            uint32 combatSeenAt = 0;   // getMSTime(): the keep-away found the bot in combat with nothing on it yet
            uint32 nextRecheck = 0;    // getMSTime(): the next look whether the run from a keep-away should be a fight
            uint32 feignAt = 0;        // getMSTime(): a hunter's Feign Death went out on this run
            bool aside = false;        // not a fight: keeping away from a roaming creature far above the bot
            bool walkAway = false;     // ... or walking away from what held a fight the module ended (WalkAway): no roamer
            float runEstimate = 0.0f;  // what the run was expected to cost (share of full health, before runFactor)
            bool byFight = false;      // left because of how this fight went (measured), not by the learned estimate
            uint32 threatEntry = 0;    // left because this creature far above the bot came near (0: by the estimate)
            uint8 threatLevel = 0;
            uint8 fetched = 0;         // creatures a fleeing foe was about to fetch, counted as foes already
        };
        Escape escape;
        float fightCost = 0.4f;        // what a fight against one foe of its level costs it (health lost plus half the mana spent, running average)
        // The same per kind of creature, for the kinds it has fought (kept across restarts, ga_bot_foe_cost): a
        // level-17 mage's fights cost it 23 % on average, 8 % against boars and all its health against Black Dragon
        // Whelps of its level.
        struct FoeNote
        {
            float cost = 0.0f;         // against one of the bot's level
            uint16 fights = 0;
            bool told = false;         // the activity log has the line about this kind
        };
        std::unordered_map<uint32, FoeNote> foeCost; // by creature entry
        uint8 playerClass = 0;         // its class (Risk::KindRatio: what the guild's characters of its class learned of a kind)
        float fightHealthRate = 0.02f; // the share of its health one foe of its level takes off it per second of a fight (running average)
        float runFactor = 1.0f;        // what its runs really cost against the estimate (learned from its own runs; a death on the run raises it)
        uint32 fightsLearned = 0;      // fights against one foe the three values above were learned from (kept across restarts, ga_bot_fight_stats)
        // What a second foe at once costs it on top of two single fights, as a share of the cheaper foe (fights against
        // exactly two foes; Escape::OthersShareOf takes the mean of its class, ga_bot_pair_share).
        float pairShare = 0.5f;
        uint16 pairFights = 0;

        // Tasks that ended because the bot left a fight, by target: the second time within half an hour the target is
        // set aside (a druid started one turn-in 15 times in a row and left a fight on the way each time).
        struct FledTarget
        {
            uint8 count = 0;
            uint32 at = 0;             // getMSTime() of the last one
        };
        std::unordered_map<uint64, FledTarget> fledTargets;
        uint32 noEscapeUntil = 0;      // after a run that failed the bot fights on until then
        struct EscapeMark
        {
            uint32 mapId = 0;
            float x = 0.0f;
            float y = 0.0f;
            uint32 at = 0;             // getMSTime()
        };
        std::vector<EscapeMark> escapeMarks; // where the bot left fights lately: a place it leaves again is set aside longer
        struct TrailPoint
        {
            uint32 mapId = 0;
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
        };
        std::vector<TrailPoint> trail; // where the bot walked lately out of combat, oldest first: its way back out of a camp
        std::vector<TrailPoint> trailOut; // a walk back the way it came, out of a place that holds it (newest point first)
        std::size_t trailOutIndex = 0;
        uint32 trailOutSince = 0;      // getMSTime()
        uint32 trailOutMovedAt = 0;    // ... the last time it got on
        float trailOutLastX = 0.0f, trailOutLastY = 0.0f;
        uint32 trailOutUsedAt = 0;     // getMSTime() of the last such walk (one per quarter of an hour, then the hearthstone)
        bool trailOutForCutOff = false; // that walk is the way out of a place cut off from every road: the rescue if it fails
        bool trailOutStraight = false; // that walk is one straight step down off a ledge (OrderService::StepOffLedge)
        // A warrior changed to Berserker Stance to Pummel this foe (Escape::StepInterrupt): the Pummel follows until then.
        ObjectGuid pummelTarget;
        uint32 pummelUntil = 0;        // getMSTime()
        ObjectGuid runnerStopped;      // the last foe that ran for help the bot tried to stop (Escape::StepStopRunner)
        uint32 runnerStopAt = 0;       // getMSTime()
        ObjectGuid runnerSeen;         // the foe of its fight it saw run away last (Escape::StepChaseRunner) ...
        uint32 runnerSeenAt = 0;       // ... since when ...
        float runnerFromX = 0.0f;      // ... and where the bot stood when it began to run
        float runnerFromY = 0.0f;
        ObjectGuid runnerLetGo;        // the runner it let go of (one line per runner)
        uint32 knockSideAt = 0;        // getMSTime() of its last step round a creature that knocks back (Escape::StepKnockbackSide)
        ObjectGuid knockSideFor;       // ... and the creature (one line per creature)
        uint32 ledgeStepAt = 0;        // getMSTime() of the last step down off a ledge (one per quarter of an hour)
        uint32 liftedAt = 0;           // getMSTime() of the last lift out of the ground (LiftOutOfGround)
        // A drop or swim of a dungeon way under way (OrderService::DropDown): until then the bot's way is not planned
        // again, so a member caught half way across does not walk back to the edge and start it anew.
        uint32 dropUntil = 0;          // getMSTime()
        float dropX = 0.0f, dropY = 0.0f, dropZ = 0.0f; // .. its landing
        uint32 dropAt = 0;             // getMSTime() when it began (NavFrom: the way is planned from its landing a while)
        float dropFromX = 0.0f, dropFromY = 0.0f, dropFromZ = 0.0f; // .. where it began
        uint32 swimAlongLogged = 0;    // getMSTime() of the last SwimAlong line
        uint32 dropKeptLogged = 0;     // getMSTime() of the last KeepDropMoving line
        uint32 nextThreatWatch = 0;    // getMSTime(): when the bot next looks for a roaming creature far above it
        ObjectGuid threatSeen;         // the roamer far above it the bot saw last, how far it was, and when
        float threatGap = 0.0f;
        uint32 threatSeenAt = 0;       // getMSTime()
        // The last keep-away that ended (Escape::Result::Clear) was from a creature that keeps to its place: waiting
        // for it to pass is no use, the task's target is counted as one it cannot get to (OrderService::UpdateBot).
        bool asideStood = false;
        // Step-outs from one creature that keeps to its place while the task leads back into its reach (Escape::Step):
        // the third within three minutes counts as asideStood.
        ObjectGuid standOutFrom;
        uint8 standOuts = 0;
        uint32 standOutSince = 0;      // getMSTime()
        // The pack of the last keep-away walks a round (its creatures are on waypoints or in a fight): it passes, the
        // task waits for it (OrderService, Escape::Result::ClearOfPack) instead of ending.
        bool packWalked = false;
        uint8 unfedRests = 0;          // rests in a row that had no food or no drink for what they were for

        // Risk (RiskModel.h): where the bot died lately, and whether it plays it safe after several deaths.
        struct DeathMark
        {
            uint32 mapId = 0;
            float x = 0.0f;
            float y = 0.0f;
            uint32 at = 0;             // getMSTime()
        };
        std::vector<DeathMark> recentDeaths;
        uint32 cautiousUntil = 0;      // getMSTime(), 0 = not cautious
        bool rescue = false;           // a death loop far above the bot's level: the dead bot is taken home
        // Objective watchdog: spawns of a quest the bot left without progress, in a row (quest id -> count).
        std::unordered_map<uint32, uint8> idleSpawns;
        std::unordered_map<uint32, uint8> emptySpawns;   // quest -> spawns in a row without a target in sight
        // An item objective worked at creatures: the fights since its last piece (OrderService::StepTask).
        struct DryKills
        {
            int32 objective = -1;      // the objective they count for
            uint32 have = 0;           // pieces it had when the count began
            uint32 fights = 0;         // fights of its objective tasks since
        };
        std::unordered_map<uint32, DryKills> dryKills;   // by quest
        // Talks with a credit per partner (OrderEvents.cpp): whom the bot has asked (it gave its credit, or had nothing
        // to say) and until when another one is asked instead; partners in a row that did not offer the talk.
        std::unordered_map<ObjectGuid, uint32> talkedTo;
        std::unordered_map<uint32, uint8> talkMisses;
        [[nodiscard]] bool TalkedTo(ObjectGuid guid, uint32 now) const
        {
            auto itr = talkedTo.find(guid);
            return itr != talkedTo.end() && now < itr->second;
        }
        uint8 cautiousLevel = 0;       // cautious while at most this level

        // A dungeon run (OrderDungeons.cpp). The leader's runtime holds the run; deadSince is each character's own.
        struct DungeonRun
        {
            uint32 id = 0;             // the dungeon (Readiness.h), 0 = no run
            uint8 phase = 0;           // 1 on the way to the entrance, 2 inside
            uint32 startedAt = 0;      // getMSTime()
            float approachX = 0.0f, approachY = 0.0f, approachZ = 0.0f; // where the group gathers and comes out again
            uint8 travelFailures = 0;
            uint8 boss = 0;            // index of the boss it is after
            uint8 bossPullTried = 0;   // boss + 1: the tank tried to pull that boss to its waiting group
            ObjectGuid patrol;         // a patrol that crosses the stretch ahead: the group waits for it, then pulls it
            // A big pull (the run window's mode "big pulls"): the tank goes from creature to creature of a cluster and
            // brings what it gathered back to its group.
            struct Gather
            {
                bool active = false;
                bool back = false;          // on its way back to the group
                std::vector<ObjectGuid> targets;
                ObjectGuid target;          // the creature it goes for now
                uint32 since = 0;           // getMSTime() it began
                uint32 targetSince = 0;     // getMSTime() it went for `target`
                uint32 backSince = 0;
                uint32 nextMove = 0;        // getMSTime() of the next movement order
                float campX = 0.0f, campY = 0.0f, campZ = 0.0f; // where the group waits
                uint8 most = 0;             // the most creatures on the tank at once
            } gather;
            ObjectGuid campFor;        // the creature a careful pull's camp was chosen for (the group waits there first)
            float campX = 0.0f, campY = 0.0f, campZ = 0.0f;
            uint32 campSince = 0;      // getMSTime() it was chosen
            uint32 patrolSince = 0;    // getMSTime() the group began to wait for it
            uint8 bossesDown = 0;
            uint32 bossStuckSince = 0; // getMSTime() since the group got no nearer to that boss
            float bossBest = 0.0f;     // ... than this
            float stillX = 0.0f, stillY = 0.0f;  // where the leader stood when it last moved on (the hop when it stands)
            uint32 stillSince = 0;     // ... since then
            uint8 wipes = 0;
            uint32 wipeSince = 0;      // getMSTime() since nobody of the group is alive inside
            // Traps: places where a member lost health out of a fight (the Ironhand Guardians' flames in Blackrock
            // Depths' Iron Hall), for three minutes; the group neither rests nor brings anybody back near one. And
            // what each member had at the last look.
            struct Hazard
            {
                float x = 0.0f, y = 0.0f, z = 0.0f;
                uint32 at = 0;
                uint8 hits = 0;        // a trap from the second loss there within a minute, or a death
            };
            std::vector<Hazard> hazards;
            struct HazardLook
            {
                uint32 health = 0, maxHealth = 0;
                bool alive = false, fight = false;
                uint32 at = 0;         // getMSTime() of the look
            };
            std::unordered_map<ObjectGuid, HazardLook> hazardLooks;
            uint32 hazardLoggedAt = 0;
            uint8 levelHopFor = 0;     // the boss (+ 1) whose floor the group was put on (it stood on another level)
            uint32 restWalkLimit = 30 * IN_MILLISECONDS;  // how long the walk back to a quiet place may take
            ObjectGuid roomInPlace;    // the neutral creature of a boss's room the group last went to fight where it stands
            // Dire Maul North's tribute run (OrderTribute.cpp): its step (0 = not a tribute run or not begun), when the
            // step began, how the inner door is opened (TributeDoor), the named ogres the group fights after all (one came
            // at it, or its means failed), what the step waited for since when, and the guard its buffs come from next.
            uint8 tribute = 0;
            uint32 tributeSince = 0;
            uint8 tributeDoor = 0;
            std::unordered_set<uint32> tributeKill;
            uint32 tributeWaitSince = 0;
            uint32 tributeTries = 0;
            uint32 tributeTrapAt = 0;
            ObjectGuid tributeOpener;
            ObjectGuid tributeMaker;  // who makes the ogre suit (Knot gives it once per character)
            uint8 tributeBuffAt = 0;
            float tributeStillX = 0.0f, tributeStillY = 0.0f;  // where the leader stood still on a tribute step, and since when
            uint32 tributeStillSince = 0;
            uint32 tributeDirectUntil = 0;  // until then the leader walks the core's path to the step's point itself
            uint32 tributeHoldUntil = 0;  // the members stand (the conductor) while the leader in the ogre suit talks to Kromcrush
            uint8 roomHopFor = 0;      // the boss (+ 1) the group was put before to clear its room (no way on foot to it)
            uint8 roomPlaceHopFor = 0; // the boss (+ 1) the group was put at the place it clears its room from (no way there)
            ObjectGuid doorHop;        // a closed door between the group and a hop: it walks to it and steps through (StepThroughDoor)
            uint32 doorHopSince = 0;   // ... since then (it gives that up after a while)
            uint32 fightSeenAt = 0;    // getMSTime() when a member was last seen in a fight, and the fight the group went
            uint32 regroupFor = 0;     // back out of sight after (StepDungeonGroupRest)
            uint8 triggerFor = 0;      // the boss (+ 1) whose area trigger the leader reported (DungeonBoss::areaTrigger), and when
            uint32 triggerAt = 0;
            uint8 triggerWalkFor = 0;  // ... the boss (+ 1) it walks to its trigger for, since when
            uint32 triggerSince = 0;
            uint32 triggerSeen = 0;    // the last step on the way to it
            uint32 deadSince = 0;      // getMSTime() since this character is dead in the dungeon
            uint32 revivedAt = 0;      // ... when its group last brought it back
            uint8 quickDeaths = 0;     // ... deaths soon after that, in a row (the next revival waits longer)
            uint32 lootSince = 0;      // getMSTime() since the group waits for a boss's loot (rolls, what is left in it)
            uint32 lastStep = 0;       // getMSTime() of the last step inside (the stuck clock stands still while the group is busy)
            uint32 nextDoorLook = 0;   // getMSTime() of the next look for a closed door in the way
            uint32 readySince = 0;     // getMSTime() since the group stands at the entrance and waits for its members to recover
            bool repeat = false;       // a further run of the same order: the group is at the entrance already
            bool fresh = false;        // nobody of the group is bound to an old instance of the dungeon (it was reset)
            uint32 resetSince = 0;     // getMSTime() since the group waits at the entrance for that reset
            bool limitNoted = false;   // the wait for the core's hourly limit of instances is in the log
            uint32 gatherSince = 0;    // getMSTime() since the group waits for a member that cannot be teleported yet
            uint32 endSince = 0;       // getMSTime() since the last boss is done (the group lingers, then leaves)
            bool endBossDown = false;  // the boss that ends the dungeon fell in this run (the dungeon counts as finished)
            // How the group fights (OrderService::StepDungeonFight).
            uint32 fightSince = 0;     // getMSTime() since the fight that is going began (0: none)
            uint32 nextPullLook = 0;   // getMSTime() of the next look for a group ahead the tank should pull carefully
            uint16 holds = 0;          // fights in which the others held back for the tank
            uint16 pulls = 0;          // careful pulls of the tank
            uint16 fleeing = 0;        // fleeing creatures the group turned to
            ObjectGuid lastFleeing;    // ... the last one (its log line is written once)
            std::unordered_map<ObjectGuid, uint32> fleeSeen; // when each runner was first seen running (after a while the rule lets go of it)
            bool heldThisFight = false;
            uint32 tankDeaths = 0;     // deaths of the group's tank in this run (it pulls carefully after the first)
            // Members under attack (OrderService::StepDungeonFight): how often the group went to help one, and when
            // each member's help was last written to the log (one line per member and fight).
            uint16 rescues = 0;
            std::unordered_map<ObjectGuid, uint32> rescueLogged;
            // The room of the boss that is next (cleared before the boss is pulled): for which boss (index + 1) the
            // log line was written, since when the group clears it, the place it fights the room's creatures at (out
            // of the boss's reach), whether the room is through, and creatures it could not pull there (until when).
            uint32 roomLogged = 0;
            uint32 roomSince = 0;
            uint32 staleSince = 0;     // since when nothing happens in the group's fight (StepDungeonFight ends it)
            float roomX = 0.0f, roomY = 0.0f, roomZ = 0.0f;
            bool roomDone = false;
            std::unordered_map<ObjectGuid, uint32> roomSkip;
            // Into the leader's instance (OrderService::StepDungeonEnter): the members still to go in, when the next
            // look comes, and the members already told to have stood in another instance.
            std::vector<ObjectGuid> enterPending;
            uint32 nextEnterLook = 0;
            std::vector<ObjectGuid> strayLogged;
            // A raid run (DungeonPlan::raid): when the raid group's flags were looked at last (KeepRaidGroup), since when
            // the leader waits for its raid group before it goes in, since when the group waits at the place of a boss
            // the instance brings (DungeonBoss::summoned), and whether that boss was seen there.
            uint32 raidLookAt = 0;
            uint32 raidWaitSince = 0;
            uint32 summonWaitSince = 0;
            bool summonSeen = false;
            // Out of the fire of a creature that bursts at its death (StepRaidBursts): when each member may step again,
            // and the kinds already named in the log.
            std::unordered_map<ObjectGuid, uint32> burstStepAt;
            std::unordered_set<uint32> waySent; // the area triggers of StepWayTriggers reported in this run
            std::set<uint32> burstLogged;
            // The Twin Emperors on two sides (StepTwinsPull, StepRaidTwins): each member's side (0: Vek'nilash's throne,
            // 1: Vek'lor's) and kind (0 tank, 1 healer, 2 physical damage, 3 magic damage, 4 warlock that tanks
            // Vek'lor), the place of each side, when the raid began to take its places and when it pulled; the side
            // Vek'nilash stood at the last look and when the twins last swapped places; the lock tanks' potions.
            std::unordered_map<ObjectGuid, std::pair<uint8, uint8>> twinSide;
            float twinX[2] = { 0.0f, 0.0f }, twinY[2] = { 0.0f, 0.0f }, twinZ[2] = { 0.0f, 0.0f };
            float twinCenterX = 0.0f, twinCenterY = 0.0f;
            uint32 twinPlacesSince = 0;
            uint32 twinPlacedAt = 0;      // everybody stood at its place (StepTwinsPull)
            uint32 twinPulledAt = 0;
            bool twinOpened = false;   // the casters of Vek'lor's half have opened on him (StepRaidTwins)
            std::unordered_map<ObjectGuid, uint32> twinStepAt;
            uint8 twinNilashSide = 0;
            uint32 twinSwapAt = 0;
            std::unordered_map<ObjectGuid, uint32> twinPotionAt;
            // Vek'lor's guards and the bugs of the twins' room killed before the places (StepTwinGuards, StepTwinsPull):
            // the raid goes for the guards, since when it clears the bugs and whether it is done; when the fight was last
            // in the log (StepRaidTwins).
            bool twinGuards = false;
            uint32 twinBugsSince = 0;
            bool twinBugsDone = false;
            uint32 twinNoteAt = 0;
            // Viscidus (StepRaidViscidus): his phase as the raid saw it last (0 none, 1 frost, 2 frozen, 3 globs), when it
            // began, when each member may act again and when it drank its last nature protection potion.
            uint8 viscPhase = 0;
            uint32 viscPhaseSince = 0;
            uint32 viscFrozenAt = 0;
            std::unordered_map<ObjectGuid, uint32> viscStepAt;
            std::unordered_map<ObjectGuid, uint32> viscPotionAt;
            // C'Thun (StepRaidCthun): when the Eye's Dark Glare began, the angle it began at and the way it turns (0 not
            // known yet, 1 or -1), each member's place on the rings around the Eye, and when each member may act again.
            uint32 glareSince = 0;
            float glareAngle = 0.0f;
            int8 glareWay = 0;
            float glareStartTurn = 0.0f;  // how far the glare's line had turned when its way became known
            float glareTurn = 0.0f;       // how far it has turned (the raid turns with it)
            std::unordered_map<ObjectGuid, std::pair<float, float>> glareTarget; // a runner: its distance, its angle
            bool cthunWeak = false;     // C'Thun's carapace is off (his flesh tentacles are dead)
            uint32 cthunCycleAt = 0;    // getMSTime() his tasks began anew (the body's phase, the end of a weakened window)
            std::unordered_set<ObjectGuid> cthunInside; // members in C'Thun's stomach (for the log)
            std::unordered_map<ObjectGuid, uint32> cthunExitAt; // getMSTime() a member sent the stomach's exit trigger
            uint32 cthunNoteAt = 0;     // getMSTime() of the last line on C'Thun's health
            std::unordered_map<ObjectGuid, std::pair<float, float>> cthunSlot;   // radius, angle
            uint32 cthunFightSince = 0;          // when the Eye began to fight (getMSTime), 0 before
            std::unordered_set<ObjectGuid> cthunPut; // put at their places on the rings when the Eye began
            std::unordered_map<ObjectGuid, uint32> cthunStepAt;
            // The hunter that pulls the Eye and takes its first beams away from the raid (StepCthunPull), and when it
            // drank its nature protection potion.
            ObjectGuid cthunPuller;
            uint32 cthunPotionAt = 0;
            // The wipes at each boss of the run's order (a raid leaves a boss that wiped it twice for this run).
            std::unordered_map<uint8, uint8> bossWipes;
            // Hakkar (StepRaidHakkar): when the raid's fight with him began, when his next Blood Siphon comes, and the
            // siphon whose cloud and Son lines are in the log.
            uint32 hakkarSince = 0;
            uint32 hakkarSiphonAt = 0;
            uint32 hakkarSiphonSeen = 0;
            uint32 hakkarWindow = 0;
            uint32 hakkarSonWindow = 0;
            // A raid's tank swap (StepRaidTankSwap): the tank that took the boss over (the main tank flag follows it) and
            // when the last swap was.
            ObjectGuid raidMainTank;
            uint32 tankSwapAt = 0;
            // Ossirian (StepRaidOssirian): the crystal its tank last took him to (logged once each).
            ObjectGuid ossirianCrystal;
            uint32 ossirianUseAt = 0;    // the next use of a crystal near Ossirian, not before (StepRaidOssirian)
            uint32 raidHoldUntil = 0;    // a raid mechanic holds the raid where it stands until then: nobody goes to help
            uint8 pullPlaceLogged = 0;   // the boss (1-based) whose walk to the place it is pulled to is in the log
            uint8 longPullFor = 0;       // the boss (1-based) the tank walks up to alone to pull it to its group's place
            uint32 longPullSince = 0;
            uint32 longPullStepAt = 0;
            uint32 longPullFail = 0;     // the cast result (+1) of its taunt that is in the log
            // Razorgore (StepRaidRazorgore): the next step and orb use, the eggs tried (by guid) and broken, the orb's uses.
            uint32 razorStepAt = 0;
            uint32 razorUseAt = 0;
            uint32 razorEggs = 0;
            uint32 razorUses = 0;
            uint32 razorFailLogged = 0;
            std::unordered_map<ObjectGuid, uint32> razorEggTried;
            // The character the owner plays by hand in the group (OrderService::StepDungeonGuest): since when the
            // group waits for it, whether it went on without it, when it was last offered a resurrection, and what
            // it has been told.
            uint32 guestWaitSince = 0;
            bool guestLeftBehind = false;
            uint32 guestResAt = 0;
            bool guestToldStart = false;
            // A boss that an event brings (DungeonBoss::talkEntry): when the leader started the event, whether the boss
            // has been seen, how often the talk was tried, when the boss was found dead, when its NPC was last seen.
            uint32 eventSince = 0;
            bool eventBossSeen = false;
            uint8 eventTalks = 0;          // ... talks so far; for an object, the uses
            uint32 eventNextTalk = 0;
            uint32 eventDoneSince = 0;
            uint32 eventNpcSeen = 0;
            uint32 eventNextHop = 0;   // the group is put beside the event's NPC at most this often
            uint32 eventRouteAt = 0;   // the next point of the NPC's way the group walks before it starts the event
            uint32 objectAt = 0;       // DungeonBoss::useObjects: the next one to use, when, and since when it walks to it
            // DungeonBoss::clearFirst: the boss (+ 1) it clears for, since when, the creature it goes for now, and those it
            // does not get to.
            uint8 clearFor = 0;
            uint8 clearLogged = 0;
            uint32 clearSince = 0;
            uint32 clearDoneAt = 0;
            uint16 clearCount = 0;
            ObjectGuid clearTarget;
            ObjectGuid clearHopTarget;  // the creature the group was put near while it cleared what holds a boss back
            uint32 clearTargetSince = 0;
            float clearBest = 0.0f;
            std::unordered_set<ObjectGuid> clearSkip;
            uint32 objectNext = 0;
            uint32 objectSince = 0;
            float objectBest = 0.0f;       // the nearest the leader came to that object since objectSince
            float objectStillX = 0.0f, objectStillY = 0.0f, objectStillZ = 0.0f; // where the leader stood when its walk to an object last got on
            uint32 objectStillSince = 0;   // getMSTime() of that (wall clock: a group that is busy all the time never moved its clock)
            uint32 objectHopFor = 0;       // the object (entry) the group was put beside already
            uint32 objectsUsed = 0;
            uint32 eventRouteSteps = 0; // steps towards that point (it is put there when it does not get to it)
            bool eventRouteBegun = false;
            uint32 eventHops = 0;
            uint32 required = 0;       // bosses down that the dungeon's end counts (not the ones an event brings)
            bool hopped = false;       // the group was put near the boss it is after (it found no way on foot)
            // Who of the group has yet to use what fits it out of the guild storage: everybody once at the start of a
            // run and once after a wipe, each at its first quiet moment inside.
            std::vector<ObjectGuid> suppliesPending;
            // Who has had this round of them: a member that comes into the group later (the guild leader's character
            // once the AI plays it) gets its round then.
            std::vector<ObjectGuid> suppliesGiven;
            // Members that came into the world after the run had begun and were put beside their leader (once each).
            std::vector<ObjectGuid> lateGathered;
            // The run's bosses in the order of the way (indexes into the plan's list, OrderService::BossOrder, package P4),
            // set when the group is first inside; a boss passed over is tried once more before the boss that ends it.
            std::vector<uint8> order;
            std::unordered_set<uint8> retried;
            ObjectGuid roomTarget;      // the creature of a boss's room the group works on (the next one when it is dead)
            // The conductor (OrderService::StepDungeonConductor, package P2): the tank's way inside (a point every 1.5 yd,
            // the newest first), where each member was last sent and when, the group as the leader's last step saw it, and
            // since when a member stands farther than the spread limit from the tank (StepDungeonSpread).
            struct TrailPoint { float x = 0.0f, y = 0.0f, z = 0.0f; };
            std::deque<TrailPoint> trail;
            std::unordered_map<ObjectGuid, TrailPoint> slotSent;
            std::unordered_map<ObjectGuid, uint32> slotSentAt;
            std::vector<ObjectGuid> groupGuids;
            std::unordered_map<ObjectGuid, uint32> strayedSince;
            uint32 spreadSince = 0;
            uint32 spreadNextLog = 0;
            // The fight rules of package P5: the creature marked with the skull (the group's one target), the tank's next
            // taunt and area threat, and when a healer or ranged member under attack was last sent to the tank.
            ObjectGuid skull;
            // The Twin Emperors' marks are set (the cross on Vek'lor, the members' "rti" by the damage they deal).
            bool twinMarks = false;
            uint32 nextTaunt = 0;
            uint32 nextAreaThreat = 0;
            std::unordered_map<ObjectGuid, uint32> toTankAt;
            // Since when a creature on a member (not the tank) has stood more than 15 yd from the tank: one that stays
            // off (a caster, a member too far to run to the tank) is gone to by the tank (StepDungeonRescue).
            std::unordered_map<ObjectGuid, uint32> offTankSince;
            // The healer of the group (package P6a): heals through (no saving of mana) while the fight is heavy; the
            // tank's health of the last seconds for it, and whether its damage and mana-saving strategies are off.
            bool pumping = false;
            bool pumpLogged = false;
            std::deque<std::pair<uint32, float>> tankHealth;
            ObjectGuid healerDpsOff;     // the healer whose "healer dps" the run took away
            ObjectGuid saveManaOff;      // ... and whose "save mana" (while it heals through)
            // The group rests together (package P6b): since when, and the healer's next walk to a place that sees the tank.
            uint32 groupRestSince = 0;
            uint32 groupRestBegan = 0; // when the rest began (its walk back to a place out of sight included)
            // Where the group goes back to before it rests, when creatures would see it where it stands.
            bool restSpot = false;
            float restX = 0.0f, restY = 0.0f, restZ = 0.0f;
            uint32 restWalkSince = 0;
            uint32 nextHealerMove = 0;
            // The buff plan (package P8): the next look, whether the providers were logged, the blessings named to
            // the paladin its owner plays by hand (their spells), the shamans whose totems were set apart.
            uint32 nextBuffLook = 0;
            bool buffsLogged = false;
            std::vector<uint32> blessingsTold;
            // The way in stretches (package P3): the next look at what stands on it, and the stretch's end last sent.
            uint32 nextWayLook = 0;
            ObjectGuid wayBlocker;       // the creature the tank walked into on its way: its target until it fights
            uint32 wayBlockerAt = 0;     // getMSTime() of that decision
            bool walkInWalking = false;  // the tank walks to it itself (GoForWalkIn): its AI is held while it does
            ObjectGuid walkInNamed;      // the creature the last walk-in's line named (one line and count per creature in 20 s)
            uint32 walkInNamedAt = 0;
            // Creatures GoForWalkIn left out for their long way (the group meets them on its own way): until when, and
            // when the line last named each (once in 10 min).
            std::unordered_map<ObjectGuid, std::pair<uint32, uint32>> wayRefused;
            uint32 wayKey = 0;           // counts those decisions (the key of the gate before each)
            float wayX = 0.0f, wayY = 0.0f;
            // The gate before a boss or a big pull (OrderService::DungeonGate, package P1): what it waits at, since when,
            // who has been away from the tank since when, and who was named for having nothing to drink.
            uint32 gateKey = 0;        // run.boss + 1 for a boss, 1000 + the pull's number for a big pull (0 = none)
            uint32 gateSince = 0;      // getMSTime()
            uint32 gateOpenFor = 0;    // run.boss + 1: the gate before this boss opened (a fight after it closes it again)
            uint32 gateNextLog = 0;
            std::string gateMissing;
            std::unordered_map<ObjectGuid, uint32> gateAway;
            std::unordered_set<ObjectGuid> gateDryNamed;
            bool gateTold = false;     // the character its owner plays by hand was told the group waits for it
            // The acceptance numbers of a run (docs/orders.md "The acceptance numbers of a run", package P7), sampled every
            // tick of its leader inside and written at its end: the group's fights (from the first character in combat to
            // the last out of it), who was in each first and how late each member came after the tank; the bosses and
            // whether the whole group stood ready at the tank when each was pulled; the leader's waits for a member far
            // behind and its walks on without a member that rested; the way the leader walked.
            struct Metrics
            {
                bool inFight = false;
                uint32 fightStart = 0;     // getMSTime() the first character of the group came into the fight
                uint32 releasedAt = 0;     // getMSTime() the group was free to fight (the tank had the creatures, or its lead time
                                           // was over: the owner's DP3 holds the others back until then)
                uint32 quietSince = 0;     // getMSTime() since nobody of the group is in combat (3 s close the fight)
                std::unordered_map<ObjectGuid, uint32> enteredAt; // ... when each character came into it
                std::unordered_map<ObjectGuid, float> startYards; // ... how far each was from the tank when it began (debug)
                uint16 fights = 0;
                uint16 memberFirst = 0;    // fights a member was in before the tank (or the tank never)
                uint16 memberPulled = 0;   // ... of them, fights a member came into before the tank by its own attack (nothing
                                           // was attacking it then), not because a creature came at it first
                std::unordered_set<ObjectGuid> ownAttack; // members that came into the fight by their own attack
                uint16 meleeIn = 0, meleeLate = 0;   // melee damage dealers in the tank's fights, and 5 s or more after it
                uint16 healerIn = 0, healerLate = 0; // the healer the same
                uint16 othersIn = 0, othersLate = 0; // everybody else of the group but the tank
                uint16 meleeFreeLate = 0, othersFreeLate = 0; // ... 5 s or more after the group was free to fight
                std::unordered_set<uint32> bossesJudged; // boss entries whose pull was judged
                uint16 bossPulls = 0, bossReady = 0;
                uint16 catchUps = 0;       // the leader waited for a member far behind (over 60 yd)
                uint16 wentOnResting = 0;  // the leader went on without a member that rested
                float walked = 0.0f;       // yards the leader walked inside (teleports and hops left out)
                // Where the members stand 5 s into a fight: within 15 yd of the tank or farther (the members that hold back
                // while the tank pulls come into the fight late by design; this counts where they are).
                bool placeJudged = false;
                uint16 placedIn = 0, placedFar = 0;
                uint16 healerReach = 0, healerOut = 0; // the healer judged 5 s into a fight, and out of heal reach of the tank
                uint16 spreadHalts = 0;    // the tank stood for a member farther than 15 yd (P2)
                uint16 taunts = 0, areaThreats = 0, toTank = 0; // the tank's taunts and area threats, members sent to it (P5)
                uint16 pumps = 0;          // fights in which the healer healed through (P6a)
                uint16 walkIns = 0, campPulls = 0; // packs on the way the tank walked into, and pulled to a camp (P3)
                uint16 bigPulls = 0, bigPullMost = 0; // big pulls of the tank, and the most creatures on it in one
                uint16 bossUnseen = 0;     // bosses that fell before their pull was judged
                uint16 groupRests = 0;     // rests of the whole group (P6b), and the healer's walks to see the tank
                uint16 healerMoves = 0;
                uint16 fetched = 0;        // members put beside the tank after a minute without coming (P1, P2)
                uint16 gates = 0;          // gates before a boss or a big pull that opened (P1), and the time they took
                uint32 gateMs = 0;
                bool hasLast = false;
                float lastX = 0.0f, lastY = 0.0f, lastZ = 0.0f;
            };
            Metrics metrics;
        };
        DungeonRun dungeon;

        // The order FARM (OrderFarm.cpp, Farming.h).
        struct Farm
        {
            uint32 item = 0;           // the item this state belongs to (another item: the state starts afresh)
            uint32 nextLook = 0;       // getMSTime(): when hunting grounds are next looked for
            uint8 problem = 0;         // FarmProblem of the last look (0: it has a ground)
            uint32 noticeAt = 0;       // getMSTime() of the last notice to the leader that there is nothing to farm
            uint8 noticeProblem = 0;   // ... and what it said
            uint8 noticeLevel = 0;     // ... at which level of the bot
            uint32 looted = 0;         // items of the order looted since it began (or since the bot entered the world)
            uint32 nextStockLook = 0;  // getMSTime(): when the stock is next counted for the order's target stock
            // Grounds it has emptied: not chosen again before their creatures are back.
            struct Emptied
            {
                uint32 mapId = 0;
                float x = 0.0f;
                float y = 0.0f;
                uint32 until = 0;      // getMSTime()
            };
            std::vector<Emptied> emptied;
            // The corpse of its own kill it walks to before the next target (auto loot takes what is in reach).
            ObjectGuid corpse;
            uint32 corpseSince = 0;    // getMSTime()
            uint32 corpseAssert = 0;   // getMSTime() of the last walk command to it
            std::vector<ObjectGuid> corpsesLeft; // corpses it did not get to: not tried again
        };
        Farm farm;

        // Squads (Squads.h, OrderSquads.cpp).
        struct Squad
        {
            bool following = false;    // a member that follows its leader this tick (its own order waits)
            ObjectGuid leader;         // ... that leader
            uint8 state = 0;           // SquadState of the last tick
            bool leads = false;        // a leader with members that follow it now
            bool besideLeader = false; // member: within 40 yd of its leader (it fights the squad's fight)
            std::string waitFor;       // leader: whom it waits for and why ("" = it does not wait)
            ObjectGuid waitMember;     // ... the member that rests or catches up (the leader waits beside it, StepSquadWait)
            uint32 waitBesideAt = 0;   // getMSTime(): its last step towards that member
            uint32 waitSince = 0;      // getMSTime() the current wait began
            uint8 waitUrgency = 0;     // 4 a dead member, 3 one that rests, 2 one that fights, 1 one behind
            uint32 noWaitUntil = 0;    // getMSTime(): after a wait that ran out the leader goes on until then
            uint32 farSince = 0;       // member: getMSTime() since it is far behind its leader
            uint32 wayCheckedAt = 0;   // member in a dungeon run: when its way on foot to its leader was looked at
            bool noWayToLeader = false; // ... and that it had none
            bool leaderCanCome = false; // ... and that its leader had a way to it (it comes back for the member)
            uint32 fetchSince = 0;     // member: getMSTime() since its leader comes back for it
            ObjectGuid fetchMember;    // leader: the member it walks back to (no way leads from it to the leader)
            uint32 fetchUntil = 0;     // leader: getMSTime() until which it does
            std::string fetchWhy;      // leader: why ("has no way up to it", or the creature on the member's way)
            ObjectGuid quietOnWay;     // member in a dungeon run: a creature in no fight its way to its leader passes
            std::string quietName;     // ... its name and level, for the log
            bool waitsForFetch = false; // member: it waits where it is while its leader comes back for it (it holds)
            uint32 nextQuests = 0;     // member: when it next looks at its leader's quests
            std::unordered_set<uint32> shared;             // member: quests it took because its leader took them
            std::unordered_map<uint32, uint32> skipShare;  // quest -> getMSTime() until which it is not tried again
            // Quest partners (QuestPartners.h): the quest of a partner squad the bot is in (member or leader), a member on
            // its way to a leader it has not met yet (it walks; it does not follow yet), a meeting walk that found no way.
            uint32 partnerQuest = 0;
            bool meeting = false;
            bool meetFailed = false;
            // A member on another level than its leader with no way on foot to it (the leader rode a lift): it walks to
            // it along the roads, as a quest partner does (meeting set too); not again before rejoinPauseUntil after a
            // walk that found no way either.
            bool rejoining = false;
            bool rejoined = false;      // ... it was on such a walk last tick (the walk's end says "back beside")
            uint32 rejoinPauseUntil = 0;
            // A member far behind (250 yd, another map: its leader flew, took a ship or a portal) travels after it as a
            // player would (StepRejoinTravel); one whose travel found no way may be put beside it (rejoinFallback).
            bool rejoinFar = false;
            bool rejoinFallback = false;
            // .. one whose travel to its leader found no way waits at a meeting point near it (the flight master of its
            // side nearest to its leader) and its leader comes to fetch it (fetchStartedAt, the leader's task fetchSquad).
            bool meetAt = false;
            bool meetWaiting = false;   // .. it stands there
            uint32 meetWaitSince = 0;   // getMSTime()
            uint32 meetMap = 0;
            float meetX = 0.0f;
            float meetY = 0.0f;
            float meetZ = 0.0f;
            std::string meetName;
            uint32 fetchStartedAt = 0;  // leader: getMSTime() its fetch of a waiting member began (fetchSquad)
            uint32 fetchingSince = 0;   // leader: getMSTime() since it fetches that member at all
        };
        Squad squad;
    };
}

class OrderService
{
public:
    static OrderService& Instance();

    // Reads the guilds' deaths of the last hours (ga_guild_death; startup): the zone pick and the quest planner
    // look at them (a zone that keeps killing the guild, a quest a higher-level character died on).
    static void LoadGuildDeaths();
    // The hand-listed crossings of gaps in the path finder's ground (ga_path_bridge; startup).
    static void LoadPathBridges();
    // What a character waits with survives a restart (ga_bot_wait): the hour on its side of the sea after it went
    // ashore, the zones that wait for it, the hand-ins outside its zone it gave up. The integration server restarts
    // every half hour: a bot crossed the sea three times in 22 minutes, each time right after a restart had made it
    // forget the landing and the hand-in it had just given up. LoadBotWaits at startup, RestoreBotWaits when a
    // character enters the world, SaveBotWaits every few seconds (it writes when something changed).
    static void LoadBotWaits();
    static uint32 RestoreBotWaits(ObjectGuid guid, GuildAdventure::BotRuntime& rt);
    static void SaveBotWaits(ObjectGuid guid, GuildAdventure::BotRuntime& rt);
    // A quest NPC the guild's characters could not get to three times within a day: left alone by all of them for a
    // day (ga_guild_npc_block).
    [[nodiscard]] static bool IsNpcBlocked(Player const* player, GuildAdventure::SpawnPoint const& spawn);
    // The walk to the spawn a task is worked at was given up: it counts against that spawn for the guild.
    static void NoteSpotUnreached(Player const* player, GuildAdventure::BotTask const& task);

    // A place to hunt creatures that drop an item (WorldIndex's item sources: quest items and the materials of
    // profession recipes). Of the creatures the character may fight there (not elite, attackable, on its land mass,
    // not in a zone above it, not at a place it keeps away from, the place usable by the risk rules; `maxLevel`: only
    // kinds up to this level, 0 = up to its own level and what the risk rules let it fight above it), the place with
    // the most of them standing within GroundYards of each other for the way it takes to get there: groups in an
    // area one walks, not single creatures far apart. `minGroup`: at least this many spawns at the place.
    // Used by the profession mode's material hunt (PlanMaterialHunt) and meant for the FARM order.
    struct HuntGround
    {
        GuildAdventure::SpawnPoint spot;   // a spawn in the middle of the group
        uint8 minLevel = 0, maxLevel = 0;  // the levels of the kinds that drop the item there
        uint32 spawns = 0;                 // spawns of such creatures within GroundYards of the spot
        float distance = 0.0f;             // from the character, as the crow flies
    };
    static constexpr float GroundYards = 80.0f;
    [[nodiscard]] std::optional<HuntGround> FindHuntGround(Player* player, GuildAdventure::BotRuntime const& rt, uint32 item,
        uint8 maxLevel = 0, uint32 minGroup = 1) const;
    // The planner's state of a character, for callers of FindHuntGround outside the planner (nullptr: not planned).
    [[nodiscard]] GuildAdventure::BotRuntime const* GetRuntime(ObjectGuid guid) const;

    void Update(uint32 diff);
    // The planner's counters of the last minute (docs/performance.md PF-0): lines for ".ga perf".
    [[nodiscard]] std::vector<std::string> DescribePerf() const;

    // The order of a bot changed (protocol ORDER_SET, .ga bot order): forget the current plan.
    void OnOrderChanged(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot);
    // Whether the bot may get the order DUNGEON for that dungeon (Readiness.h id); `error` says why not.
    bool CheckDungeonOrder(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot, uint32 dungeonId,
        std::string& error) const;
    // The dungeon run window (docs/orders.md "The dungeon run window"): the guild leader picks a dungeon, the bots of the
    // group and the number of runs. The bots become the squad of one of them (`lead`, else the first tank, else the
    // first) for the run and get back what they were afterwards. `runs`: 1 to GuildAdventure.Dungeons.MaxRuns, or
    // DungeonRunsEndless.
    static constexpr uint32 DungeonRunsEndless = 255;
    // What the guild leader sets in the run window's preparation step (stage C): the kinds of items of the guild
    // storage the group may use during the run (DungeonSupplies.h), and how the group fights (unset: the server's
    // GuildAdventure.Dungeons.* values).
    enum DungeonMode : uint8
    {
        ModeBosses = 0,
        ModeRooms = 1,
        ModeBigPulls = 2,
        // Dire Maul North's tribute run (the owner, 2026-11-21; OrderTribute.cpp): the guards live, King Gordok dies.
        ModeTribute = 3
    };
    struct DungeonFightRules
    {
        uint32 tankLeadSeconds = 4;
        uint32 emergencyHealth = 50;
        bool carefulPulls = true;
        uint32 carefulPullPack = 3;
        bool focusFleeing = true;
        // How much the group fights on its way (the owner, 2026-11-19), the run window's choice: ModeBosses, only what would
        // attack it on its way, one pack at a time; ModeRooms, every room it walks through, generously around its way in
        // the open, one pack at a time; ModeBigPulls, as ModeRooms, but the tank gathers what stands within gatherYards
        // of the first creature and stops when gatherCount creatures are on it.
        uint8 mode = 0;
        uint32 gatherYards = 25;
        uint32 gatherCount = 6;
    };
    struct DungeonRunOptions
    {
        // What the group may use out of the guild storage: every kind of item a run uses except `excluded` (the
        // run window's preparation step: everything is ticked, the leader takes kinds out), or with `listed` only the
        // kinds of `items` (the ticks of an addon from before 2026-11-14).
        bool listed = false;
        std::vector<uint32> items;
        std::vector<uint32> excluded;
        bool hasRules = false;
        DungeonFightRules rules;
    };
    // The server's values (the defaults of the preparation step).
    [[nodiscard]] static DungeonFightRules DefaultFightRules();
    // The rules of the run a bot leads (its group's, else the server's), and the items its group may use.
    [[nodiscard]] DungeonFightRules GetFightRules(ObjectGuid leader);
    [[nodiscard]] std::vector<uint32> GetRunItems(ObjectGuid leader);
    // `checkUnlock`: the dungeon has to be open for the guild (IsDungeonUnlocked); the test aids pass false.
    // True for a member of a dungeon group (its squad leader has the order DUNGEON): a character of that kind that its
    // owner plays by hand carries a placeholder AI (GuildBotHolder::SetPlaceholderPolicy).
    [[nodiscard]] bool IsDungeonGuest(ObjectGuid guid) const;
    bool StartDungeonGroup(GuildAdventure::AdventureGuild& guild, uint32 dungeonId, std::vector<ObjectGuid> const& bots, ObjectGuid lead,
        uint32 runs, std::string& error, bool checkUnlock = true, DungeonRunOptions const* options = nullptr);
    // `now`: the run ends at once (the group leaves); else the run that is going is the order's last.
    bool StopDungeonRun(GuildAdventure::AdventureGuild& guild, ObjectGuid leader, bool now, std::string& error);
    // Test aid (.ga admin dungeonskip): the run that is going inside its dungeon goes on at boss number <number> of its
    // order; every boss before it still alive is killed by the leader, so that what its death opens is open.
    bool SkipDungeonBosses(GuildAdventure::AdventureGuild& guild, ObjectGuid leader, uint32 number, std::string& result);
    struct DungeonRunStatus
    {
        ObjectGuid leader;
        uint32 dungeon = 0;
        std::string name;
        uint8 phase = 0;           // 0 not started (the leader is not in the world), 1 getting ready, 2 inside
        uint8 bossesDown = 0;
        uint8 bosses = 0;
        std::string nextBoss;      // the boss the group is after ("" = none left)
        uint8 wipes = 0;
        uint32 runsLeft = 1;       // the run that is going included; DungeonRunsEndless
        uint32 runsDone = 0;
        uint32 minutes = 0;        // of the run that is going
        std::vector<ObjectGuid> members; // without the leader
        std::string why;
    };
    std::vector<DungeonRunStatus> GetDungeonRunStatus(GuildAdventure::AdventureGuild& guild);
    // The order of the dungeons (docs/orders.md "The dungeons in their order"): a guild's side's dungeons a run can
    // be ordered into, by level. The first is open; a finished run (the end boss down) opens the next one.
    [[nodiscard]] std::vector<uint32> DungeonOrder(GuildAdventure::AdventureGuild const& guild) const;
    // How often the guild has finished a dungeon.
    uint32 GetDungeonsFinished(GuildAdventure::AdventureGuild const& guild, uint32 dungeonId);
    // False with `need` = the dungeon that has to be finished first (GuildAdventure.Dungeons.UnlockInOrder).
    bool IsDungeonUnlocked(GuildAdventure::AdventureGuild const& guild, uint32 dungeonId, std::string* need = nullptr);
    // Counts every finished run of the guild (a change tells the protocol to send the dungeons again).
    uint32 GetDungeonProgressVersion(GuildAdventure::AdventureGuild const& guild);
    // Test aid (.ga admin dungeondone): the guild has finished the dungeon `runs` times (0: not at all).
    void SetDungeonsFinished(GuildAdventure::AdventureGuild const& guild, uint32 dungeonId, uint32 runs);
    // Whether everyone of the bot's group who is in the world is ready for the dungeon (Readiness, at least 50 % each);
    // `summary` gets "name percent" for each. Refused with the names when not.
    // Repeated runs (ga_bot_dungeon_runs): how many runs the bot's dungeon order still has, the one that is going
    // included (1 without a row), and how many it has done. SetDungeonRuns belongs to a new order (1 = one run).
    void SetDungeonRuns(ObjectGuid bot, uint32 runs);
    [[nodiscard]] uint32 GetDungeonRuns(ObjectGuid bot);
    [[nodiscard]] uint32 GetDungeonRunsDone(ObjectGuid bot);
    bool CheckDungeonReady(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot, uint32 dungeonId,
        std::string& error, std::string* summary = nullptr) const;
    // A bot inside a dungeon that has no run there (its order was changed in the middle of one, a restart after the
    // run's end): back to the dungeon's entrance in the open world. True while that is what it does.
    bool LeaveDungeonWithoutRun(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player,
        GuildAdventure::BotRuntime& rt);
    // Whether the order FARM can be given for an item (Farming.h: it exists and creatures bots grind drop it for the
    // guild's side); `error` says why not.
    bool CheckFarmOrder(GuildAdventure::AdventureGuild const& guild, uint32 item, std::string& error) const;
    // The loot filter saw a character with the order FARM loot `count` of its item (the why line counts them).
    void OnFarmLoot(ObjectGuid bot, uint32 count);
    // The leader's order (docs/orders.md "The leader"): an order makes the planner play the leader whenever the AI
    // holds it; nullopt ("HOLD") gives it back to the hold policy (stand where it is, its owner's character as master).
    // Persists the change. The caller ends watch mode of a leader that goes back to hold.
    void SetLeaderOrder(GuildAdventure::AdventureGuild& guild, std::optional<GuildAdventure::BotOrder> order);
    // The bot reached a new level (queued hook, world thread).
    void OnLevelUp(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, uint8 level);
    // What the nearest class trainer of the character's side would teach it now, learned where it stands (test aid of
    // .ga admin boost; the planner does it on a level-up). Returns how many spells.
    uint32 LearnClassSpellsNow(Player* player);
    // The module moved the character itself (an instant flight of the guild perks): no jump of the planner's, the task
    // goes on from the new place.
    void OnModuleTeleport(ObjectGuid bot);
    // Whether the risk model lets this character fight `count` creatures of up to `level` at once (true for a character
    // the planner does not play). For `.ga admin route`: the route a bot's walk would take.
    [[nodiscard]] bool CanTakeGroup(Player const* player, uint8 level, uint8 count) const;

    // Master policy for GuildBotHolder: only bots that follow get a playerbots master (a leader without an order
    // always has one).
    [[nodiscard]] bool WantsMaster(ObjectGuid bot) const;
    // True for a guild leader that has an order (the planner plays it); false for everyone else.
    [[nodiscard]] bool IsLeaderPlanned(ObjectGuid guid) const;

    // The live "why" line of an AI-driven bot, empty if the service has none (offline, controlled, not started).
    [[nodiscard]] std::string GetWhy(ObjectGuid bot) const;
    [[nodiscard]] uint32 GetXpPerHour(ObjectGuid bot) const;
    [[nodiscard]] uint32 GetMoneyPerHour(ObjectGuid bot) const;

    // Multi-line description of the planner state for the .ga bot plan command.
    [[nodiscard]] std::vector<std::string> Describe(ObjectGuid bot) const;
    // For the addon's control bar (its Details window, GADV GET what=plan): what a guild character does now and what
    // comes next, in words for its owner: pairs of kind (now, task, squad, wait, run, hand, quest, errand, upkeep, aside) and
    // text; wait: for a squad member, what its leader does and why it waits (LeaderWhyForMember).
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> DescribeNext(ObjectGuid bot) const;
    // For ".ga admin farm": where a character would farm an item (Farming.h), with its planner state.
    [[nodiscard]] std::vector<std::string> DescribeFarm(Player* player, uint32 item) const;
    // For ".ga admin spot": how the risk model judges a spot for a character (RiskModel.h), with its planner state.
    [[nodiscard]] std::string DescribeSpot(Player* player, float x, float y, float z, uint32 creatureEntry) const;

    void Forget(ObjectGuid bot);

private:
    OrderService() = default;
    // Quest partners (QuestPartners.h) read and reset the planner state of the bots they pair up.
    friend class QuestPartnerService;

    // One planner tick for a roster bot or a planned leader.
    void Step(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot);
    void UpdateBot(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player,
        GuildAdventure::BotRuntime& rt);
    void Reconcile(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt);
    void Sample(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // How active the bot is: the seconds it stands, the yards it walks, its turns back and target switches (the status line).
    void TrackActivity(Player* player, GuildAdventure::BotRuntime& rt, bool busy);

    // Orders
    void RunAdventure(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player,
        GuildAdventure::BotRuntime& rt);
    void RunIdle(GuildAdventure::GuildBot& bot, Player* player, GuildAdventure::BotRuntime& rt);
    void RunFollow(GuildAdventure::GuildBot& bot, Player* player, GuildAdventure::BotRuntime& rt);

    // Zones
    uint32 ChooseZone(GuildAdventure::AdventureGuild const& guild, GuildAdventure::GuildBot const& bot, Player* player,
        uint32 exclude) const;
    uint32 CountEligibleQuests(Player* player, uint32 zoneId, bool afar = false) const;
    // The strongest hostile creatures (the road map's danger) on the safest way from the zone's hub to the nearest place
    // where a quest's work is done (its kill targets, the creatures that drop its items) for a walker of `level`; 0 when
    // none of that lies in the zone. Cached per quest, team and level: the zone pick asks it for every quest it counts.
    uint8 QuestWorkDanger(uint32 zoneId, Quest const* quest, TeamId team, uint8 level) const;
    // The same for one place (a giver, an ender): the strongest hostile creatures on the safest way from the zone's
    // hub to it. Cached per place, team and level.
    uint8 PlaceWayDanger(uint32 zoneId, GuildAdventure::SpawnPoint const& spot, TeamId team, uint8 level) const;
    // A way from the zone's hub to a place as the walk gate would judge it for a walker of a level: the safest way
    // (its danger and the biggest group on it) and the way around every group (its danger and the group or camp it
    // cannot avoid). The bot's own verdict on those groups (Risk::CanFight) is asked per bot: HubWayPasses.
    struct HubWay
    {
        uint8 danger = 0;            // 0: not judged (no hub on the place's map)
        uint8 crowd = 0, crowdLevel = 0;
        bool aroundFound = false;
        uint8 aroundDanger = 0;
        uint8 aroundCrowd = 0, aroundCrowdLevel = 0;
        bool aroundCamp = false;
    };
    HubWay JudgeHubWay(uint32 zoneId, GuildAdventure::SpawnPoint const& spot, TeamId team, uint8 level) const;
    bool HubWayPasses(Player const* player, GuildAdventure::BotRuntime const& rt, HubWay const& way) const;
    // Whether the walk rules let this bot get to a quest's work (the nearest few of its places) or to a place from the
    // zone's hub: the danger and the groups on the way (QuestWorkDanger, PlaceWayDanger with the groups).
    bool QuestWorkReachable(Player const* player, GuildAdventure::BotRuntime const& rt, uint32 zoneId, Quest const* quest) const;
    bool PlaceWayReachable(Player const* player, GuildAdventure::BotRuntime const& rt, uint32 zoneId,
        GuildAdventure::SpawnPoint const& spot) const;
    void CheckOutleveled(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player,
        GuildAdventure::BotRuntime& rt);

    // Planning
    bool PlanUpkeep(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // A rogue's poisons (RoguePoisons.h): the trip to a poison vendor, and the stop in a town of a rogue with bare weapons.
    void StartPoisonTrip(Player* player, GuildAdventure::BotRuntime& rt, GuildAdventure::ServicePoint const& vendor, bool sell, bool repair,
        char const* why);
    bool PlanPoisonStop(Player* player, GuildAdventure::BotRuntime& rt);
    // `within` above 0: only the hand-ins and pickups whose NPC stands that close to the bot (what is done on the way
    // before a trip to a vendor).
    bool PlanQuests(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt, uint32 zoneId,
        float within = 0.0f,
        bool anyKind = false);
    // (`minFoeLevel` and `maxDistance`: the bot at the level cap grinds creatures of a wider band farther away; -1 and 0
    // are the grind settings.)
    bool PlanGrind(Player* player, GuildAdventure::BotRuntime& rt, uint32 zoneId, int32 minFoeLevel = -1, float maxDistance = 0.0f);
    bool IsQuestPickable(GuildAdventure::GuildBot const& bot, Player* player, Quest const* quest) const;
    // A walk of a task (to a quest giver, an ender, a flight master, the spot of an objective) that leads past
    // creatures more than `margin` levels above the bot is not walked: the target is left alone for a while and the
    // task ends (true), so the planner takes something else first. Looked at once per task, when its way is known.
    // `beforeOnly`: the last stretch before the target does not count (an objective's own creatures).
    bool LeaveDangerousWalk(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt, int32 margin,
        bool beforeOnly = false);
    // (The margin of such walks is the setting GuildAdventure.Walks.MaxLevelsAbove.)
    // Where a bot enters a zone it travels to: the nearest giver, in that zone, of a quest it can take there. False
    // when it can take none (the zone's hub is the target then).
    bool ZoneEntry(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime const& rt,
        GuildAdventure::ZoneTemplate const& zone, GuildAdventure::SpawnPoint& entry) const;

    // Execution
    void StartTask(Player* player, GuildAdventure::BotRuntime& rt, GuildAdventure::BotTask task);
    // Drives the current task one step; false when it is over (done or failed) and a new plan is needed.
    bool StepTask(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player,
        GuildAdventure::BotRuntime& rt);
    bool StepNpcTask(Player* player, GuildAdventure::BotRuntime& rt, bool& done);
    // The NPC of the task stands in a room only a teleporter leads into, and the bot is not in that room: it walks to
    // the teleporter and steps in. True while it is on that way.
    bool ThroughLocalPortal(Player* player, GuildAdventure::BotRuntime& rt);
    // A travel refused for a group that stands where the bot is: its way out is planned without that camp, once.
    bool WidenTravelStart(Player* player, GuildAdventure::BotTask& task, char const* what);
    // The bot stands in the middle of a camp it cannot take, or at its edge (within 35 yd of such a middle): its way
    // out is no reason to keep it there.
    bool StandsAtCamp(Player const* player, GuildAdventure::BotRuntime const& rt) const;
    // The legs of a way of the whole navmesh (DungeonNav::FindWay) judged as a straight way is: what lives on them, a
    // group, a camp it cannot take, a place it has set aside. False with `why` when one of them refuses it.
    bool NavWayIsSafe(Player* player, GuildAdventure::BotRuntime& rt, std::vector<GuildAdventure::NavPoint> const& corners,
        std::string& why) const;
    // Before a crossing: can the bot go on from the landing to the zone it sails for? On foot (the road map's way,
    // judged as a travel's), or by a flight from a flight master at the landing to one at the zone. A level-31 bot
    // sailed to Theramore for Thousand Needles, which lies behind a marsh ten levels above it, and stood on the pier.
    bool ReachableFromLanding(Player* player, GuildAdventure::BotRuntime const& rt, GuildAdventure::TravelLink const& link,
        GuildAdventure::ZoneTemplate const& zone) const;
    // A journey waits for a strong creature on its next stretch to pass (Escape::RoamerOnLeg); true: it waits, the
    // task's own step is skipped. `PauseForRoamer` begins such a wait (false: the task has waited long enough).
    bool StepRoamerWait(Player* player, GuildAdventure::BotRuntime& rt);
    bool PauseForRoamer(Player* player, GuildAdventure::BotRuntime& rt, std::string const& who);
    // The bot stands in such a room and its walk leads out of it: the walk's destination becomes the teleporter out
    // (`x`, `y`, `z` are changed). True in the moment it steps into the teleporter.
    bool LeaveLocalRoom(Player* player, GuildAdventure::BotRuntime& rt, float& x, float& y, float& z);
    bool ViaRoadNode(Player* player, GuildAdventure::BotRuntime& rt, float& x, float& y, float& z);
    // Inside a dungeon: the walk's destination becomes a point some yards along the whole navmesh's way there
    // (DungeonNav), so the core's path finder, which stops after 148 polygons, walks it a stretch at a time.
    // True when it moved the bot itself (down a drop).
    bool ViaDungeonPath(Player* player, GuildAdventure::BotRuntime& rt, float& x, float& y, float& z);
    // Where a dungeon way is planned from: where the bot stands, or for half a minute after a drop or swim the landing,
    // when the bot stands within 6 yd of it (the navmesh's part there may lie yards below the floor the bot stands on).
    // True when it is the landing.
    bool NavFrom(Player* player, GuildAdventure::BotRuntime const& rt, float& x, float& y, float& z) const;
    // A character that floats yards above the way's part under the water (a pool's floor) after a drop or swim: the
    // path finder starts on the ground beside the pool and has no way, so it swims straight to the way's next point.
    void SwimAlong(Player* player, GuildAdventure::BotRuntime& rt, float x, float y, float z);
    // A drop or swim whose time is over: a character that stands yards above its landing stopped on something on the
    // way down, and that drop is taken out of the navmesh's ways (DungeonNav::BlockDrop).
    void DropDone(Player* player, GuildAdventure::BotRuntime& rt);
    // While a drop or swim is under way: the bot's AI is held and its straight move to the landing set again when
    // something else moves it (the leader of Blackfathom Deeps swam towards Ghamoo-ra, was back on the ledge 18 s later,
    // and did so for 13 minutes: the drop's hold kept the planner out, not mod-playerbots' AI).
    void KeepDropMoving(Player* player, GuildAdventure::BotRuntime& rt);
    // An object whose linked trap summons what the bot cannot take (Risk::JudgeSummon): the interaction step does not
    // open it, as the planner does not choose its spawn.
    [[nodiscard]] bool TrapRefused(Player* player, GuildAdventure::BotRuntime const& rt, GameObject const* object) const;
    // Down a drop: a straight fall onto the part of the navmesh below, as a player jumps down a ledge.
    void DropDown(Player* player, float x, float y, float z, bool swim = false);
    // A member of a dungeon group walks the navmesh's way to (x, y, z) itself, a stretch at a time (and down a drop on
    // it). False when there is none or it fights.
    bool WalkDungeonWay(Player* player, float x, float y, float z, bool inFight = false);
    // NewRpg's walk to a place inside a dungeon run, along the whole navmesh's way (ViaDungeonPath) when it winds.
    void DungeonGoTo(Player* player, uint32 mapId, float x, float y, float z);
    // The tank of a dungeon group goes for the creature it walks into: its AI fights it once it is near and in its sight
    // (or in a fight); before that the tank walks the navmesh's way to it itself. True when it attacks or walks.
    bool GoForWalkIn(Player* leader, GuildAdventure::BotRuntime& rt, Creature* target);
    // Interact: walk to the spot, find the target, step into range, act, check the progress (QuestInteraction).
    bool StepInteraction(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player,
        GuildAdventure::BotRuntime& rt);
    // An NPC that follows the bot for a quest (a lead event): fetches the quest item that wakes or calls it back when
    // the bot lacks it and its chest is in sight, and uses it on the NPC when it sleeps or stopped following
    // (QuestInteraction::GetFollowerAid). True while it acts: the caller's step returns; false: the lead goes on.
    bool AidFollower(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt, Quest const* quest,
        Creature* follower);
    // Interact, plan Provoke: talk to the creature, lead it to its place if it follows, let the bot fight it when it turns.
    // An item for a weakened creature ("capture a raptor" below 20 % health): the fight down to the line, then the item.
    // Also called in a fight, where the planner does not run the task (true: it acted or waits on the fight).
    bool StepWeaken(Player* player, GuildAdventure::BotRuntime& rt);
    bool StepProvoke(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // Long walks: fly instead when the bot knows a flight path that saves most of the way.
    // `anyWalkShare`: also when the walks to the master and from the landing are most of the way (the way on foot was
    // refused).
    bool TryFlight(Player* player, GuildAdventure::BotRuntime& rt, uint32 destMap, float destX, float destY, float destZ,
        bool anyWalkShare = false);
    // The walk to the task's spot is refused (creatures far above the bot, a group it cannot take) and the spot is far:
    // when a flight leads near it and the walk from the landing is one the bot may take, the task becomes a journey
    // there (a Travel task with `journey`), and the planner plans anew where it arrives. True when it did.
    bool StartJourney(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt, char const** whyNot = nullptr,
        bool farAround = false, bool longWay = false);
    // The flight TryFlight would book, without booking it: from which flight master, over which nodes, at what cost.
    struct FlightPlan
    {
        GuildAdventure::ServicePoint const* master = nullptr;
        std::vector<uint32> route;
        uint32 cost = 0;
        float toMaster = 0.0f;         // walk to the flight master
        float fromLanding = 0.0f;      // walk from the landing spot to the destination
    };
    // `maxWalkDanger`: creatures this many levels above the bot may live on the walk from the landing (-1: the limit of
    // a travel to a zone).
    // `whyNot` gets the reason when no flight is found.
    bool FindFlight(Player* player, GuildAdventure::BotRuntime const& rt, uint32 destMap, float destX, float destY, float destZ,
        FlightPlan& plan, int32 maxWalkDanger = -1, char const** whyNot = nullptr, bool anyWalkShare = false) const;
    // Errands (OrderErrands.cpp): hand in completed quests whose ender is outside the target zone. `all`: every hand-in
    // near is due (the bot is outside its zone); `distant`: also the ones a voyage or a long way off (the zone has no work
    // left); else only a batch of them.
    bool PlanErrand(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player,
        GuildAdventure::BotRuntime& rt, uint32 zoneId, bool all, bool distant = false);
    // The hand-in of a completed quest as an errand: its cheapest ender and what the trip costs (yards of walking, with
    // flights and legs counted in). Nothing when the turn-in is zone work or the bot should not or cannot go there;
    // `whyNot` then says which.
    struct ErrandOption
    {
        uint32 questId = 0;
        GuildAdventure::SpawnPoint spot;
        float cost = 0.0f;
    };
    std::optional<ErrandOption> FindErrand(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime const& rt,
        uint32 zoneId, uint32 questId, std::string* whyNot = nullptr) const;
    // Lines for ".ga bot plan": the completed quests that wait for an errand.
    void DescribeErrands(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime const& rt,
        std::vector<std::string>& lines) const;
    // Gives up the current errand; its quest is not tried again for a while.
    void DropErrand(GuildAdventure::GuildBot const& bot, GuildAdventure::BotRuntime& rt, char const* why);
    bool PlanDiscovery(Player* player, GuildAdventure::BotRuntime& rt);
    // Quest events (OrderEvents.cpp). TrackEventQuests watches the quest log for quests whose event starts when they
    // are taken; true when one just appeared and the running task should end for it. PlanFreshEvent starts the event
    // task of such a quest, StepEvent drives an event task.
    bool TrackEventQuests(Player* player, GuildAdventure::BotRuntime& rt);
    bool PlanFreshEvent(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // A quest in the log whose talk partner stands close by: the talk comes before the bot travels on (the quest planner
    // itself only works inside the target zone).
    bool PlanNearbyTalk(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt);
    bool StepEvent(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player,
        GuildAdventure::BotRuntime& rt);
    // Makes the bot attack what attacks an NPC it protects (an escort, the NPC of an event) or the bot itself. True
    // when it attacks.
    bool DefendWard(Player* player, Creature* ward);
    bool StepVoyage(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player,
        GuildAdventure::BotRuntime& rt);
    void StartVoyage(Player* player, GuildAdventure::BotRuntime& rt, GuildAdventure::TravelLink const& link);
    // Grey and failed quests in the log (docs/orders.md "Grey and failed quests"): a quest that failed is given back
    // (FailQuest: taken again once), a quest that is grey for the character is abandoned (bot setting
    // Quests.AbandonGrey), a finished grey one only when its ender is not close by.
    void CareForStaleQuests(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player,
        GuildAdventure::BotRuntime& rt);
    // Hands a finished quest in at its ender (OrderSquads.cpp). False when the core would not reward it.
    static bool HandInAt(Player* player, Quest const* quest, Object* ender);
    void FailQuest(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player, uint32 questId,
        std::string const& reason);
    // Ends the current task; `outcome` (optional) goes on its line in the activity log.
    void ClearTask(GuildAdventure::BotRuntime& rt, uint32 waitMs = 0, char const* outcome = nullptr);
    // A bot whose ways are refused again and again where it stands (BoxedRefusals within BoxedYards and BoxedWindowMs:
    // every route from there begins in a camp, past creatures far above it) goes home by hearthstone and plans from its
    // inn. True when it did.
    bool LeaveBoxedPlace(Player* player, GuildAdventure::BotRuntime& rt);
    // A bot on the profession mode leaves the squad it is in (a member would follow its leader and craft nothing, a
    // leader would keep its members waiting); not a dungeon group.
    void LeaveSquadForCrafting(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot);
    // A tool its orange and yellow recipes lack that another profession makes: taken out of the guild storage, or
    // ordered from the guild's crafters (CraftOrders), once in ten minutes.
    void SeeToLackedTool(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot, Player* player,
        GuildAdventure::BotRuntime& rt, uint32 skill);
    // Activity log: one status line per bot every few minutes (level, zone, position, how far it moved, task, why).
    void LogStatus(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // An NPC task ended with the NPC in sight but without the quest taken or turned in: what the bot and the NPC looked
    // like at that moment, for the activity log (so the cause can be found afterwards).
    std::string DescribeNpcFailure(Player* player, GuildAdventure::BotRuntime const& rt, Quest const* quest) const;
    // A quest NPC that was in sight did not give or take the quest: a short break and a failure (A21), except in the
    // bot's first minute in the world, where it only tries again.
    void OnQuestNpcFailed(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player,
        GuildAdventure::BotRuntime& rt, char const* reason);
    // The bot just died: count it against the task and set the quest or grind spot aside after repeated deaths.
    void OnDied(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player,
        GuildAdventure::BotRuntime& rt);
    // A ghost that should not walk back to its corpse: to the spirit healer at its graveyard.
    void StepSpiritRes(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // True when the bot is stuck on its way to (targetX, targetY), see BotRuntime's stuck tracker.
    bool UpdateStuck(Player* player, GuildAdventure::BotRuntime& rt, float targetX, float targetY);
    // Walks the bot towards a spot of its task: along a route of the road map when the way is long, else straight
    // (OrderRoutes.cpp). Call every tick while the task walks.
    void WalkTo(Player* player, GuildAdventure::BotRuntime& rt, uint32 mapId, float x, float y, float z);
    // How far the walk to a spot is: along the road map's route, or the path finder's whole path; -1 when neither has a
    // way (a spot behind a mountain range). For choices between places to walk to (OrderRoutes.cpp).
    float WalkLength(Player* player, uint32 mapId, float x, float y, float z) const;
    // Rescue: the bot is under the ground, or has not moved for minutes although its task wants it to walk (it stands
    // off the navmesh, on a peak or a roof). It is put back at the nearest node of the road map, or uses its hearthstone.
    // True when it was moved.
    bool CheckStranded(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // The rescue itself: back on the ground at the nearest node of the road map at least `minDistance` away, or, right
    // after such a rescue or with no node in reach, the hearthstone. False when neither is possible.
    bool Rescue(Player* player, GuildAdventure::BotRuntime& rt, char const* what, float minDistance);
    // A bot cut off from every road where it stands: the way it came first (StartTrailOut), what led it there set aside,
    // the rescue only when it has no trail to walk back.
    bool CutOffWayOut(Player* player, GuildAdventure::BotRuntime& rt, char const* what);
    // A bot cut off from every road on a patch of walkable ground the path finder joins to nothing (a rock top a fight
    // carried it onto): one straight step down off its edge onto ground from which the path finder walks to a road, the
    // step probed for the ground first. False when it has no such step.
    bool StepOffLedge(Player* player, GuildAdventure::BotRuntime& rt, std::string const& why);
    // A bot a few yards inside the ground (under the terrain, no floor under it) is put on the ground right above it.
    bool LiftOutOfGround(Player* player, GuildAdventure::BotRuntime& rt, std::string const& why);
    // True when the bot stands where the navmesh does not reach (a slope it slid onto in a fight, a roof, a ledge): the
    // core then has only straight lines for it, to the two nearest nodes of the road map as well.
    bool IsOffNavmesh(Player* player) const;
    // The bot stands on the path finder's ground: a short path from its feet starts on a polygon of the navmesh.
    bool StandsOnNavmesh(Player* player) const;
    // Quest items from the guild (QuestSupply.cpp): what an open quest of the bot asks for comes out of the guild
    // storage, from another guild character that has it to spare, or is crafted by one that knows a recipe.
    void SupplyQuestItems(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player,
        GuildAdventure::BotRuntime& rt);
    // The profession mode (order CRAFT, docs/orders.md "Profession mode"): the bot works on the professions of its plan
    // until they reach their targets: it crafts what gives skill from its bags and the guild storage, and gathers for
    // its gathering professions on a round over the places where nodes its skill opens spawn.
    void RunProfessions(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player,
        GuildAdventure::BotRuntime& rt);
    bool PlanGatherRound(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // No safe way from where it stands (a travel or the walk to a dock was refused for what stands on it, and no
    // flight was to be had): the character uses its hearthstone when it has one, it is ready and its inn is not next
    // door, and plans again from there. False when it cannot.
    // (`minYards`: an inn nearer than this is no way out of anything; less for a bot that is stranded where it stands.)
    bool HearthHome(Player* player, GuildAdventure::BotRuntime& rt, std::string const& reason, float minYards = 1500.0f);
    // The hearthstone as the way to a place: when the bot's inn stands near that place and far from the bot.
    bool HearthTowards(Player* player, GuildAdventure::BotRuntime& rt, uint32 mapId, float x, float y, std::string const& reason);
    // Whether the hearthstone may shorten a travel (bot setting Travel.Hearthstone): its inn on another continent or in
    // a capital of its side, or always, or never. Emergencies do not ask.
    [[nodiscard]] bool TravelHearthAllowed(Player const* player) const;
    // The ground guard refused a step towards the task's target, and the target stands on another level that the path
    // finder has no way to from where the bot stands: the target is set aside at once (not after three refusals), for
    // hours and across a restart when no node of the road map near it leads there either. True if it did.
    // The first refused step of a straight walk with no way at all: the walk's path is asked again from where the bot
    // stands (a short walk), or the planner walks it along the path finder's way itself at once. True if it did.
    bool RetryRefusedWalk(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt, bool noWay);
    bool SetAsideOtherLevel(GuildAdventure::AdventureGuild const& guild, GuildAdventure::GuildBot const& bot, Player* player,
        GuildAdventure::BotRuntime& rt, uint32 mapId, float x, float y);
    // AUTO (the owner, 2026-11-13: [quote left out]): between two tasks in the zone it quests in, a planned gathering skill (Mining, Herbalism, Skinning)
    // that is below everything the zone has starts a catch-up: the profession mode's gathering (rounds over nodes its
    // skill opens, a hunt for skins; no crafting) until the skill reaches the zone's lowest node. True while it
    // catches up (the caller does not quest then). Bot setting Professions.CatchUpGathering.
    bool CatchUpGathering(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player,
        GuildAdventure::BotRuntime& rt);
    // Whether the risk model lets the bot work at a place that is no spawn of a creature (a node, a corpse): the spot
    // verdict (Risk::JudgeSpot) for the place.
    bool IsPlaceUsable(Player const* player, GuildAdventure::BotRuntime const& rt, uint32 mapId, float x, float y, float z) const;
    // Fishing (Fishing.cpp, docs/orders.md "Fishing"): the task that walks to the water, casts and takes the catch; a
    // fishing trip for the skill (profession mode); the weapon back into the hand after it.
    bool StepFishing(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // One stretch of the core's own path towards a point, moved by the planner (NewRpg rests meanwhile). False when
    // the path finder has no way that leads on from where the bot stands.
    bool OwnStep(Player* player, GuildAdventure::BotTask& task, uint32 mapId, float x, float y, float z);
    // A target across water that no way on the ground leads to (a ship's deck off the coast): the bot swims to a point
    // in the water from which the path finder leads to the target, along a way through the water (a search over the
    // water around). True while it swims; once at the landing the walk goes on as usual.
    bool StepSwim(Player* player, GuildAdventure::BotTask& task, float x, float y, float z);
    // A bot on a floor under water that the navmesh keeps apart from the water over it (the Undercity's canals): the
    // core keeps a swimmer near that floor and its paths start there, where none leads out. It swims out along the
    // path from the water's surface, to the shore and a few yards beyond. True while it does.
    bool WadeOut(Player* player, GuildAdventure::BotTask& task, float x, float y, float z, char const* why);
    // A ride on a lift of the bot's route (RoadRoute::lifts): it waits at the stop until the platform is there, steps
    // onto it as its passenger, and leaves it at the other stop. True while it waits or rides.
    bool StepLift(Player* player, GuildAdventure::BotRuntime& rt);
    // The walkable place at a lift's stop beside its shaft, at the stop's height and with a way from the bot, cached in
    // the task (BotTask::liftLand...). False when there is none within 14 yd.
    bool LiftLanding(Player* player, GuildAdventure::BotTask& task, float stopX, float stopY, float stopZ, bool quiet = false);
    // A passenger of a lift (a static transport) without a ride of its own: it entered the world on the platform after
    // a restart, followed its leader onto it, or the task of its ride ended during the ride. It gets off at the
    // platform's next stop onto the landing beside it (a passenger far from its platform at once). True while it rides.
    bool StepStrayLiftPassenger(Player* player, GuildAdventure::BotRuntime& rt, Transport* platform);
    // A meeting point for a squad member far behind with no way of its own to its leader: the flight master of its side
    // nearest to the leader (UpdateSquad, StepRejoinTravel). False when there is none worth it.
    bool ChooseMeetingPoint(Player* player, GuildAdventure::BotRuntime& rt, Player* leader);
    // Whether the straight way from the bot to a point passes what the walk gate lets through: nothing more levels above it
    // than Walks.MaxLevelsAbove, no group it cannot take, no place it has set aside, no camp (beforeOnly: the last stretch
    // is the spot's own matter).
    bool StraightWayIsSafe(Player* player, GuildAdventure::BotRuntime& rt, float x, float y, float z, bool beforeOnly);
    // The core's path finder asked hop by hop along the straight line from the bot to a point on its map (it cannot
    // look farther than a few hundred yards at once): true when every hop has a way, with the length of the whole.
    bool ChainReaches(Player* player, float x, float y, float z, float& length) const;
    // The path to the target ends short of it, and a hand-listed crossing (ga_path_bridge) leads on: the bot's
    // ground reaches its one point and the target is reached from its other. The own steps lead to the first and
    // straight to the second. False: no such crossing here.
    bool BridgeGap(Player* player, GuildAdventure::BotTask& task, float x, float y, float z);
    bool PlanFishing(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt, uint32 nearCasts = 0);
    void RestoreWeapon(Player* player, GuildAdventure::BotRuntime& rt);
    // What a bot in profession mode gathered and made goes into the guild storage (materials, and the products of
    // recipes it does not use itself). Returns the stacks moved.
    uint32 StoreProfessionGoods(GuildAdventure::GuildBot const& bot, Player* player);
    // The order FARM (OrderFarm.cpp, docs/orders.md "Farm orders"): the bot hunts the creatures that drop the ordered
    // item, at a hunting ground Farming::FindGrounds picks, on and on. PlanFarm starts the walk to a ground or the hunt
    // at it; with nothing to farm it tells the leader (once) and the bot waits at an inn. StepFarmLoot walks a hunting
    // bot to the corpse of its kill before it takes the next target (true while it walks).
    void RunFarm(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player, GuildAdventure::BotRuntime& rt);
    bool PlanFarm(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player, GuildAdventure::BotRuntime& rt);
    bool StepFarmLoot(Player* player, GuildAdventure::BotRuntime& rt);
    // Dungeon runs (OrderDungeons.cpp, docs/orders.md "Dungeon runs").
    void RunDungeon(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // How a dungeon group fights (docs/orders.md "How a dungeon group fights"), every tick of its leader inside the
    // dungeon: the others hold back while the tank pulls and takes the creatures; everybody turns to a creature
    // that runs away. And the tank's careful pull of a group ahead (true: one began).
    void StepDungeonFight(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // The acceptance numbers of a run (BotRuntime::DungeonRun::Metrics), every tick of its leader inside; and their line
    // for the end of the run.
    void StepDungeonMetrics(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot, Player* leader,
        GuildAdventure::BotRuntime& rt, GuildAdventure::DungeonPlan const& plan, std::vector<Player*> const& group, Player* guest);
    void StepDungeonMetricsTick(Player* leader, GuildAdventure::BotRuntime& rt);
    [[nodiscard]] static std::string DescribeDungeonMetrics(GuildAdventure::BotRuntime::DungeonRun::Metrics const& metrics);
    // The group into its leader's instance of the dungeon: the members that are still to go in, and anybody of the
    // group who stands in another instance of the map (out, then in).
    void StepDungeonEnter(GuildAdventure::GuildBot const& bot, Player* leader, GuildAdventure::BotRuntime::DungeonRun& run,
        GuildAdventure::DungeonPlan const& plan, std::vector<Player*> const& group, Player* guest);
    // The raid group of a raid run (docs/orders.md "Raid runs"): the leader's party a raid group (the core lets nobody
    // onto a raid map without one), the main tank flag on the first tank (the leader when it tanks), the assistant flag
    // on the other tanks (mod-playerbots' main and assist tanks). Returns whether the leader leads a raid group now.
    bool KeepRaidGroup(GuildAdventure::AdventureGuild& guild, Player* leader, std::vector<Player*> const& group, bool force);
    // Whether this character leads a run into a raid (its order is DUNGEON with a raid's plan).
    [[nodiscard]] bool LeadsRaidRun(Player const* player) const;
    // The last hit a player took inside a dungeon or raid within 10 s, as ", the last hit N from X (spell)" (empty without one).
    [[nodiscard]] static std::string DescribeLastHit(Player const* player);
    // Everybody but its tank out of the fire of a creature that bursts at its death (Garr's Firesworn).
    void StepRaidBursts(GuildAdventure::BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& raid);
    bool StepTwinsPull(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot, Player* leader,
        GuildAdventure::BotRuntime& rt,
        std::vector<Player*> const& group, Creature* nilash);
    bool StepTwinGuards(GuildAdventure::GuildBot const& bot, Player* leader, GuildAdventure::BotRuntime& rt);
    void StepRaidTwins(GuildAdventure::BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& raid);
    void StepDungeonFlames(GuildAdventure::BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& dungeon);
    void StepWayTriggers(GuildAdventure::BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& dungeon);
    void StepDungeonPulsers(GuildAdventure::BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& dungeon);
    // An assist tank taunts a boss whose stacks of a wound weaken its tank too much (Kurinnaxx's Mortal Wound).
    void StepRaidTankSwap(GuildAdventure::BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& raid);
    // Ossirian: his tank takes him to the next crystal before his Strength comes back.
    void StepRaidOssirian(GuildAdventure::BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& raid);
    // Hakkar: a Son of Hakkar killed before each Blood Siphon and everybody into its poison cloud.
    void StepRaidHakkar(GuildAdventure::BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& raid);
    void StepRaidViscidus(GuildAdventure::BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& raid);
    void StepRaidCthun(GuildAdventure::BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& raid);
    // C'Thun's Eye pulled by a hunter while the raid waits out of its sight (the owner's guide of 2026-11-24).
    bool StepCthunPull(GuildAdventure::BotRuntime& rt, std::vector<Player*> const& group, Creature* eye, std::string const& raid);
    // Razorgore: the Orb of Domination used, the eggs broken by him, no pull of him while eggs are left.
    void StepRaidRazorgore(GuildAdventure::BotRuntime::DungeonRun& run, std::vector<Player*> const& group,
        GuildAdventure::DungeonPlan const& plan, Player* leader);
    [[nodiscard]] static bool RazorgoreEggsLeft(Creature const* razorgore);
public:
    // Whether a member stands in the same instance of a map as its leader.
    static bool SameInstance(Player const* member, Player const* leader);
    // Binds a member to the dungeon instance its leader stands in, so that its next way into that map leads there
    // (the core sends a character into the instance it is bound to, else into its party leader's, else a new one).
    // False when it cannot be done (the leader is in no dungeon, or the member has a lockout of its own there).
    static bool BindToLeaderInstance(Player* member, Player* leader);
private:
    // One target for all (package P5): the skull on the healer's attacker, else on the tank's target.
    void StepDungeonSkull(GuildAdventure::AdventureGuild& guild, GuildAdventure::BotRuntime::DungeonRun& run,
        std::vector<Player*> const& group, Player* tank, std::vector<Creature*> const& fight);
    // A member under attack: the tank goes to it (the healer first) and the damage dealers turn to its foes.
    void StepDungeonRescue(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot, Player* player,
        GuildAdventure::BotRuntime::DungeonRun& run, std::vector<Player*> const& group, Player* tank, bool pulling, bool held, uint32 now);
    // The way to the next boss in sections (package P3): the next stretch of at most 35 yd along the path finder's way
    // (goX, goY, goZ), and what stands on it within the reach of the group's lowest member plus a margin: a small pack is
    // walked into by the tank, a big one, a pack with casters, or any after a wipe or the tank's death is pulled to a
    // camp 25 yd back on the cleared way. True: the step is handled (a pull, a gate, an attack).
    bool StepDungeonWay(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot, Player* leader,
        GuildAdventure::BotRuntime& rt, GuildAdventure::DungeonPlan const& plan, std::vector<Player*> const& group, Player* guest,
        float toX, float toY, float toZ, float& goX, float& goY, float& goZ);
    // The gate before a boss or a big pull (package P1): the whole group at the tank (12 yd), nobody in a fight, health
    // and mana up to the owner's thresholds by role; members short of them are asked to rest, members that do not come are
    // walked to the tank and at last put beside it. True: it is open. False: the group gathers (the tank stands).
    // The conductor of a dungeon group (package P2), four times a second for the bot that leads a run inside: the
    // tank's way, and the members walking it behind the tank at their places while it walks out of any fight.
    void StepDungeonConductor(Player* leader, GuildAdventure::BotRuntime& rt);
    // The healer of a dungeon group (package P6a), four times a second: no damage spells in a dungeon (its wand while
    // nobody needs a heal), and healing through while the fight is heavy.
    void StepDungeonHealer(Player* leader, GuildAdventure::BotRuntime& rt);
    // The spread limit (package P2): true while the tank stands for a member farther than 15 yd from it.
    // The group rests together (package P6b): when somebody is below the owner's trigger (health 50 %, a healer's mana 40 %,
    // a damage dealer's 20 %), all of it rests up; true while it does (the tank stands).
    // The buff plan of a dungeon group (package P8): who gives which buff (a log line once per run), a second
    // shaman's totems apart from the first's, and the blessing the group leaves to a paladin its owner plays by hand
    // named to him once.
    // A big pull of the run window's mode "big pulls": the tank gathers the creatures of a cluster and brings them to its
    // group (OrderDungeons.cpp).
    void StepDungeonGather(GuildAdventure::GuildBot const& bot, Player* leader, GuildAdventure::BotRuntime& rt, GuildAdventure::DungeonPlan const& plan,
        DungeonFightRules const& rules);
    void StepDungeonBuffPlan(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot, Player* leader,
        GuildAdventure::BotRuntime& rt, GuildAdventure::DungeonPlan const& plan, std::vector<Player*> const& group, Player* guest);
    bool StepDungeonGroupRest(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot, Player* leader,
        GuildAdventure::BotRuntime& rt, GuildAdventure::DungeonPlan const& plan, std::vector<Player*> const& group);
    bool StepDungeonSpread(GuildAdventure::GuildBot const& bot, Player* leader, GuildAdventure::BotRuntime& rt,
        GuildAdventure::DungeonPlan const& plan, std::vector<Player*> const& group);
    // The bosses of a dungeon in the order of the way (package P4): from its entrance along the path finder's way, the
    // nearest next, a stop that must come before a boss with it, the boss that ends it last. Once per dungeon.
    std::vector<uint8> BossOrder(Player* leader, GuildAdventure::DungeonPlan const& plan);
    std::unordered_map<uint32, std::vector<uint8>> _bossOrders;
public:
    // The run's `at`th boss, and how many it has (the order of the way once it is inside).
    [[nodiscard]] static GuildAdventure::DungeonBoss const& RunBoss(GuildAdventure::BotRuntime::DungeonRun const& run,
        GuildAdventure::DungeonPlan const& plan, std::size_t at);
    [[nodiscard]] static std::size_t RunBossCount(GuildAdventure::BotRuntime::DungeonRun const& run, GuildAdventure::DungeonPlan const& plan);
private:
    bool DungeonGate(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot, Player* leader,
        GuildAdventure::BotRuntime& rt, GuildAdventure::DungeonPlan const& plan, std::vector<Player*> const& group, Player* guest,
        std::string const& what, uint32 key, bool big = true, bool counted = true);
    // No way on foot to a boss: walkable ground about 25 yd before it, on the side the group comes from, with the
    // fewest creatures around. False: none found. `nearRadius` / `farRadius`: the two rings looked at (also the place
    // a boss's room is cleared from, out of the boss's reach). `clear`: only a spot no hostile creature that is in no
    // fight would see the group at (its reach and 8 yd), on rings out to farRadius + 30 yd.
    bool FindHopSpot(Player* player, float bossX, float bossY, float bossZ, float& x, float& y, float& z, float nearRadius = 25.0f,
        float farRadius = 35.0f, bool clear = false) const;
    bool StepThroughDoor(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt, std::vector<Player*> const& group,
        GuildAdventure::DungeonPlan const& plan, float toX, float toY, float toZ, std::string const& toName);
    void EndDungeonRun(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player, GuildAdventure::BotRuntime& rt,
        std::string const& how, bool done);
    // A dead character of a dungeon group: brought back by the group, or the wipe. True when it belongs to a run.
    bool StepDungeonDead(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player, GuildAdventure::BotRuntime& rt);
    std::vector<Player*> DungeonGroup(GuildAdventure::AdventureGuild& guild, ObjectGuid leader) const;
    // The member of a dungeon group that its owner plays by hand right now (one client per owner: at most one);
    // nullptr when the AI plays them all. It is no part of DungeonGroup (docs/orders.md "The guild leader in a
    // dungeon run").
    Player* DungeonGuest(ObjectGuid leader) const;
    // The group stays with that character: it waits while it is behind, rests, fights or is dead (as long as a squad
    // waits for a member), and offers it a resurrection. True: the leader waits this tick.
    bool StepDungeonGuest(GuildAdventure::GuildBot& bot, Player* player, GuildAdventure::BotRuntime& rt, GuildAdventure::DungeonPlan const& plan,
        Player* guest, std::vector<Player*> const& group);
    // A teleport of the group inside its dungeon (no way on foot, beside an event's NPC): the owner's character goes
    // along when it stands with the group (GuildAdventure.Dungeons.TakeOwnerAlong).
    void TakeGuestAlong(GuildAdventure::GuildBot const& leader, Player* leaderPlayer, float x, float y, float z, std::string const& why);
    void LoadDungeonRuns();
    // Between two runs of one order, at the entrance: repairs (the character's own money) and room in the bags.
    void DungeonUpkeep(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& leader, std::vector<Player*> const& group);
    // A closed door next to the leader inside a dungeon is opened (GuildAdventure.Dungeons.OpenDoors). `tribute`: not
    // the doors a tribute run opens with their key (OrderTribute.cpp).
    void OpenDoorsInTheWay(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt, std::string const& dungeon,
        bool tribute = false);
    // Dire Maul North's tribute run (the owner, 2026-11-21; docs/orders.md "The Dire Maul tribute run", OrderTribute.cpp).
    static constexpr uint32 TributeDungeonId = 38;
    // At the start (StartDungeonGroup): what the guild storage lacks for it, with craft orders and shopping list entries for
    // that; empty when it can go.
    std::string CheckTributeStart(GuildAdventure::AdventureGuild& guild, std::vector<ObjectGuid> const& members);
    // A named ogre the tribute run leaves alive: nobody of the group attacks it, its way keeps out of its reach.
    [[nodiscard]] static bool IsTributeSpared(GuildAdventure::BotRuntime::DungeonRun const& run, Creature const* creature);
    // Once the group is inside: how the inner door is opened, and the run's bosses (King Gordok; Guard Mol'dar first
    // when nobody of the group can open the inner door without his key).
    void BeginTribute(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot, GuildAdventure::DungeonPlan const& plan,
        GuildAdventure::BotRuntime::DungeonRun& run, std::vector<Player*> const& group);
    // Before King Gordok: the key, the doors, the trap, the suit, Kromcrush. True: it acted (the caller returns).
    bool StepTribute(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* leader, GuildAdventure::BotRuntime& rt,
        GuildAdventure::DungeonPlan const& plan, std::vector<Player*> const& group, Player* guest);
    // After King Gordok: Mizzle, the tribute, the guards' buffs. True: it acted (the caller returns).
    bool StepTributeEnd(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* leader, GuildAdventure::BotRuntime& rt,
        GuildAdventure::DungeonPlan const& plan, std::vector<Player*> const& group, Player* guest);
    // In a fight of a tribute run: no member keeps a spared ogre as its target, and an ogre that came at the group in the
    // open is fought after all.
    void StepTributeFight(GuildAdventure::BotRuntime::DungeonRun& run, std::vector<Player*> const& group, std::string const& leaderName,
        std::string const& dungeon);
    // True while the bot belongs to a dungeon order: its own (it leads) or its squad leader's.
    bool IsOnDungeonRun(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot);
    // A dungeon order of a squad whose leader does not tank while a member does: the tank leads the run (the squad
    // becomes a group of the run window, StartDungeonGroup). True: done, the order is the tank's now.
    // A boss that an event brings: the leader walks to the NPC, picks its gossip option, the group stays with the NPC
    // until the boss comes, and kills it. Approach: not there yet (the caller walks on); Busy: the event goes on;
    // Done / GivenUp: on to the next boss.
    enum class EventStep : uint8 { Approach, Busy, Done, GivenUp };
    EventStep StepDungeonEvent(GuildAdventure::GuildBot& bot, Player* player, GuildAdventure::BotRuntime& rt, GuildAdventure::DungeonPlan const& plan,
        GuildAdventure::DungeonBoss const& boss, std::vector<Player*> const& group, bool groupBusy);
    // An event stop at a game object that brings its boss (DungeonBoss::summonObject): used again whenever it can be,
    // until the boss is there.
    EventStep StepObjectEvent(GuildAdventure::GuildBot& bot, Player* player, GuildAdventure::BotRuntime& rt, GuildAdventure::DungeonPlan const& plan,
        GuildAdventure::DungeonBoss const& boss, std::vector<Player*> const& group, bool groupBusy, Creature* creature);
    bool PutTankInFront(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player, std::vector<Player*> const& group);
    std::unordered_set<ObjectGuid::LowType> _tankFrontNoted;
    struct DungeonRuns
    {
        uint8 left = 1;            // DungeonRunsEndless: until the guild leader stops it
        uint8 done = 0;
    };
    std::unordered_map<ObjectGuid::LowType, DungeonRuns> _dungeonRuns;
    bool _dungeonRunsLoaded = false;
    // A dungeon group the guild leader put together in the addon's run window (ga_dungeon_group, _member): what its
    // bots were before, to give it back when the order is over.
    struct DungeonGroupState
    {
        struct Link
        {
            ObjectGuid::LowType member = 0;
            ObjectGuid::LowType prevLeader = 0;  // the squad leader it followed before (0 = none)
            bool inGroup = false;                // false: a follower of one of the group's bots that stays outside
            bool lentOrder = false;              // the guild leader's own character, which had no order: it got one for the run
        };
        uint32 owner = 0;
        uint32 dungeon = 0;
        std::string prevOrder;                   // the leader's order before (token) ...
        uint32 prevZone = 0;                     // ... and its zone
        std::vector<Link> links;
        DungeonRunOptions options;               // the preparation step's choices
    };
    std::unordered_map<ObjectGuid::LowType, DungeonGroupState> _dungeonGroups;
    bool _dungeonGroupsLoaded = false;
    void LoadDungeonGroups();
    // ga_guild_dungeon: owner account -> dungeon -> finished runs.
    std::unordered_map<uint32, std::unordered_map<uint32, uint32>> _dungeonsFinished;
    bool _dungeonsFinishedLoaded = false;
    void LoadDungeonsFinished();
    // Gives the bots of a dungeon group back what they were (squads; with `restoreOrder` the leader's order too).
    // False: the bot leads no such group.
    bool DisbandDungeonGroup(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& leader, bool restoreOrder);
    GuildAdventure::BotRuntime* FindRuntime(ObjectGuid guid)
    {
        auto itr = _runtime.find(guid);
        return itr != _runtime.end() ? &itr->second : nullptr;
    }
    // Room in the bags at a vendor (docs/orders.md "Live round 39"): what mod-playerbots sells there is junk; a bot whose
    // bags are full of other things stays full. Its bags go through the guild's loot filter (which sells what is junk
    // by the junk rule), crafting materials go into the guild storage (a town trip, like a walk to the bank), then
    // what else it does not use itself, until a few slots are free. The planner sells nothing of its own judgement.
    // Returns the number of stacks that went into the storage.
    uint32 MakeBagSpace(GuildAdventure::GuildBot const& bot, Player* player);
    // Out of a camp (docs/orders.md "Live round 39"): the bot has nothing it can do from where it stands, and the
    // risk model would not let it work there: it walks to the nearest road map node that is a safe place.
    bool PlanRetreat(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // The bot swims or dives at the place its task is about (a quest's clams on the sea floor, creatures in a lake):
    // it is where it wants to be. Under water the path finder has no way to any road, which is not being cut off.
    [[nodiscard]] bool IsAtWaterTarget(Player* player, GuildAdventure::BotRuntime const& rt) const;
    // Crafting and enchanting take a bot's eyes off the world for up to half a minute: only where nothing hostile
    // lives or stands within 30 yd, whatever its level (a level-22 priest enchanted its bracers on Westfall's murloc
    // shore, nine yards from a murloc, and died). `hostile` gets how many there are.
    [[nodiscard]] bool IsQuietToCraft(Player* player, uint32* hostile = nullptr) const;
    // Profession mode with nothing else to do at a place that is not quiet: a walk to the nearest quiet node of the
    // road map, to craft there.
    bool PlanQuietPlace(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // A stepping stone (docs/orders.md "Live round 38"): the path finder has no way from where the bot stands to where
    // its task sends it, but it has ways to road map nodes around. The bot walks about 60 yd along the path to the
    // node that takes it nearest to (towardX, towardY) first, and the task goes on from there; up to three per task.
    // StepSteppingStone keeps it walking (true: the task's own step waits).
    bool TakeSteppingStone(Player* player, GuildAdventure::BotRuntime& rt, float towardX, float towardY);
    bool StepSteppingStone(Player* player, GuildAdventure::BotRuntime& rt);
    // Gear for the guild (package K5, docs/orders.md "Gear made for the guild"): at a hub, or in the profession mode,
    // a bot crafts a piece of equipment one of the guild's AI-played characters would wear. Returns true if a cast
    // started. FinishGearCraft hands the piece on once the queue has ended: put on by the bot itself, or into the
    // guild storage, where its wearer takes it (StorageSupply).
    // What the piece lacks comes first (Professions::CraftGearFor): a material the bot makes itself, a purchase at a
    // vendor near the hub, and in the profession mode the walk to an anvil or forge.
    bool CraftGearForGuild(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt,
        bool professionMode = false);
    void FinishGearCraft(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // A hunt for skins: the nearest beasts the bot's skinning skill is enough for and still learns from, and that
    // the risk rules let it fight.
    bool PlanSkinHunt(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // The walk to the nearest anvil, forge or fire a recipe is made at.
    // One look at the professions of a bot's plan, in the order of their turns (the one after the last batch's
    // first). Each has its own look: what it can make it makes (Crafting), and what its best recipe lacks is fetched
    // for it, whatever another profession could make meanwhile: pigments milled and dust disenchanted out of the guild
    // storage, a vendor's material bought (Shopping), in the profession mode also a walk to an anvil, a forge or a fire
    // (Walking). None: no profession has anything to do as things are. A material its best recipe lacks that it makes
    // itself (bars out of ore) is made first (Crafting). `onlySkill`: that profession alone (the profession mode's focus
    // on a profession far behind).
    enum class ProfessionWork : uint8 { None, Crafting, Shopping, Walking };
    ProfessionWork WorkOnProfessions(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot, Player* player,
        GuildAdventure::BotRuntime& rt, uint32 batch, bool professionMode, uint32 onlySkill = 0);
    // Profession mode: a hunt for a material a recipe lacks and creatures drop (cloth, meat), at a hunting ground
    // (FindHuntGround); a journey first when it is far.
    bool PlanMaterialHunt(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt, uint32 item, uint32 count);
    // `reach` (yards, 0 = anywhere in the region): the hub's crafting walks to a place next to the hub only.
    bool PlanFocusTrip(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt, uint32 focus, float reach = 0.0f);
    // The trip to a vendor for a material of a recipe (within the guild's budget).
    bool PlanMaterialPurchase(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt, uint32 item,
        uint32 count, float reach = 0.0f, char const* why = "Profession mode");
    // A mining or herb node close to the way of a walking bot is fetched (GuildAdventure.Professions.GatherDetour): the
    // bot walks to it and auto loot gathers it; a corpse it can skin likewise. True while the detour runs.
    bool GatherDetour(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // What a grinding bot fights next: the nearest creature around its spot that it may attack, of a level between
    // `minLevel` and `maxLevel`, no elite, nobody else's, and without a stronger one standing next to it.
    // Nor one whose company (the hostile creatures next to it) the risk rules do not let the bot take, nor one at a
    // place it keeps away from.
    // With `skins` only beasts its skinning skill is enough for.
    // With `dropsItem` only creatures that drop that item (Farming.h); `around` (above 0): how far from the spot.
    Unit* PickGrindTarget(Player* player, GuildAdventure::BotRuntime const& rt, GuildAdventure::SpawnPoint const& spot, uint8 minLevel,
        uint8 maxLevel, bool skins = false, uint32 dropsItem = 0, float around = 0.0f) const;
    // Sends a diving bot straight up to the surface (NewRpg rests meanwhile). False when it is in no water.
    bool SwimToSurface(Player* player) const;
    // True when the path finder walks from the bot to none of the three nearest road map nodes: it stands in a pocket of
    // the navmesh (a ledge, a mountain top) that no travel leaves.
    bool IsCutOff(Player* player) const;
    // A spot a bot is about to be put at (a pier, a rescue's node): moved onto the navmesh when it lies off it (the top
    // platform of a zeppelin tower), so that the bot can walk on from there.
    void SnapToNavmesh(Player* player, uint32 mapId, float& x, float& y, float& z) const;
    // A diving bot comes up for air before its breath runs out. True while it is on its way up or breathing (the
    // planner does nothing else meanwhile).
    bool CheckBreath(Player* player, GuildAdventure::BotRuntime& rt);
    // Whether a gathering node lies among creatures four or more levels above the bot (60 yd; spawns and patrol ways).
    [[nodiscard]] bool StrongNearNode(Player const* player, uint32 mapId, float x, float y, float z) const;
    static void TrackBreath(Player* player, GuildAdventure::BotRuntime& rt);
    // The breath the game gives this character under water, in milliseconds, as Player::getMaxTimer counts it: the
    // server's WaterBreath.Timer times its auras that lengthen it (the undead's Underwater Breathing), 0 when it breathes
    // under water (a Water Breathing aura, the guild perk Gills: CP-8's hook).
    static uint32 BreathMs(Player* player);
    // A form to travel in is none to fight in: an AI-played character that is attacked in aquatic form, travel form
    // or as a ghost wolf leaves the form (playerbots puts a druid under water into aquatic form and its combat
    // strategies do not take it out again). Not while it runs from a fight: there the form is the faster way out.
    void LeaveTravelForm(Player* player, GuildAdventure::BotRuntime& rt);
    // A character with a healing spell heals itself in a fight when it is low (GuildAdventure.Fight.SelfHealBelow).
    void StepSelfHeal(Player* player, GuildAdventure::BotRuntime& rt);
    // A healing or mana potion in a fight when the character is low (GuildAdventure.Fight.PotionBelowHealth,
    // .PotionBelowMana): the best it carries or its guild's storage has, one piece (StorageSupply::TakeOne).
    void StepPotion(Player* player, GuildAdventure::BotRuntime& rt);
    // A priest, mage or warlock with a wand shoots it where a filler spell would burn mana for little.
    void StepWand(Player* player, GuildAdventure::BotRuntime& rt);
    // A hunter's Auto Shot that outlived its fight is ended, as a player's client does when the target dies: the core
    // never ends Auto Shot by itself (only a wand's Shoot), and every "is it casting?" counts it, so a resting hunter
    // neither ate nor drank (a level-43 hunter sat 155 s at 35 % mana beside 97 drinks of the guild storage while its
    // dungeon group waited for it, 2026-11-14).
    void StopLeftoverAutoShot(Player* player, GuildAdventure::BotRuntime& rt);
    // What a class should always have on and mod-playerbots never casts at its level: a mage's Frost or Ice Armor
    // below Mage Armor.
    void StepSelfBuff(Player* player, GuildAdventure::BotRuntime& rt);
    // The pull check (FightEscape.h CheckPull) for one character, and what the planner does with a refusal.
    void StepPullCheck(Player* player, GuildAdventure::BotRuntime& rt);
    // A character that stands in a hostile ground effect steps out of it (BotDriver::StepOutOfGroundEffect), four times a
    // second, whatever the module holds it for; a rest ends for it.
    void StepGroundEffect(Player* player, GuildAdventure::BotRuntime& rt);
    // C'Thun's step four times a second for the bot that leads a raid in his chamber (StepRaidCthun).
    void StepRaidCthunFast(Player* lead, GuildAdventure::BotRuntime& rt);
    // Out of a fight: a hostile ground effect its movement would lead through is walked around while it lasts; with no
    // way around it the bot waits at its edge (an effect that ends soon, or one its movement ends in) or goes through.
    void StepGroundEffectAhead(Player* player, GuildAdventure::BotRuntime& rt, uint32 now);
    // A step out of the hostile ground effect it stands in to the nearest spot outside every hostile effect around (the
    // Clouds of Disease of several dead ghouls lie side by side). False when none is in reach.
    bool StepOutOfGroundEffects(Player* player);
    // The ground guard cancels every movement of the bot from where it stands: it takes a teleporter next to it; a bot
    // off the navmesh is rescued; else the guard is switched off for a minute (`what` goes into the activity log line:
    // "is stuck", ...).
    // A teleporter next to it, or the rescue when it stands off the walkable ground; false when neither applies (the
    // caller ends what it was doing: the ground guard is never switched off, the owner: no walk goes blindly straight).
    bool GroundWayOut(Player* player, GuildAdventure::BotRuntime& rt, char const* what);
    // In a fight every step towards its enemy cancelled and no way out (GroundWayOut): the creature is left alone for 15
    // minutes (BotRuntime::undyingFoes, its spawn too), the fight ends and the bot walks away from it.
    void LeaveUnreachableFoe(Player* player, GuildAdventure::BotRuntime& rt);
    // A gathering node among more creatures than the bot takes at once: four or more that would attack it within 45 yd
    // of the node (down to eight levels below it, Risk::GuardsNear), three of the strongest of them more than its fights
    // allow (Risk::CanFight). Nodes are optional: no detour and no gather round goes to such a one.
    bool NodeAmongMany(Player const* player, GuildAdventure::BotRuntime const& rt, uint32 mapId, float x, float y, float z) const;
    // In a dungeon: a bot that fights something it cannot walk to, 8 yd or more below or above it, is put on the
    // ground beside it (at most once a minute).
    bool HopDownToFight(Player* player, GuildAdventure::BotRuntime& rt);
    // A squad member's own visit at the vendor its leader has just visited (BotRuntime::vendorVisit): true while it is
    // under way (the member walks there, then sells, is repaired and makes room).
    bool StepMemberVendorVisit(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // Out of a place that holds the bot (every way from it refused, its zone travels ending where they began): back the
    // way it came along its trail before the hearthstone (the owner: the hearthstone only in the utmost emergency). True
    // when the walk begins; once per quarter of an hour.
    bool StartTrailOut(Player* player, GuildAdventure::BotRuntime& rt, std::string const& why);
    bool StepTrailOut(Player* player, GuildAdventure::BotRuntime& rt);
    // A fight or a death: the stuck tracker's clocks stand still meanwhile.
    void PauseStuck(GuildAdventure::BotRuntime& rt);
    // Rest between fights: true while the bot rests (nothing is planned or stepped meanwhile).
    bool StepRest(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt);
    void EndRest(Player* player, GuildAdventure::BotRuntime& rt, char const* outcome);
    // Activity log: follows the fight the bot is in and writes its line when it ends (won, or died when `died`).
    void TrackFight(Player* player, GuildAdventure::BotRuntime& rt);
    void EndFight(Player* player, GuildAdventure::BotRuntime& rt, bool died);
    // On a ship or zeppelin without a voyage (the order changed on board, the owner played the bot there): go ashore at
    // the next dock. False when the transport is not one of the known routes.
    bool StartShoreLeave(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime& rt, MotionTransport* ship);
    // A travel (or the walk to a dock) got stuck: try again shortly, set the zone aside after repeated attempts.
    void OnTravelStuck(GuildAdventure::GuildBot const& bot, GuildAdventure::BotRuntime& rt, float distance);

    std::string DescribeTask(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::BotRuntime const& rt) const;

    // Squads (OrderSquads.cpp, Squads.h). UpdateSquad runs once a tick before Reconcile: whether the bot follows a squad
    // leader now, a member far behind catching up, a leader waiting for a member. RunSquadMember replaces the order of a
    // following member (quests with its leader, the why line). StepSquadFlee: a member runs with its fleeing leader
    // (true while it does). StepSquadWait: a waiting leader's task clocks stand still (true while it waits).
    void UpdateSquad(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player, GuildAdventure::BotRuntime& rt);
    void RunSquadMember(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player, GuildAdventure::BotRuntime& rt);
    bool StepSquadFlee(Player* player, GuildAdventure::BotRuntime& rt);
    bool StepSquadWait(Player* player, GuildAdventure::BotRuntime& rt);
    // What a squad member's why line and its Details window say of its leader (the owner, 2026-11-27: in a raid he wants to
    // see why the group or the tank waits): the leader's why without "In <dungeon>: ", its "Squad: waiting (..)" as
    // "waiting for its squad (..)", nothing for its other squad lines or none.
    [[nodiscard]] static std::string LeaderWhyForMember(GuildAdventure::BotRuntime const* leader);
    // A member died: its leader remembers the place (RiskModel) as if it had died there itself.
    void OnSquadMemberDied(Player* member, GuildAdventure::BotRuntime& rt);
    // A quest partner on its way to its leader (QuestPartners.h): it walks there along the road map (no catch-up teleport
    // before they met). In place of the member's order while it is under way.
    void StepPartnerMeet(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // A squad member far behind its leader travels after it: the next ship, zeppelin or portal to the leader's land, else
    // a travel to where it stands (flights included), the last stretch on foot.
    void StepRejoinTravel(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot& bot, Player* player, GuildAdventure::BotRuntime& rt);
    // A member in its leader's party (free for all loot while the AI plays the leader).
    void KeepSquadGroup(Player* player, GuildAdventure::BotRuntime const& rt, Player* master);

    // The planner's counters (PF-0): one record per world tick, the last minute kept.
    struct StepCost
    {
        uint32 us = 0;
        ObjectGuid guid;
        GuildAdventure::TaskKind kind = GuildAdventure::TaskKind::None;
    };
    struct TickRecord
    {
        uint64 atMs = 0;
        uint32 plannerUs = 0;     // the whole of OrderService::Update in this world tick
        uint32 tickMs = 0;        // the world tick's diff
        uint16 stepped = 0;       // characters and guilds stepped
        uint16 deferred = 0;      // ... due but left for the next tick (budget)
        uint32 lateMs = 0;        // how late the oldest step of this tick was
        uint32 routes = 0, routeUs = 0, probes = 0, probeUs = 0, hits = 0;
        uint16 flights = 0, errands = 0, zonePicks = 0;
        StepCost slowest[3];
    };
    // The planner's passes (docs/performance.md PF-1). GuildAdventure.Planner.Spread = 0: every character in one world
    // tick, once a second (and the quick checks four times a second), as before. Spread: every character and every
    // guild has a phase of its own; each world tick steps what is due, oldest first, within a budget.
    void UpdateAllAtOnce(uint32 diff);
    void UpdateSpread(uint32 diff, TickRecord& record);
    // One character's turn in the four-times-a-second loop (pull check, careful pull, dungeon conductor, ground effects).
    void StepFast(ObjectGuid guid, GuildAdventure::BotRuntime& rt);
    // A guild's own work once a second: quest partners; squads and recipes once a minute.
    void StepGuild(GuildAdventure::AdventureGuild& guild, bool pairSquads);

    void RecordTick(TickRecord& record, uint64 startUs, GuildAdventure::Perf::Counters const& before);
    void NoteStepCost(TickRecord& record, uint32 us, ObjectGuid guid);

    std::unordered_map<ObjectGuid, GuildAdventure::BotRuntime> _runtime;
    mutable std::unordered_map<uint64, uint8> _questWorkDanger; // QuestWorkDanger: (quest, team, level) -> danger
    mutable std::map<std::tuple<uint32, bool, uint8, uint8>, uint8> _placeWayDanger; // PlaceWayDanger: (spawn, object, team, level)
    mutable std::unordered_map<uint64, std::vector<HubWay>> _questWorkWays; // QuestWorkReachable: (quest, team, level)
    mutable std::map<std::tuple<uint32, bool, uint8, uint8>, HubWay> _placeWays; // PlaceWayReachable: (spawn, object, team, level)
    // Vendors whose limited stock of an item a guild character found sold out: (vendor spawn << 32 | item) -> until when
    // (the stock's restock time). No trip is planned there for that item meanwhile (PlanMaterialPurchase).
    std::unordered_map<uint64, uint32> _soldOut;
    uint32 _timer = 0;
    uint32 _pullTimer = 0; // the pull check runs more often than the planner
    uint32 _nextSquadPairing = 0;  // getMSTime()
    std::unordered_map<ObjectGuid, uint64> _planDue;     // Perf::NowMs(): a character's next step (Spread)
    std::unordered_map<uint32, uint64> _guildDue;        // owner account -> its guild's next step
    std::unordered_map<uint32, uint64> _guildPairDue;    // ... and its next squad pairing and recipe look
    uint64 _nextDuePrune = 0;
    std::deque<TickRecord> _perfTicks;                   // the last minute
    uint64 _nextSlowTickLog = 0;
    uint32 _slowTicksQuiet = 0;
};

#define sOrderService OrderService::Instance()

#endif
