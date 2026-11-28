/*
 * OrderService implementation. See OrderService.h and docs/orders.md.
 */

#include "OrderService.h"
#include "Ammunition.h"
#include "BotSettings.h"
#include "ClassAbilities.h"
#include "DeathKnights.h"
#include "FightProfiles.h"
#include "RoguePoisons.h"
#include "CraftService.h"
#include "Bag.h"
#include "CharacterCache.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Group.h"
#include "GuildAdventureMgr.h"
#include "GameGraveyard.h"
#include "GroundGuard.h"
#include "GuildAdventureSettings.h"
#include "GuildBudget.h"
#include "GuildAdventureUtil.h"
#include "GuildBotHolder.h"
#include "GuildEvents.h"
#include "Item.h"
#include "Log.h"
#include "LootService.h"
#include "MapMgr.h"
#include "LootMgr.h"
#include "Map.h"
#include "NewRpgHelpers.h"
#include "ObjectAccessor.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "QuestDef.h"
#include "QuestEvents.h"
#include "QuestPartners.h"
#include "QuestSupport.h"
#include "CraftOrders.h"
#include "DungeonNav.h"
#include "DungeonRoles.h"
#include "Dungeons.h"
#include "FightEscape.h"
#include "Formulas.h"
#include "Fishing.h"
#include "GearUpkeep.h"
#include "GridTerrainData.h"
#include "RiskModel.h"
#include "PathProbe.h"
#include "PathGenerator.h"
#include "Perf.h"
#include "Professions.h"
#include "RoadMap.h"
#include "SpecService.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "ShoppingList.h"
#include "Squads.h"
#include "ScriptMgr.h"
#include "StorageService.h"
#include "StorageSupply.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Trainer.h"
#include "Transport.h"
#include "CellImpl.h"
#include "DynamicObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "World.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <array>
#include <cmath>
#include <deque>
#include <functional>
#include <optional>
#include <unordered_set>

using namespace GuildAdventure;

namespace
{
    // A quest whose zone is the area of a dungeon or raid (QuestSortID): its targets live inside.
    bool IsDungeonQuest(Quest const* quest)
    {
        int32 const sort = quest ? quest->GetZoneOrSort() : 0;
        if (sort <= 0)
            return false;
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(uint32(sort));
        MapEntry const* map = area ? sMapStore.LookupEntry(area->mapid) : nullptr;
        return map && map->Instanceable();
    }

    constexpr uint32 TickMs = 1000;

    // WAQ sweep: an eligible quest whose giver is this close is picked up before any other work.
    constexpr float NearbyPickupRadius = 60.0f;
    // Keep this many quest-log slots free; NewRpg empties the whole log when fewer than two are free.
    constexpr uint32 MinFreeQuestSlots = 3;
    constexpr uint32 DroppedQuestSeconds = 3 * HOUR; // a quest dropped for the quest log is not taken again this long
    // Closer than this to an NPC target the bot stops walking to the spawn and visits the NPC it sees.
    constexpr float NearDistance = 30.0f;
    // Search radius for the NPC of a task once the bot is near its spawn point.
    constexpr float SightDistance = 60.0f;
    // How long a bot may stand at an NPC without the task finishing before it counts as failed.
    constexpr uint32 NpcTimeoutMs = 45 * IN_MILLISECONDS;
    // Upper bound for walking to and dealing with one NPC.
    constexpr uint32 NpcTaskMaxMs = 15 * MINUTE * IN_MILLISECONDS;
    // Grind tasks end when NewRpg's wander phase ends, at the latest after this.
    constexpr uint32 GrindMaxMs = 10 * MINUTE * IN_MILLISECONDS;
    // A hold is re-planned this often (level cap raised, order changed in between, ...).
    constexpr uint32 HoldReplanMs = 30 * IN_MILLISECONDS;
    // Re-assert the task's NewRpg status at most this often when NewRpg left it.
    constexpr uint32 AssertGapMs = 5 * IN_MILLISECONDS;
    // wowbot335 A20: no 10-yard progress in two minutes (fights and death excluded) = stuck.
    constexpr float StuckDistance = 10.0f;
    constexpr uint32 StuckTimeMs = 2 * MINUTE * IN_MILLISECONDS;
    // AUTO re-evaluates its zone this often (and after every level-up).
    constexpr uint32 ZoneRecheckMs = MINUTE * IN_MILLISECONDS;
    constexpr uint32 UpkeepCheckMs = 30 * IN_MILLISECONDS;
    constexpr uint32 GroupCheckMs = 10 * IN_MILLISECONDS;
    // Repair when the most worn equipped item is below this percentage.
    constexpr uint32 RepairBelowPercent = 25;
    // Sell when this few bag slots are left.
    constexpr uint32 SellAtFreeSlots = 2;
    // A repair the bot could not pay, or a selling trip that freed no space, is tried again after this time.
    constexpr uint32 UpkeepRetryMs = 15 * MINUTE * IN_MILLISECONDS;
    // Restock: a bot that runs low on food (or, using mana, on drink) walks to a food vendor, one trip per this time
    // at most, in case the vendor has nothing for it (GuildAdventure.Provisions.*: the money it needs, how low is low).
    constexpr uint32 RestockGapMs = 10 * MINUTE * IN_MILLISECONDS;
    // A vendor near the bot (GuildAdventure.Provisions.HubYards) is looked for this often while it carries less than
    // half a stack of food or drink.
    constexpr uint32 NearbyRestockGapMs = 3 * MINUTE * IN_MILLISECONDS;
    // The guild storage is looked into for supplies this often (StorageSupply.h).
    constexpr uint32 SupplyCheckMs = MINUTE * IN_MILLISECONDS;
    // Class-quest abilities (ClassAbilities.h) are looked at this often besides logins and level-ups.
    constexpr uint32 ClassAbilitiesCheckMs = 5 * MINUTE * IN_MILLISECONDS;
    // A hunter's pet's free talent points are looked at this often (a field read while it has none).
    constexpr uint32 PetTalentsCheckMs = 10 * IN_MILLISECONDS;
    // A rogue that went to a poison vendor (or found none at its hub) looks again after this long.
    constexpr uint32 PoisonTopUpGapMs = 10 * MINUTE * IN_MILLISECONDS;
    // A trip to a poison vendor that something cut short is taken up again for this long while the bot is this near the vendor.
    constexpr uint32 PoisonTripMs = 20 * MINUTE * IN_MILLISECONDS;
    constexpr float PoisonTripReach = 400.0f;
    // A rogue with bare weapons and no poison: a poison vendor this near that stands in a town (an innkeeper or a flight
    // master within PoisonTownYards of it), looked for between two tasks at most this often.
    constexpr float PoisonTownReach = 300.0f;
    constexpr float PoisonTownYards = 250.0f;
    constexpr uint32 PoisonLookMs = 15 * IN_MILLISECONDS;
    constexpr float PoisonLookMoveYards = 60.0f;
    // A vendor, repairer or trainer in a zone that starts more than this many levels above the bot is none for it
    // (Lakeshire for a level 9 at the eastern edge of Elwynn Forest: the way there killed it).
    constexpr uint8 ServiceZoneLevelMargin = 2;
    // A vendor NewRpg does not count as visited although the bot stands in front of it (innkeepers with their gossip
    // menu): after this time near it the module buys and repairs itself.
    constexpr uint32 VendorSelfServeMs = 8 * IN_MILLISECONDS;
    // An objective at a known spawn without a kill, a fight, an item or a rest for this long: the spawn is skipped and
    // another one tried (NewRpg's own five-minute rule only runs once the bot stands at the spot).
    constexpr uint32 ObjectiveIdleMs = 3 * MINUTE * IN_MILLISECONDS;
    // ... and after this many spawns in a row without progress the quest counts as failed (A21), like NewRpg's own rule.
    constexpr uint8 ObjectiveIdleSpawns = 3;
    // A spawn with no target of the objective in sight (killed, not respawned): the bot moves on to the next spawn after
    // this long instead of waiting out ObjectiveIdleMs there (a warlock stood three minutes at every dead boar's place:
    // nine fights in sixteen minutes of "objective"). The spawn is left alone for about a respawn; this many such
    // spawns in a row without progress are a failure of the quest like the idle ones.
    constexpr uint32 ObjectiveEmptyMs = 12 * IN_MILLISECONDS;
    // ... and after this long when the map says the spawn's own creature is dead or its object taken (the bot's kill):
    // nothing comes back there for minutes, and the twelve seconds were paid after every kill at a lone spawn.
    constexpr uint32 ObjectiveDownMs = 3 * IN_MILLISECONDS;
    constexpr float ObjectiveEmptyReach = 70.0f;
    constexpr uint32 ObjectiveRespawnMs = 4 * MINUTE * IN_MILLISECONDS;
    // An objective at creatures: once the bot has been at the spawn, after each fight and rest, it looks within
    // ObjectiveNearYards for a living creature of the objective; with none it goes on to the next spawn at once
    // (OrderService::StepTask). ObjectiveSettleMs after a fight or rest first: the kill is looted (auto loot), the rest
    // and the check before the next pull decide.
    constexpr float ObjectiveNearYards = 45.0f;
    constexpr uint32 ObjectiveSettleMs = 3 * IN_MILLISECONDS;
    constexpr uint8 ObjectiveEmptySpawns = 12;
    // A quest whose every spot is too much for the bot (Risk::JudgeSpot) is left until the level the risk model names,
    // or for the second time when it names none (no level in reach helps, or the places are only set aside for minutes).
    constexpr uint32 SpotRetryLevelMs = 8 * HOUR * IN_MILLISECONDS;
    constexpr uint32 SpotRetryMs = 15 * MINUTE * IN_MILLISECONDS;
    // Spell categories of food and drink items (the same playerbots' eat and drink actions look for).
    constexpr uint32 FoodSpellCategory = 11;
    constexpr uint32 DrinkSpellCategory = 59;
    // Services farther away than this are not worth the walk.
    constexpr float MaxServiceDistance = 3000.0f;
    // A quest ender outside the target zone is visited only when it is this close.
    constexpr float NearbyTurnInDistance = 400.0f;
    // Escorts: the bot keeps this close to the NPC; an NPC that has not left its spot after EscortStartMs has not
    // started (the quest is given back and taken again); out of sight for EscortLostMs the escort is lost; the whole
    // escort may take EscortMaxMs.
    constexpr float EscortFollowDistance = 8.0f;
    constexpr float EscortSight = 150.0f;
    constexpr uint32 EscortStartMs = 90 * IN_MILLISECONDS;
    constexpr uint32 EscortLostMs = 75 * IN_MILLISECONDS;
    constexpr uint32 EscortMaxMs = 25 * MINUTE * IN_MILLISECONDS;
    // Quest givers and enders farther away than this are not planned.
    constexpr float MaxQuestNpcDistance = 2500.0f;
    // Quest givers of the target zone farther than that count when nothing else is in reach.
    constexpr float ZoneWideDistance = 12000.0f;
    // A zone the bot travels to is entered at the nearest quest giver it has there, looked for this far.
    constexpr float ZoneEntryReach = 30000.0f;
    // An errand is walked along a route of at most this length; farther only by flight.
    constexpr float ErrandMaxRoute = 8000.0f;
    // A cancelled step of a route takes the road map nodes this close to its target out of the graph.
    constexpr float RouteNodeRadius = 25.0f;
    // Where a rescued bot is put: the nearest node of the road map within this range.
    constexpr float RescueRange = 1500.0f;
    // An objective's target outside the target zone is worked when it is this close (targets just across the border)
    // and not more than this many levels above the bot.
    constexpr float NearbyObjectiveDistance = 1500.0f;
    constexpr uint8 NearbyObjectiveLevels = 3;
    // Grind spots: candidates around the bot, cluster radius.
    constexpr float GrindClusterRadius = 50.0f;
    // A grinding bot attacks a creature from this far; with nothing to fight at its spot for the time below, the spot is
    // left alone for the last value. (Which creatures, and how far away: GuildAdventure.Grind.*.)
    constexpr float GrindAttackDistance = 30.0f;
    constexpr uint32 GrindIdleMs = 20 * IN_MILLISECONDS;
    constexpr uint32 GrindEmptyMs = 5 * MINUTE * IN_MILLISECONDS;
    // A hand-in or a pickup farther away than the first value is not walked to when the weakest of the creatures
    // within the second value of it is too far above the bot (at least PlaceMinCreatures of them live there).
    constexpr float PlaceCheckDistance = 150.0f;
    constexpr float PlaceRadius = 150.0f;
    constexpr uint32 PlaceMinCreatures = 3;
    // A group of hostile creatures standing together on a way is asked of the risk model before a task's walk is walked
    // when it counts for the bot at all (RoadMap::GroupCounts: three of about its level, four not more than eight below
    // it). (With twelve levels below, as first built, journeys were given up for creatures far below the traveller: a
    // level-58 priest was kept from both Plaguelands by five level-47 creatures somewhere on 10 000 yd.)
    // Full bags are a reason for a vendor trip only to a vendor this close: the bag sweep puts loot into the guild
    // storage anyway.
    constexpr float FullBagsVendorDistance = 400.0f;
    // A hand-in or a pickup this close to the bot comes before upkeep (a vendor, a trainer).
    constexpr float NearQuestNpcDistance = 150.0f;
    // Distance at which a trainer teaches (INTERACTION_DISTANCE plus a counter's width).
    constexpr float TeachDistance = 10.0f;
    // An NPC or spot the bot failed to reach or deal with is skipped this long.
    constexpr uint32 AvoidSpawnMs = 20 * MINUTE * IN_MILLISECONDS;

    // Whether a living creature the objective needs is within `range` of the bot: the creature of a kill objective, the
    // creatures an item objective's item comes from (WorldIndex::GetItemSourceCreatures: all four kinds of coyote of
    // "Badlands Reagent Run", not only the kind of the spawn the task was given).
    bool NeedNear(Player* player, Quest const* quest, int32 objective, uint32 spotEntry, float range)
    {
        std::vector<uint32> entries;
        if (quest && objective >= 0 && objective < QUEST_OBJECTIVES_COUNT && quest->RequiredNpcOrGo[objective] > 0)
            entries.push_back(uint32(quest->RequiredNpcOrGo[objective]));
        else if (quest && objective >= QUEST_OBJECTIVES_COUNT &&
            objective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
            entries = sWorldIndex.GetItemSourceCreatures(quest->RequiredItemId[objective - QUEST_OBJECTIVES_COUNT]);
        if (spotEntry && std::find(entries.begin(), entries.end(), spotEntry) == entries.end())
            entries.push_back(spotEntry);
        for (uint32 entry : entries)
            if (player->FindNearestCreature(entry, range, true))
                return true;
        return false;
    }
    // The loot of its kills for an item objective: a corpse of its own kill (or its group's) within 45 yd that holds
    // `item` for the character is walked to, 12 s at most each; true while it walks to one. Auto loot takes a corpse only
    // within GuildAdventure.AutoLoot.Range (10 yd), and a caster or a hunter kills its target 20-35 yd away.
    bool StepObjectiveLoot(Player* player, GuildAdventure::BotTask& task, uint32 item)
    {
        constexpr float LookYards = 45.0f;
        constexpr float ReachYards = 3.0f;
        constexpr uint32 MaxMs = 12 * IN_MILLISECONDS;
        constexpr uint32 AssertMs = 2 * IN_MILLISECONDS;
        if (!item || !player->GetFreeInventorySpace())
            return false;
        uint32 const now = std::max<uint32>(getMSTime(), 1);
        std::list<Creature*> dead;
        player->GetDeadCreatureListInGrid(dead, LookYards);
        Group const* group = player->GetGroup();
        Creature* corpse = nullptr;
        float best = 0.0f;
        for (Creature* creature : dead)
        {
            if (!creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE) || creature->loot.loot_type == LOOT_SKINNING || creature->loot.isLooted())
                continue;
            if (creature->GetLootRecipient() != player && !(group && creature->GetLootRecipientGroup() == group))
                continue;
            if (std::find(task.lootLeft.begin(), task.lootLeft.end(), creature->GetGUID()) != task.lootLeft.end())
                continue;
            bool holds = false;
            uint32 const slots = creature->loot.GetMaxSlotInLootFor(player);
            for (uint32 i = 0; i < slots && !holds; ++i)
                if (LootItem* loot = creature->loot.LootItemInSlot(i, player))
                    holds = loot->itemid == item;
            if (!holds)
                continue;
            float const distance = player->GetDistance(creature);
            if (!corpse || distance < best)
            {
                corpse = creature;
                best = distance;
            }
        }
        if (!corpse)
        {
            task.lootCorpse.Clear();
            return false;
        }
        if (corpse->GetGUID() != task.lootCorpse)
        {
            task.lootCorpse = corpse->GetGUID();
            task.lootSince = now;
            task.lootAssert = 0;
        }
        if (getMSTimeDiff(task.lootSince, now) > MaxMs)
        {
            task.lootLeft.push_back(corpse->GetGUID());
            task.lootCorpse.Clear();
            return false;
        }
        if (best > ReachYards && (!task.lootAssert || getMSTimeDiff(task.lootAssert, now) >= AssertMs))
        {
            BotDriver::GoTo(player, player->GetMapId(), corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ(), false);
            task.lootAssert = now;
        }
        return true;
    }
    // A quest NPC the bot saw but that did not give or take the quest is skipped only this long: in a start zone one
    // giver often holds every quest, and a 20-minute break left a bot idle (Hunterbm in Shadowglen, 2026-11-12).
    constexpr uint32 QuestNpcRetryMs = 2 * MINUTE * IN_MILLISECONDS;
    // In its first minute in the world a bot that fails at a quest NPC only tries again (no failure, no break): its AI
    // may still be settling in after the login.
    constexpr uint32 LoginGraceMs = MINUTE * IN_MILLISECONDS;
    // Activity log (docs/orders.md "Activity log"): its own logger, so it can go to a file of its own; without a
    // Logger.module.ga.activity line in worldserver.conf it falls back to the "module" logger (Server.log).
    constexpr char const* ActivityLog = "module.ga.activity";
    constexpr uint32 StatusLogMs = 5 * MINUTE * IN_MILLISECONDS;
    // Deaths: this many deaths at the same quest (within the window) or during one grind task set the quest or the
    // grind spot (with this radius) aside until the bot gains a level or the time passes.
    constexpr uint8 DeathsToSetAside = 3;
    constexpr uint32 DeathWindowMs = 30 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 SetAsideMs = 30 * MINUTE * IN_MILLISECONDS;
    constexpr float AvoidAreaRadius = 100.0f;
    // Targets set aside for hours (BotRuntime::lastingAvoids), as kinds of ga_bot_wait: a creature's or an object's spawn
    // id, or an area as map and a cell of 10 yd.
    constexpr uint8 LastingCreature = 5;
    constexpr uint8 LastingObject = 6;
    constexpr uint8 LastingArea = 7;
    // A zone counted from afar that the bot found nothing to do in (BotRuntime::afarEmpty): the zone and the level, as
    // a kind of ga_bot_wait.
    constexpr uint8 WaitAfarEmpty = 8;
    constexpr uint32 AfarEmptyMs = 6 * HOUR * IN_MILLISECONDS;
    // A node of the road map that stands on the navmesh: from it the path finder has a path in some direction. (The lifts
    // of the Searing Gorge's Cauldron are nodes without a polygon of their own: a rescue put a druid at one, "Scaffold
    // Cars" at (-6900, -1339), and 26 s later it was rescued again "off the walkable ground".)
    bool NodeOnNavmesh(Player* player, RoutePoint const& node)
    {
        constexpr float AroundYards = 4.0f;
        Map* map = player->GetMap();
        int asked = 0;
        for (int i = 0; i < 8; ++i)
        {
            float const px = node.x + std::cos(float(i) * float(M_PI) / 4.0f) * AroundYards;
            float const py = node.y + std::sin(float(i) * float(M_PI) / 4.0f) * AroundYards;
            float const pz = map->GetHeight(player->GetPhaseMask(), px, py, node.z + 3.0f, true, 8.0f);
            if (pz <= INVALID_HEIGHT)
                continue;
            ++asked;
            PathReport const step = ProbePath(player, node.x, node.y, node.z, px, py, pz);
            if (step.valid && !(step.type & (PATHFIND_NOT_USING_PATH | PATHFIND_FARFROMPOLY_START)))
                return true;
        }
        // No ground around it at all: it hangs in the air (a lift's stop); a few points to ask: not judged.
        return asked > 0 && asked < 3;
    }

    uint32 LastingAreaKey(uint32 mapId, float x, float y)
    {
        uint32 index = 0;
        switch (mapId)
        {
            case 0: index = 0; break;
            case 1: index = 1; break;
            case 530: index = 2; break;
            case 571: index = 3; break;
            default: return 0;
        }
        int32 const cx = int32(std::floor((x + 20480.0f) / 10.0f)), cy = int32(std::floor((y + 20480.0f) / 10.0f));
        if (cx < 0 || cx > 4095 || cy < 0 || cy > 4095)
            return 0;
        return (1u << 26) | (index << 24) | (uint32(cx) << 12) | uint32(cy);
    }
    bool LastingAreaSpot(uint32 key, uint32& mapId, float& x, float& y)
    {
        if (!(key & (1u << 26)))
            return false;
        static constexpr uint32 Maps[] = { 0, 1, 530, 571 };
        mapId = Maps[(key >> 24) & 3];
        x = float((key >> 12) & 4095) * 10.0f - 20480.0f + 5.0f;
        y = float(key & 4095) * 10.0f - 20480.0f + 5.0f;
        return true;
    }
    // A second death within this time and distance of the first one: the ghost takes the spirit healer, which stands
    // this close to where a released ghost appears.
    constexpr uint32 CorpseCampWindowMs = 10 * MINUTE * IN_MILLISECONDS;
    constexpr float CorpseCampRadius = 40.0f;
    constexpr float SpiritHealerSearchRadius = 60.0f;
    // The spirit healer costs 25 % durability: below this the corpse run is the better choice.
    constexpr uint32 SpiritResMinDurability = 40;
    // Stuck tracker: the bot must get this much closer to its target within the time (NewRpg teleports after 90 s,
    // BotDriver::HoldOffStuckTeleport keeps it from doing so).
    constexpr float ApproachDistance = 5.0f;
    constexpr uint32 NoApproachMs = 3 * MINUTE * IN_MILLISECONDS;
    // This many stuck travels in a row towards the same zone set it aside for the time.
    constexpr uint8 TravelStuckLimit = 3;
    constexpr uint32 UnreachableZoneMs = 30 * MINUTE * IN_MILLISECONDS;
    // Voyages: a ship counts as docked within this distance of its stop. Walking to the dock may end this far away
    // when the path runs out at the end of the pier; the bot waits there and may board from there (the same distance,
    // so a bot that counts as arrived can always board).
    constexpr float DockedDistance = 12.0f;
    constexpr float DockReachedDistance = 60.0f;
    // A voyage that has not boarded within this time is given up (the dock is unreachable).
    constexpr uint32 VoyageBoardTimeoutMs = 30 * MINUTE * IN_MILLISECONDS;
    // A travel is not walked when even its safest route passes hostile creatures more than this many levels above the
    // bot (elites count four levels higher).
    // (Eight until 2026-11-13, from the days when the danger map counted a creature 25 yd from the way. Since it counts
    // what would really attack, a creature six levels above the walker on the way is a fight it loses and a run it
    // seldom survives: a level-22 character got away from a level-27 scout and died to a level-29 lurker.)
    constexpr int32 RouteMaxDanger = 5;
    // The walk to the spot of an objective is not walked when its route is longer than both of these: a spot "close
    // by" behind a mountain.
    constexpr float ObjectiveMaxRoute = 2500.0f;
    constexpr float ObjectiveMaxDetour = 4.0f;
    // Vendor trips "at a hub" (GuildAdventure.Vendor.Trips = 0): this long after a hand-in, to a vendor this close;
    // urgent: this few free bag slots, gear below this, and no trip for ammunition more often than the last value.
    constexpr uint32 HubWindowMs = 3 * MINUTE * IN_MILLISECONDS;
    constexpr float HubDistance = 200.0f;
    constexpr uint32 UrgentFreeSlots = 1;
    constexpr uint32 UrgentRepairPercent = 20;
    constexpr uint32 AmmoTripGapMs = 30 * MINUTE * IN_MILLISECONDS;
    // A top-up of a hunter's ammunition at a vendor in reach: not more often than this, and only when at least one
    // vendor bundle is missing.
    constexpr uint32 AmmoTopUpGapMs = 10 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 AmmoBundle = 200;
    // The profession plan is looked at this often for something to learn; crafting for skill is tried once per this
    // time; a gather detour may take this long, and nodes are looked for this often.
    constexpr uint32 ProfessionCheckMs = MINUTE * IN_MILLISECONDS;
    constexpr uint32 CraftCheckGapMs = 10 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 GatherDetourMs = 30 * IN_MILLISECONDS;
    constexpr uint32 GatherLookMs = 3 * IN_MILLISECONDS;
    // A gathering bot stands this close to its node (inside the reach of the gathering spell).
    constexpr float GatherStandDistance = 3.0f;
    // The profession mode: places of nodes are looked for this far, a place counts as reached this close and is not
    // visited again for the time below (its node may be another place of the pool); crafting is tried this often, up
    // to this many casts at a time.
    // Farther than GatherRoundDistance the walk is a travel (flights), and only the nearest few places are looked at
    // closely (region, zone, risk).
    constexpr float GatherRoundDistance = 2500.0f;
    // A hunting ground (for skins, for a material) farther than this is walked to as a journey first: the walk of a
    // grind task is made for the next spawn, not for 2000 yd (a hunter stood six minutes at the Thandol Span).
    constexpr float HuntGroundNear = 300.0f;
    // The guild's deaths of the last hours by zone and quest (ga_guild_death, kept across restarts; world thread).
    // Half of a day's deaths fell in one zone, Redridge, on characters of level 17 to 26, mostly in fights against
    // groups at and below their level: what one character learns from its deaths the others walked into anew.
    struct GuildDeath
    {
        uint32 zone = 0;
        uint32 quest = 0;
        uint8 level = 0;
        time_t at = 0;
    };
    std::unordered_map<uint32, std::deque<GuildDeath>> sGuildDeaths; // by owner account
    std::unordered_map<uint64, time_t> sQuestDeathNotes;             // (character, quest) -> when the wait was last logged
    constexpr time_t GuildDeathKeepSeconds = 6 * HOUR;
    // A zone is hot for a character when this many guild characters of its level - 1 and above died there in this time.
    constexpr time_t HotZoneSeconds = 3 * HOUR;
    constexpr uint32 HotZoneDeaths = 5;
    // A quest a guild character this many levels above the bot died on waits for the bot's level (or a partner).
    constexpr uint8 QuestDeathLevelsAbove = 2;
    // With this many deaths on a quest it waits for every bot that is not that many levels above the highest of the
    // dead: the ones who die on a quest are often the highest that have it (the Kurzen compound in Stranglethorn: four
    // deaths in three hours at level 32 to 37, and nobody below them had its quests).
    constexpr uint32 QuestDeathsMany = 2;
    constexpr float PartnerBesideYards = 40.0f;

    uint32 CountGuildDeaths(uint32 owner, uint32 zone, uint8 fromLevel)
    {
        auto deaths = sGuildDeaths.find(owner);
        if (deaths == sGuildDeaths.end())
            return 0;
        time_t const oldest = GameTime::GetGameTime().count() - HotZoneSeconds;
        uint32 count = 0;
        for (GuildDeath const& death : deaths->second)
            if (death.zone == zone && death.level >= fromLevel && death.at >= oldest)
                ++count;
        return count;
    }

    // How often guild characters died on the quest in the last hours, and the highest level among them.
    uint32 QuestDeaths(uint32 owner, uint32 quest, uint8& highest)
    {
        highest = 0;
        auto deaths = sGuildDeaths.find(owner);
        if (deaths == sGuildDeaths.end() || !quest)
            return 0;
        time_t const oldest = GameTime::GetGameTime().count() - GuildDeathKeepSeconds;
        uint32 count = 0;
        for (GuildDeath const& death : deaths->second)
            if (death.quest == quest && death.at >= oldest)
            {
                ++count;
                highest = std::max(highest, death.level);
            }
        return count;
    }

    void AddGuildDeath(uint32 owner, uint32 zone, uint32 quest, uint8 level)
    {
        time_t const now = GameTime::GetGameTime().count();
        std::deque<GuildDeath>& deaths = sGuildDeaths[owner];
        while (!deaths.empty() && deaths.front().at + GuildDeathKeepSeconds < now)
            deaths.pop_front();
        deaths.push_back({ zone, quest, level, now });
        CharacterDatabase.Execute("INSERT INTO ga_guild_death (owner_account, zone_id, quest_id, level, died_at) VALUES ({}, {}, {}, {}, {})",
            owner, zone, quest, uint32(level), uint32(now));
    }

    // Crafting and enchanting where the bot stands: nothing hostile within this many yards (IsQuietToCraft).
    constexpr float CraftQuietYards = 30.0f;
    constexpr uint32 UnquietNoteMs = 3 * MINUTE * IN_MILLISECONDS;
    // The hearthstone of a bot that is stranded where it stands (the zone it is in has nothing it can safely get to,
    // or killed it twice): an inn this far away is a way out already. Otherwise HearthHome's 1500 yd.
    constexpr float StrandedHearthYards = 250.0f;
    constexpr float DefaultHearthYards = 1500.0f;
    // After a rescue out of the water the water around is not walked through for this long (Rescue).
    constexpr uint32 WaterAvoidMs = 30 * MINUTE * IN_MILLISECONDS;

    // Stranded: the zone the bot stands in is one it has set aside for itself.
    bool IsStrandedHere(Player* player, BotRuntime const& rt)
    {
        uint32 const zone = player->GetZoneId();
        return rt.IsZoneUnreachable(zone) || (rt.emptyZone == zone && getMSTime() < rt.emptyZoneUntil);
    }
    // The zone the bot stands in keeps killing it (it died in it twice: the zone rests for deaths). Not a zone it merely
    // wrote off as having nothing it can safely get to: two bots in the Blasted Lands wrote the zone off, picked another
    // one a second later, found no way there and used their hearthstone at once (round 126).
    bool KeepsKillingHere(Player* player, BotRuntime const& rt)
    {
        return rt.emptyZoneDeaths && rt.emptyZone == player->GetZoneId() && getMSTime() < rt.emptyZoneUntil;
    }
    // A way to a place to gather at that ends within this time was refused at its gate; this many such ways one after
    // the other pause the gathering ways for this long.
    constexpr uint32 GatherRefusalMs = 20 * IN_MILLISECONDS;
    constexpr uint8 GatherRefusalsMax = 3;
    constexpr uint32 GatherPauseMs = 10 * MINUTE * IN_MILLISECONDS;
    // A place a bot stands still at for minutes (a fire, an anvil, a forge): hostile spawns within this many yards
    // count against it, whatever their level, and with more than this many it is not used.
    constexpr float QuietPlaceYards = 45.0f;
    constexpr uint32 QuietPlaceMostHostile = 2;
    constexpr float GatherPlaceReached = 12.0f;
    constexpr std::size_t ProfessionPlaceChecks = 40;
    // A corpse to skin: the bot stands this close (the skinning spell reaches 5 yd).
    constexpr float SkinStandDistance = 3.0f;
    // Auto loot leaves everything alone with fewer free bag slots than this (Storage/AutoLoot.cpp).
    constexpr uint32 GatherMinFreeSlots = 4;
    constexpr uint32 GatherPlaceGapMs = 10 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 ProfessionModeCraftGapMs = 30 * IN_MILLISECONDS;
    constexpr uint32 ProfessionModeBatch = 20;
    // Below this many free bag slots a bot in profession mode puts its goods into the guild storage.
    constexpr uint32 ProfessionModeFreeSlots = 12;
    // A flight does not land in a zone (other than the one the bot wants to reach) whose level range starts more than
    // this above the bot.
    constexpr uint8 FlightZoneMargin = 5;
    // More than this in one planner tick (a second) is a jump, not a walk or a ride.
    constexpr float JumpDistance = 120.0f;
    // AUTO zone choice: a ship leg counts as this many yards of walking, so a zone across the sea wins only when
    // nothing at home fits; a zone is worth moving to with at least this many eligible quests.
    constexpr float VoyageLegCost = 6000.0f;
    constexpr uint32 MinQuestsToMove = 3;
    // A zone across the sea needs more of a reason, and only when no zone on the bot's side has enough quests; a bot
    // that has gone ashore does not sail again within this time; a zone left because nothing in it could be got to
    // safely waits this long. (Live round 48: four bots spent 25 of 27 minutes on ships between the continents.)
    // (Eight quests, were five: a crossing takes four to twelve minutes, and round 51 had a bot sail twelve for six.)
    constexpr uint32 MinQuestsToCross = 8;
    constexpr uint32 ZoneRestMs = 45 * MINUTE * IN_MILLISECONDS;
    // The flight master of the port a bot stands in is its way on, whatever the levels of the zone around the port.
    constexpr float PortFlightYards = 400.0f;
    // A bot within this many levels of a zone's lowest level has just grown into it: zones it is further into come first.
    constexpr uint8 ZoneComfortLevels = 2;
    // Flights: only for walks longer than this; the walk to the flight master plus from the landing spot to the
    // destination must save at least this share of the way; looked for once a minute.
    constexpr float FlightMinDistance = 1500.0f;
    constexpr float FlightMaxWalkShare = 0.5f;
    // A journey to a task's far spot arrives this near to it; one journey in this time.
    constexpr float JourneyArriveDistance = 250.0f;
    constexpr uint32 JourneyGapMs = 5 * MINUTE * IN_MILLISECONDS;
    // The landings a flight looks at (the nearest to the destination first), and how near to a taxi node its flight
    // master stands.
    constexpr std::size_t FlightLandings = 3;
    constexpr float LandingMasterReach = 80.0f;
    constexpr uint32 FlightCheckMs = MINUTE * IN_MILLISECONDS;
    // Flight masters the bot does not know yet are visited when they are this close.
    constexpr float DiscoverDistance = 120.0f;
    constexpr uint32 DiscoverCheckMs = MINUTE * IN_MILLISECONDS;
    // The planner's walk to an objective's spot ends this close to it; NewRpg works the objective from there.
    constexpr float ObjectiveApproachDone = 150.0f;
    // A flight is booked (handed to NewRpg) when the bot stands next to the flight master, on its level (Stormwind's
    // stands on the wall, 27 yd above the gate): NewRpg's own walk to it is one straight move. From farther away the
    // task walks there first.
    constexpr float FlightBookDistance = 20.0f;
    constexpr float FlightBookHeight = 6.0f;
    // A bot this close to the flight master of its booked flight has reached it (NewRpg talks to it from 5 yd).
    constexpr float FlightMasterReach = 30.0f;
    // How long a booked flight is waited for at the master: NewRpg drops its TRAVEL_FLIGHT status in the moment it asks
    // for the flight, and with instant flight paths the core's teleport and playerbots' answer to it follow a moment
    // later (Hordeboss stood at the master of Bloodvenom Post for four seconds after its booking).
    constexpr uint32 FlightTeleportWaitMs = 8 * IN_MILLISECONDS;
    // At the server's level cap a bot without quests waits at the inn and looks at the zones again this often.
    constexpr uint32 CapIdleRecheckMs = 5 * MINUTE * IN_MILLISECONDS;
    // Rates: one sample a minute, an hour of history, at least five minutes before a rate is shown.
    constexpr uint32 SampleGapSec = 60;
    constexpr uint32 SampleWindowSec = HOUR;
    constexpr uint32 MinRateSpanSec = 5 * MINUTE;

    float Dist2d(Player const* player, float x, float y)
    {
        float const dx = player->GetPositionX() - x;
        float const dy = player->GetPositionY() - y;
        return std::sqrt(dx * dx + dy * dy);
    }

    // A quest whose tasks end this often in a row, within this time, for what stood in their way and without any
    // progress waits for SpotRetryMs: another quest, or another zone, comes first.
    constexpr uint8 QuestMissLimit = 2;
    constexpr uint32 QuestMissWindowMs = 10 * MINUTE * IN_MILLISECONDS;
    // A travel (to a zone, to the ender of an errand) begun more often than this within the window without getting
    // this much nearer is given up for a while.
    constexpr uint8 TravelRestartLimit = 3;
    constexpr uint32 TravelRestartWindowMs = 10 * MINUTE * IN_MILLISECONDS;
    constexpr float TravelRestartProgress = 150.0f;
    // A bot that is this near to where the first of those starts was has not got away from its place: the place holds
    // it (packs on every side), whatever zone it wants to go to. It waits this long when its hearthstone is not ready.
    constexpr float BoxedInYards = 150.0f;
    constexpr uint32 BoxedInWaitMs = 60 * IN_MILLISECONDS;
    // A route's first node the bot did not get to for what stood before it, this near to one noted before, counts
    // again; the second time within the time it is no start of the bot's routes for that long.
    constexpr float RefusedStartYards = 40.0f;
    constexpr uint32 RefusedStartMs = 30 * MINUTE * IN_MILLISECONDS;
    constexpr uint8 RefusedStartTimes = 2;
    // AUTO: after this many holds in a row (HoldReplanMs each) the zone counts as empty for this long.
    constexpr uint8 IdleHoldLimit = 2;
    constexpr uint32 EmptyZoneMs = 15 * MINUTE * IN_MILLISECONDS;

    // How far a quest in the log has got: its counters added up (kills, objects, items), and whether it is complete.
    uint32 QuestMark(Player* player, uint32 questId)
    {
        QuestStatusMap& statusMap = player->getQuestStatusMap();
        auto itr = statusMap.find(questId);
        if (itr == statusMap.end())
            return 0;
        uint32 mark = itr->second.Status == QUEST_STATUS_COMPLETE ? 100000 : 1;
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            mark += itr->second.CreatureOrGOCount[i];
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            mark += itr->second.ItemCount[i];
        return mark;
    }

    // Whether a task ended for what stood in its way (the outcomes ClearTask is given by the risk checks), not for
    // anything about its target.
    // The end of a task whose way was refused (the walk gate, a route, the walk to a dock).
    bool IsRefusedWay(char const* outcome)
    {
        for (char const* refused : { "way too dangerous", "a group on the way", "a place set aside on the way", "place above its level",
                 "route too dangerous", "no way on the ground",
                 "kept away from a creature far above its level" })
            if (!std::strcmp(outcome, refused))
                return true;
        return false;
    }

    // The end of a task whose target no way led to (the path finder had none, or none to its level): counted with the
    // refused ways at the place the bot stands. A level-60 mage put onto Acherus, 215 yd above the Eastern Plaguelands,
    // ended 120 grind tasks there in a row "grind spot not reached", one every seven seconds, without a fight, and was
    // never boxed in by the count of refused ways, round 200.
    bool IsNoWayEnd(char const* outcome)
    {
        for (char const* noWay : { "grind spot not reached", "objective spot not reached", "spot not reached", "no way to its level" })
            if (!std::strcmp(outcome, noWay))
                return true;
        return false;
    }

    bool IsBlockedEnd(char const* outcome)
    {
        // (A rescue counts too: the "Pearl Diving" clams lie on the sea floor, and a hunter was taken out of the
        // water to the graveyard every 25 to 75 s and swam to the next clam, five times in ten minutes.)
        for (char const* blocked : { "a pack on its way", "a pull it cannot afford", "left a fight", "way too dangerous",
                 "kept away from a creature far above its level", "a group on the way", "a place set aside on the way",
                 "rescued", "rescued: hearthstone", "rescued: graveyard", "a foe that does not die",
                 // (A shaman on a cliff of Feralas had the Sea Sprays of quest 7721 set aside one after the other, every
                 // 30 s: each spawn alone "on a level the path finder has no way to".)
                 "no way to its level" })
            if (!std::strcmp(outcome, blocked))
                return true;
        return false;
    }

    // Bags (the maintainers, 2026-11-14, from the storage work's bag check: one of the owner's new recruits, level 12,
    // had no bag at all, sixteen slots full of quest items and two Skinning Knives; nothing gave a guild bot bags). A
    // character with an empty bag slot puts the largest plain bag it can use on: out of the guild storage first, else
    // bought at a vendor it visits, within the guild's budget (GuildBudget).
    uint8 EmptyBagSlot(Player* player)
    {
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
            if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                return slot;
        return 0;
    }

    bool IsPlainBag(ItemTemplate const* proto)
    {
        return proto && proto->Class == ITEM_CLASS_CONTAINER && proto->SubClass == ITEM_SUBCLASS_CONTAINER && proto->ContainerSlots > 0;
    }

    std::string TakeBagFromStorage(Player* player)
    {
        uint8 const slot = EmptyBagSlot(player);
        uint32 const owner = slot ? sStorageService.GetOwnerOf(player) : 0;
        if (!owner)
            return std::string();
        ItemTemplate const* best = nullptr;
        int32 bestProperty = 0;
        for (StorageItem const& stored : sStorageService.List(owner))
        {
            if (stored.itemClass != ITEM_CLASS_CONTAINER || stored.subClass != ITEM_SUBCLASS_CONTAINER || !stored.amount)
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(stored.entry);
            if (!IsPlainBag(proto) || player->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;
            if (!best || proto->ContainerSlots > best->ContainerSlots)
            {
                best = proto;
                bestProperty = stored.randomProperty;
            }
        }
        uint16 dest = 0;
        if (!best || player->CanEquipNewItem(slot, dest, best->ItemId, false) != EQUIP_ERR_OK)
            return std::string();
        // Straight into the bag slot (a full backpack takes nothing out of the storage).
        if (!sStorageService.Remove(owner, best->ItemId, bestProperty, 1))
            return std::string();
        if (!player->EquipNewItem(dest, best->ItemId, true))
        {
            sStorageService.Add(owner, best, bestProperty, 1);
            return std::string();
        }
        sStorageService.Touch(player);
        return Acore::StringFormat("{} ({} slots)", best->Name1, uint32(best->ContainerSlots));
    }

    std::string BuyBagAt(Player* player, Creature* vendor)
    {
        uint8 const slot = vendor ? EmptyBagSlot(player) : 0;
        VendorItemData const* items = slot ? vendor->GetVendorItems() : nullptr;
        if (!items)
            return std::string();
        ItemTemplate const* best = nullptr;
        uint32 bestSlot = 0;
        for (uint32 i = 0; i < items->GetItemCount(); ++i)
        {
            VendorItem const* item = items->GetItem(i);
            if (!item || item->ExtendedCost)
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->item);
            if (!IsPlainBag(proto) || player->CanUseItem(proto) != EQUIP_ERR_OK || !proto->BuyPrice)
                continue;
            if (!best || proto->ContainerSlots > best->ContainerSlots)
            {
                best = proto;
                bestSlot = i;
            }
        }
        if (!best)
            return std::string();
        uint32 const price = std::max<uint32>(1, uint32(std::floor(best->BuyPrice * player->GetReputationPriceDiscount(vendor))));
        GuildBudget::Grant grant;
        bool booked = false;
        if (player->GetMoney() < price)
        {
            grant = GuildBudget::Request(player, price);
            booked = grant.ok;
            if (!grant.ok)
                return Acore::StringFormat("no {} ({}): it cannot pay it and the guild's budget does not allow it", best->Name1,
                    GuildBudget::MoneyText(price));
        }
        uint32 const moneyBefore = player->GetMoney();
        player->BuyItemFromVendorSlot(vendor->GetGUID(), bestSlot, best->ItemId, 1, INVENTORY_SLOT_BAG_0, slot);
        Item const* put = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!put || put->GetEntry() != best->ItemId)
        {
            if (booked)
                GuildBudget::Cancel(player, price, grant);
            return std::string();
        }
        uint32 const paid = moneyBefore > player->GetMoney() ? moneyBefore - player->GetMoney() : 0;
        return Acore::StringFormat("{} ({} slots; {}{})", best->Name1, uint32(best->ContainerSlots), GuildBudget::MoneyText(paid),
            booked ? ", the guild helped to pay" : "");
    }

    // The server's level cap (worldserver MaxPlayerLevel): no experience beyond it.
    bool IsAtLevelCap(Player const* player)
    {
        return player->GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    }

    // At the level cap with no quest left (the owner, 2026-11-21: [quote left out]):
    // the profession mode while a primary profession of its plan is not learned or below its target (300 when the plan
    // names none); when the mode finds nothing to do it hands back to the autopilot, which grinds creatures of level 50 to
    // 60 and looks at the professions again after CapCraftRetryMs (what it grinds brings cloth, leather and the rest).
    constexpr uint32 CapCraftRetryMs = 30 * MINUTE * IN_MILLISECONDS;
    constexpr int32 CapGrindLevelsBelow = 10;
    constexpr float CapGrindYards = 1500.0f;
    std::unordered_map<ObjectGuid, uint32> capCraftRetry; // bot -> getMSTime(): not into the profession mode before

    // An engineer looks at the guild's ammunition this often (Professions::AmmoForGuild), at a hub after a hand-in, in
    // the profession mode and, at the level cap, before it grinds.
    constexpr uint32 AmmoLookMs = 15 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 AmmoCapBatch = 10;
    // One engineer of a guild at a time: the first that sets out for the guild's ammunition makes it, the others go on
    // with what they do. (Three of the guild's engineers at the level cap went into the profession mode in the same
    // second for 1040 rounds, each walking to an anvil 1100 to 1700 yd away.)
    constexpr uint32 AmmoClaimMs = 30 * MINUTE * IN_MILLISECONDS;
    std::unordered_map<uint32, std::pair<ObjectGuid, uint32>> ammoMaker; // guild owner -> its engineer, until (getMSTime)

    bool AmmoClaimedByOther(AdventureGuild const& guild, ObjectGuid bot, uint32 now)
    {
        auto const claim = ammoMaker.find(guild.ownerAccount);
        return claim != ammoMaker.end() && claim->second.first != bot && static_cast<int32>(claim->second.second - now) > 0;
    }

    // Endgame consumables for the guild's dungeon runs (Professions::ConsumablesForGuild): a crafter looks this often, at
    // a hub after a hand-in, in the profession mode and, at the level cap, before it grinds; one crafter of a guild at a
    // time goes into the profession mode for them (as for ammunition).
    constexpr uint32 ConsumableLookMs = 15 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 ConsumableCapBatch = 5;
    constexpr uint32 ConsumableClaimMs = 30 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 ConsumableSkipMs = HOUR * IN_MILLISECONDS;
    std::unordered_map<uint32, std::pair<ObjectGuid, uint32>> consumableMaker; // guild owner -> its crafter, until (getMSTime)

    bool ConsumablesClaimedByOther(AdventureGuild const& guild, ObjectGuid bot, uint32 now)
    {
        auto const claim = consumableMaker.find(guild.ownerAccount);
        return claim != consumableMaker.end() && claim->second.first != bot && static_cast<int32>(claim->second.second - now) > 0;
    }

    void ReleaseConsumableClaim(AdventureGuild const& guild, ObjectGuid bot)
    {
        if (auto const claim = consumableMaker.find(guild.ownerAccount); claim != consumableMaker.end() && claim->second.first == bot)
            consumableMaker.erase(claim);
    }

    // The highest level of the guild's characters (its roster and the leader): what its endgame consumables are for.
    uint8 GuildTopLevel(AdventureGuild const& guild)
    {
        uint8 top = 0;
        auto consider = [&](ObjectGuid guid)
        {
            Player const* member = ObjectAccessor::FindConnectedPlayer(guid);
            top = std::max<uint8>(top, member ? member->GetLevel() : uint8(sCharacterCache->GetCharacterLevelByGuid(guid)));
        };
        for (auto const& [guid, member] : guild.bots)
            if (member.state == BotState::Active)
                consider(guid);
        consider(guild.leader);
        return top;
    }

    // Raw materials processed for the guild (Professions::ProcessForGuild): a crafter looks this often.
    constexpr uint32 ProcessLookMs = 15 * MINUTE * IN_MILLISECONDS;
    // One crafter of a guild at a time processes them (as for ammunition and consumables): whenever the miners' ore brought
    // the storage 20 casts above the line, every capped smelter went into the profession mode for it, two at a time
    // smelted the same 20 bars and the rest found nothing and stood two minutes at an inn (Lilwar and Warriorio at the
    // same forge at 02:42, 830 such switches in a day, 2026-11-27).
    constexpr uint32 ProcessClaimMs = 30 * MINUTE * IN_MILLISECONDS;
    std::unordered_map<uint32, std::pair<ObjectGuid, uint32>> processMaker; // guild owner -> its crafter, until (getMSTime)

    bool ProcessClaimedByOther(AdventureGuild const& guild, ObjectGuid bot, uint32 now)
    {
        auto const claim = processMaker.find(guild.ownerAccount);
        return claim != processMaker.end() && claim->second.first != bot && static_cast<int32>(claim->second.second - now) > 0;
    }

    void ReleaseProcessClaim(AdventureGuild const& guild, ObjectGuid bot)
    {
        if (auto const claim = processMaker.find(guild.ownerAccount); claim != processMaker.end() && claim->second.first == bot)
            processMaker.erase(claim);
    }

    // A look of a capped bot for the guild's ammunition, consumables, materials or enchantments that takes this long
    // gets a line: such a look runs in one step of the planner, on the world thread (Magefrost's steps at the level cap
    // took 50 to 97 ms every quarter of an hour, 2026-11-27).
    class SlowLook
    {
    public:
        SlowLook(char const* what, std::string const& name) : _what(what), _name(name), _start(Perf::NowUs()) { }
        ~SlowLook()
        {
            constexpr uint64 LineUs = 20000;
            uint64 const us = Perf::NowUs() - _start;
            if (us >= LineUs)
                LOG_INFO(ActivityLog, "activity: {} looked for {} at the level cap for {} ms", _name, _what, us / 1000);
        }
        SlowLook(SlowLook const&) = delete;
        SlowLook& operator=(SlowLook const&) = delete;

    private:
        char const* _what;
        std::string const& _name;
        uint64 _start;
    };

    // Enchantments for the guild's gear (Professions::EnchantScrollForGuild, UseEnchantScroll): an enchanter looks this
    // often for a scroll to write, a guild character for a scroll of the guild storage to put on.
    constexpr uint32 ScrollLookMs = 10 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 ScrollUseMs = 5 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 VellumOrder = 10;

    // The guild's characters the AI plays, in the world and alive: whose gear gets enchantments.
    std::vector<Player*> GuildEnchantWearers(AdventureGuild const& guild)
    {
        std::vector<Player*> wearers;
        auto consider = [&](ObjectGuid guid)
        {
            Player* member = ObjectAccessor::FindPlayer(guid);
            if (member && member->IsInWorld() && member->IsAlive() && BotDriver::GetAI(member))
                wearers.push_back(member);
        };
        for (auto const& [guid, member] : guild.bots)
            consider(guid);
        consider(guild.leader);
        return wearers;
    }

    // The consumables a crafter's casts failed for (twice in a row no more of it after a batch): not for an hour.
    std::unordered_set<uint32> ConsumableSkips(Player* player, BotRuntime& rt, uint32 owner, uint32 now)
    {
        if (rt.consumableItem)
        {
            uint32 const has = player->GetItemCount(rt.consumableItem, true) + sStorageService.GetAmount(owner, rt.consumableItem, 0);
            if (has <= rt.consumableHad)
            {
                if (++rt.consumableStrikes >= 2)
                {
                    rt.consumableSkip[rt.consumableItem] = now + ConsumableSkipMs;
                    rt.consumableStrikes = 0;
                }
            }
            else
                rt.consumableStrikes = 0;
            rt.consumableItem = 0;
        }
        std::unordered_set<uint32> skip;
        for (auto itr = rt.consumableSkip.begin(); itr != rt.consumableSkip.end();)
            if (static_cast<int32>(itr->second - now) <= 0)
                itr = rt.consumableSkip.erase(itr);
            else
                skip.insert((itr++)->first);
        return skip;
    }

    // The guild's hunters in the world whose weapon is a gun: the ones an engineer makes ammunition for.
    std::vector<Player*> GunHunters(AdventureGuild const& guild)
    {
        std::vector<Player*> shooters;
        auto consider = [&](ObjectGuid guid)
        {
            if (Player* member = ObjectAccessor::FindConnectedPlayer(guid); member && member->IsInWorld() &&
                member->getClass() == CLASS_HUNTER && Ammo::KindOf(member) == ITEM_SUBCLASS_BULLET)
                shooters.push_back(member);
        };
        for (auto const& [guid, member] : guild.bots)
            if (member.state == BotState::Active)
                consider(guid);
        consider(guild.leader);
        return shooters;
    }

    bool CapProfessionWorkLeft(GuildBot const& bot, Player const* player)
    {
        for (std::size_t i = 0; i < 2; ++i)
            if (uint32 const skill = bot.professions.primary[i])
                if (!player->HasSkill(skill) || Professions::HasWork(bot, player, skill))
                    return true;
        return false;
    }

    float Dist2d(Player const* player, SpawnPoint const& spot)
    {
        return Dist2d(player, spot.x, spot.y);
    }

    bool IsAtFlightMaster(Player const* player, SpawnPoint const& spot)
    {
        return Dist2d(player, spot) <= FlightBookDistance && std::fabs(player->GetPositionZ() - spot.z) <= FlightBookHeight;
    }

    // Distances in "why" lines change in steps of 250 yards only, so the roster is not pushed a new line every second.
    std::string RoughDistance(float distance)
    {
        if (distance < 250.0f)
            return "almost there";
        return Acore::StringFormat("about {} yd", (uint32(distance) + 125) / 250 * 250);
    }

    uint32 FreeQuestSlots(Player const* player)
    {
        uint32 free = 0;
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (!player->GetQuestSlotQuestId(slot))
                ++free;
        return free;
    }

    uint64 TotalXp(Player const* player)
    {
        uint64 total = player->GetUInt32Value(PLAYER_XP);
        for (uint8 level = 1; level < player->GetLevel(); ++level)
            total += sObjectMgr->GetXPForLevel(level);
        return total;
    }

    // Food or drink items in the bags the bot can use at its level.
    uint32 CountConsumables(Player const* player, uint32 spellCategory)
    {
        uint32 count = 0;
        auto add = [&](Item const* item)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            if (proto && proto->Spells[0].SpellCategory == spellCategory && proto->RequiredLevel <= player->GetLevel())
                count += item->GetCount();
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            add(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag const* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    add(bag->GetItemByPos(uint8(slot)));
        return count;
    }

    // Food and drink: up to one stack of each, the best kind the vendor sells for the bot's level that the bot can
    // pay, for at most GuildAdventure.Provisions.BudgetPercent of its money (real price, through the core's vendor
    // purchase). Returns what it bought, e.g. "20 Tough Jerky".
    std::string BuyProvisions(Player* player, Creature* vendor)
    {
        VendorItemData const* items = vendor ? vendor->GetVendorItems() : nullptr;
        Settings const settings = sBotSettings.For(player);
        if (!items || player->GetLevel() < settings.provisionsMinLevel || player->GetMoney() < settings.provisionsMinMoney)
            return "";

        std::string bought;
        std::vector<uint32> categories = { FoodSpellCategory };
        if (player->getPowerType() == POWER_MANA)
            categories.push_back(DrinkSpellCategory);
        uint32 budget = uint32(uint64(player->GetMoney()) * settings.provisionsBudgetPercent / 100);
        for (std::size_t i = 0; i < categories.size(); ++i)
        {
            uint32 const category = categories[i];
            // Nothing a mage conjures itself, and no food the guild storage has for the bot (it takes a piece from
            // there when it eats: StorageSupply::TakeOne; the cooks make it for that).
            // Drink is bought whatever the storage holds (the owner, 2026-11-14: [quote left out]): the storage
            // bridges the way to the next town, it is not the supply. With "none while the storage has some" thirty
            // bots drank it dry a piece at a time, each bought nothing in town because a few pieces lay there at
            // that moment, and the guild leader sat out its mana for three minutes at a time with 37 gold on it.
            StorageSupply::Kind const kind = category == DrinkSpellCategory ? StorageSupply::Kind::Drink : StorageSupply::Kind::Food;
            if (StorageSupply::Conjures(player, kind) || (category != DrinkSpellCategory && StorageSupply::Has(player, kind)))
                continue;
            uint32 const have = CountConsumables(player, category);
            uint32 const stack = category == DrinkSpellCategory ? settings.provisionsDrinkStack : settings.provisionsStack;
            if (have >= stack)
                continue;
            // What is left of the budget is shared between the kinds still to buy.
            uint32 const share = budget / uint32(categories.size() - i);

            // The best kind for the level of which the share pays at least one bundle.
            uint32 bestSlot = 0;
            ItemTemplate const* best = nullptr;
            for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
            {
                VendorItem const* item = items->GetItem(slot);
                ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
                if (!proto || item->ExtendedCost || !proto->BuyPrice || proto->Spells[0].SpellCategory != category ||
                    proto->RequiredLevel > player->GetLevel() || player->CanUseItem(proto) != EQUIP_ERR_OK ||
                    proto->BuyPrice > share)
                    continue;
                if (!best || proto->RequiredLevel > best->RequiredLevel)
                {
                    best = proto;
                    bestSlot = slot;
                }
            }
            if (!best)
                continue;

            uint32 const bundle = std::max<uint32>(1, best->BuyCount);
            uint32 const bundles = std::min<uint32>((stack - have + bundle - 1) / bundle, share / best->BuyPrice);
            if (!bundles)
                continue;
            // (The core's return value only says whether the vendor's list changed: the money says whether it bought.)
            uint32 const before = player->GetMoney();
            player->BuyItemFromVendorSlot(vendor->GetGUID(), bestSlot, best->ItemId, uint8(bundles), NULL_BAG, NULL_SLOT);
            if (player->GetMoney() < before)
            {
                bought += Acore::StringFormat("{}{} {}", bought.empty() ? "" : ", ", bundles * bundle, best->Name1);
                budget -= std::min(budget, before - std::min(before, player->GetMoney()));
            }
        }
        return bought;
    }

    // A quest item vendors sell: buys up to `count` of it at the real price. Returns how many the bot got.
    uint32 BuyQuestItem(Player* player, Creature* vendor, uint32 itemId, uint32 count)
    {
        VendorItemData const* items = vendor ? vendor->GetVendorItems() : nullptr;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!items || !proto || !count)
            return 0;

        uint32 const before = player->GetItemCount(itemId);
        for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
        {
            VendorItem const* item = items->GetItem(slot);
            if (!item || item->item != itemId || item->ExtendedCost)
                continue;
            uint32 const bundle = std::max<uint32>(1, proto->BuyCount);
            // All at once; a vendor with a limited stock sells what it has, one bundle at a time. Counted by the bags:
            // the core answers false for an item of unlimited stock although it sold it (a second round bought more).
            uint32 const bundles = (count + bundle - 1) / bundle;
            player->BuyItemFromVendorSlot(vendor->GetGUID(), slot, itemId, uint8(std::min<uint32>(bundles, 255)), NULL_BAG, NULL_SLOT);
            for (uint32 left = bundles; left && player->GetItemCount(itemId) - before < count; --left)
            {
                uint32 const had = player->GetItemCount(itemId);
                player->BuyItemFromVendorSlot(vendor->GetGUID(), slot, itemId, 1, NULL_BAG, NULL_SLOT);
                if (player->GetItemCount(itemId) <= had)
                    break; // no money, no room, no stock
            }
            break;
        }
        return player->GetItemCount(itemId) - before;
    }

    // Whether the vendor of that creature entry sells food or drink (spell category) the bot can use at its level.
    bool VendorSells(uint32 entry, uint32 spellCategory, Player const* player)
    {
        VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(entry);
        if (!items)
            return false;
        for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
        {
            VendorItem const* item = items->GetItem(slot);
            ItemTemplate const* proto = item ? sObjectMgr->GetItemTemplate(item->item) : nullptr;
            if (proto && !item->ExtendedCost && proto->BuyPrice && proto->Spells[0].SpellCategory == spellCategory &&
                proto->RequiredLevel <= player->GetLevel())
                return true;
        }
        return false;
    }

    // Fewer than `below` pieces of food or, using mana, of drink, and no other source: a mage conjures its own, and
    // a bot whose guild storage has some takes a piece from there when it eats or drinks.
    bool NeedsFood(Player* player, uint32 below)
    {
        return CountConsumables(player, FoodSpellCategory) < below && !StorageSupply::Conjures(player, StorageSupply::Kind::Food) &&
            !StorageSupply::Has(player, StorageSupply::Kind::Food);
    }

    bool NeedsDrink(Player* player, uint32 below)
    {
        return player->getPowerType() == POWER_MANA && CountConsumables(player, DrinkSpellCategory) < below &&
            !StorageSupply::Conjures(player, StorageSupply::Kind::Drink) && !StorageSupply::Has(player, StorageSupply::Kind::Drink);
    }

    // Fewer than `below` drinks in the bags of a character that uses mana and conjures none: at a town it buys,
    // whatever the guild storage holds (BuyProvisions).
    bool CarriesFewDrinks(Player* player, uint32 below)
    {
        return player->getPowerType() == POWER_MANA && CountConsumables(player, DrinkSpellCategory) < below &&
            !StorageSupply::Conjures(player, StorageSupply::Kind::Drink);
    }

    uint32 MinDurabilityPercent(Player const* player)
    {
        uint32 lowest = 100;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            uint32 const max = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            if (!max)
                continue;
            lowest = std::min(lowest, item->GetUInt32Value(ITEM_FIELD_DURABILITY) * 100 / max);
        }
        return lowest;
    }

    std::string ZoneName(uint32 zoneId)
    {
        if (ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(zoneId))
            return zone->name;
        std::string const name = GetAreaName(zoneId);
        return name.empty() ? Acore::StringFormat("zone {}", zoneId) : name;
    }

    std::string QuestTitle(uint32 questId)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        return quest ? quest->GetTitle() : Acore::StringFormat("quest {}", questId);
    }

    std::string SpawnName(SpawnPoint const& spawn)
    {
        if (spawn.gameObject)
        {
            GameObjectTemplate const* goinfo = sObjectMgr->GetGameObjectTemplate(spawn.entry);
            return goinfo ? goinfo->name : "an object";
        }
        CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(spawn.entry);
        return cinfo ? cinfo->Name : "someone";
    }

    bool CanTalkTo(SpawnPoint const& spawn, Player const* player)
    {
        if (spawn.gameObject)
            return true;
        CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(spawn.entry);
        return cinfo && WorldIndex::IsFriendlyFaction(cinfo->faction, player);
    }

    // One of the given spawns that stands in the zone and that the player can talk to, wherever the player is (a zone
    // across the sea is judged from afar). Zones are looked up for a few candidates only.
    std::optional<SpawnPoint> SpawnInZone(std::vector<SpawnPoint> const& spawns, Player const* player, uint32 zoneId, BotRuntime const& rt)
    {
        constexpr std::size_t MaxZoneChecks = 8;
        std::size_t checks = 0;
        for (SpawnPoint const& spawn : spawns)
        {
            if (rt.IsAvoided(spawn) || !CanTalkTo(spawn, player))
                continue;
            if (checks++ >= MaxZoneChecks)
                break;
            if (sWorldIndex.GetSpawnZone(spawn) == zoneId)
                return spawn;
        }
        return std::nullopt;
    }

    // Whether the creature or the object of a spawn is down right now and will still be when the bot gets there. The
    // map knows while the grid around the spawn is loaded: a dead creature's object stays until it respawns, a taken
    // chest's too; and a spawn that has no object at all there is a place of a pool that is not in use (a rare with
    // five places and none of them taken, a herb's or a vein's other places). A grid that is not loaded: unknown,
    // which counts as up.
    // (The planner chose the nearest spawn whatever its state. A paladin that had just emptied a nest of screechers
    // was sent to four of their spawns in a row, 7 to 60 yd apart, and stood twelve seconds at each before the task
    // ended "no target at this spawn": live round 54. A third of the objective tasks of an hour ended that way.)
    // Quest NPCs the guild's characters could not get to (a library behind a hole in the navmesh, a giver on a broken
    // bridge, a caravan on the move). Each bot left such an NPC alone for half an hour and walked there again, and so
    // did every other bot: 98 pickups and hand-ins of one day ended that way, 31 of them at one librarian. After three
    // failures of guild characters within a day the NPC is left alone by all of them for a day (ga_guild_npc_block,
    // kept across restarts; world thread).
    struct NpcBlock
    {
        uint8 fails = 0;
        time_t lastAt = 0;
        time_t until = 0;
    };
    std::unordered_map<uint32, std::unordered_map<uint64, NpcBlock>> sNpcBlocks; // by owner account, then spawn key
    constexpr uint8 NpcBlockFails = 3;
    constexpr time_t NpcBlockSeconds = DAY;

    uint64 NpcKey(SpawnPoint const& spawn) { return (uint64(spawn.gameObject ? 1 : 0) << 32) | uint64(spawn.spawnId); }

    std::unordered_map<uint64, NpcBlock> const* NpcBlocksOf(Player const* player)
    {
        if (sNpcBlocks.empty())
            return nullptr;
        AdventureGuild const* guild = sGuildAdventureMgr.FindGuildOfMember(player->GetGUID());
        auto blocks = guild ? sNpcBlocks.find(guild->ownerAccount) : sNpcBlocks.end();
        return blocks != sNpcBlocks.end() ? &blocks->second : nullptr;
    }

    bool IsBlockedIn(std::unordered_map<uint64, NpcBlock> const* blocks, SpawnPoint const& spawn)
    {
        if (!blocks || !spawn.spawnId)
            return false;
        auto block = blocks->find(NpcKey(spawn));
        return block != blocks->end() && block->second.until > GameTime::GetGameTime().count();
    }

    // One more failure for the way at this NPC. True when it made the guild leave the NPC alone.
    bool AddNpcFailure(uint32 owner, SpawnPoint const& spawn, uint32 weight = 1)
    {
        if (!spawn.spawnId)
            return false;
        time_t const now = GameTime::GetGameTime().count();
        NpcBlock& block = sNpcBlocks[owner][NpcKey(spawn)];
        if (block.lastAt + NpcBlockSeconds < now)
            block.fails = 0;
        block.lastAt = now;
        block.fails = uint8(std::min<uint32>(uint32(block.fails) + weight, 250));
        bool const blocks = block.fails >= NpcBlockFails && block.until <= now;
        if (blocks)
            block.until = now + NpcBlockSeconds;
        CharacterDatabase.Execute("REPLACE INTO ga_guild_npc_block (owner_account, spawn_id, is_gameobject, fails, last_at, blocked_until) "
            "VALUES ({}, {}, {}, {}, {}, {})", owner, uint32(spawn.spawnId), spawn.gameObject ? 1 : 0, uint32(block.fails),
            uint32(block.lastAt), uint32(block.until));
        return blocks;
    }

    // A pickup or a hand-in was given up for the way to its NPC, or the walk to the spawn an objective or an
    // interaction is worked at (the Ancient Relics in the pit of Whelgar's Excavation: one bot, three objects, four
    // minutes each, and the next bot the same).
    // `journey`: the character had travelled for this hand-in (an errand) and the ground has no way to its ender. Once
    // is enough then: the next try is another journey (a level-28 hunter sailed from Auberdine to Stormwind for the
    // historian in the keep's library, where the path finder's ground ends at the door, sailed back, and set out again
    // seventeen minutes later).
    void NoteNpcUnreached(AdventureGuild const& guild, std::string const& who, BotTask const& task, bool journey = false)
    {
        bool const node = task.kind == TaskKind::Travel && task.gatherRound && !task.focus && !task.skinHunt && !task.huntItem &&
            !task.fishTrip;
        if (!task.hasSpot || !task.spot.spawnId || (task.kind != TaskKind::Pickup && task.kind != TaskKind::TurnIn &&
            task.kind != TaskKind::Objective && task.kind != TaskKind::Interact && !node))
            return;
        if (AddNpcFailure(guild.ownerAccount, task.spot, journey ? NpcBlockFails : 1))
            LOG_INFO(ActivityLog, "activity: the guild's characters could not get to {}{} at ({:.0f}, {:.0f}) {} (the "
                "last: {}{}): they all leave it alone for {} h", task.kind == TaskKind::Interact ? "the spot to " : "",
                task.targetName, task.spot.x, task.spot.y, journey ? std::string("after a journey for it, and the ground has no way there") :
                    Acore::StringFormat("{} times within a day", uint32(NpcBlockFails)), who,
                task.questId ? Acore::StringFormat(", quest {} \"{}\"", task.questId, QuestTitle(task.questId)) :
                    std::string(" (a node of its profession)"),
                NpcBlockSeconds / HOUR);
    }

    constexpr float DownLookYards = 400.0f;
    bool IsSpawnDown(Player const* player, SpawnPoint const& spawn)
    {
        if (!spawn.spawnId || spawn.mapId != player->GetMapId())
            return false;
        float const distance = Dist2d(player, spawn);
        if (distance > DownLookYards)
            return false;
        Map* map = player->GetMap();
        time_t respawn = 0;
        if (spawn.gameObject)
        {
            auto const bounds = map->GetGameObjectBySpawnIdStore().equal_range(spawn.spawnId);
            if (bounds.first == bounds.second)
                return map->IsGridLoaded(spawn.x, spawn.y);
            if (bounds.first->second->isSpawned())
                return false;
            respawn = bounds.first->second->GetRespawnTime();
        }
        else
        {
            auto const bounds = map->GetCreatureBySpawnIdStore().equal_range(spawn.spawnId);
            if (bounds.first == bounds.second)
                return map->IsGridLoaded(spawn.x, spawn.y);
            if (bounds.first->second->IsAlive())
                return false;
            respawn = bounds.first->second->GetRespawnTime();
        }
        // Up again by the time the bot is there (on foot, to be on the safe side) is as good as up.
        return respawn > GameTime::GetGameTime().count() + time_t(distance / 7.0f) + 5;
    }

    // The spawns without the ones that are down; `buffer` holds the copy when one had to be made.
    std::vector<SpawnPoint> const& WithoutDown(std::vector<SpawnPoint> const& spawns, Player const* player, std::vector<SpawnPoint>& buffer)
    {
        std::size_t first = 0;
        while (first < spawns.size() && !IsSpawnDown(player, spawns[first]))
            ++first;
        if (first == spawns.size())
            return spawns;
        buffer.assign(spawns.begin(), spawns.begin() + first);
        for (std::size_t i = first + 1; i < spawns.size(); ++i)
            if (!IsSpawnDown(player, spawns[i]))
                buffer.push_back(spawns[i]);
        return buffer;
    }

    // The nearest of the given spawns on the player's map, optionally only inside a zone and only ones the player can
    // talk to. Zones are looked up for the nearest few candidates only (terrain lookups, cached per spawn).
    std::optional<SpawnPoint> NearestSpawn(std::vector<SpawnPoint> const& spawns, Player const* player, uint32 zoneId,
        bool mustTalk, float maxDistance, BotRuntime const& rt)
    {
        std::vector<std::pair<float, SpawnPoint const*>> candidates;
        // (An NPC the guild could not get to is nobody's target for a day.)
        auto const* npcBlocks = NpcBlocksOf(player);
        for (SpawnPoint const& spawn : spawns)
        {
            if (spawn.mapId != player->GetMapId() || rt.IsAvoided(spawn) || IsBlockedIn(npcBlocks, spawn))
                continue;
            float const distance = Dist2d(player, spawn);
            if (distance > maxDistance || IsSpawnDown(player, spawn))
                continue;
            if (mustTalk && !CanTalkTo(spawn, player))
                continue;
            candidates.emplace_back(distance, &spawn);
        }

        std::sort(candidates.begin(), candidates.end(), [](auto const& a, auto const& b)
        {
            return a.first < b.first || (a.first == b.first && a.second->spawnId < b.second->spawnId);
        });

        // Only spots the bot can walk to: in its own region (and in the zone, when one is asked for).
        Region const region = GetRegion(player);
        constexpr std::size_t MaxZoneChecks = 8;
        std::size_t checks = 0;
        for (auto const& [distance, spawn] : candidates)
        {
            if (checks++ >= MaxZoneChecks)
                break;
            if (sWorldIndex.GetSpawnRegion(*spawn) != region)
                continue;
            if (!zoneId || sWorldIndex.GetSpawnZone(*spawn) == zoneId)
                return *spawn;
        }
        return std::nullopt;
    }

    ServicePoint const* NearestService(ServiceType type, Player const* player, BotRuntime const& rt, uint8 trainerClass = 0,
        std::function<bool(ServicePoint const&)> const& accept = {})
    {
        Region const region = GetRegion(player);
        std::vector<std::pair<float, ServicePoint const*>> candidates;
        for (ServicePoint const& service : sWorldIndex.GetServices(type))
        {
            if (service.spawn.mapId != player->GetMapId() || rt.IsAvoided(service.spawn))
                continue;
            if (trainerClass && service.trainerClass != trainerClass)
                continue;
            if (accept && !accept(service))
                continue;
            float const distance = Dist2d(player, service.spawn);
            if (distance > MaxServiceDistance)
                continue;
            if (!WorldIndex::IsFriendlyFaction(service.factionTemplate, player))
                continue;
            candidates.emplace_back(distance, &service);
        }

        std::sort(candidates.begin(), candidates.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        constexpr std::size_t MaxRegionChecks = 16;
        std::size_t checks = 0;
        // The best of those with a way the bot should rather not walk (below), by how bad the way is.
        ServicePoint const* lastChoice = nullptr;
        uint32 lastChoicePenalty = std::numeric_limits<uint32>::max();
        // By the walk, not by the straight line: a level-8 druid with full bags stood 187 yd from a vendor of the Valley
        // of Trials on the plateau beside it, the road there was 4683 yd through Razor Hill and back, and it died to a
        // scorpid on that walk (2026-11-24). A service whose walk is not much longer than the straight line is taken at
        // once; else the shortest walk of the first MaxWalkChecks with a good way; a service the bot has no way to at
        // all (neither the path finder nor the road map) is none.
        constexpr std::size_t MaxWalkChecks = 4;
        constexpr float WalkDetourOk = 2.0f;
        constexpr float WalkSlack = 100.0f;
        constexpr float WalkNear = 40.0f;
        auto walkYards = [player](SpawnPoint const& spot, float straight)
        {
            if (straight <= WalkNear)
                return straight;
            float length = 0.0f;
            if (WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), spot.x, spot.y, spot.z,
                &length))
                return length;
            RoadRoute const route = sRoadMap.FindRoute(player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
                player->GetPositionZ(), spot.x, spot.y, spot.z, player->GetTeamId(), player->GetLevel());
            return route.found ? route.length : -1.0f;
        };
        ServicePoint const* nearestWalk = nullptr;
        float nearestWalkYards = 0.0f;
        std::size_t walkChecks = 0;
        for (auto const& [distance, service] : candidates)
        {
            if (checks++ >= MaxRegionChecks || (nearestWalk && distance >= nearestWalkYards))
                break;
            if (sWorldIndex.GetSpawnRegion(service->spawn) != region)
                continue;
            // (Not the flight master the bot stands next to: a level-26 bot that had gone ashore in Booty Bay could
            // not fly on to Duskwood, because Stranglethorn Vale begins at 30; it was sent on foot, the way was refused,
            // and it sailed back.)
            ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(sWorldIndex.GetSpawnZone(service->spawn));
            if (zone && zone->minLevel > player->GetLevel() + ServiceZoneLevelMargin &&
                !(type == ServiceType::FlightMaster && distance <= PortFlightYards))
                continue;
            // The same risk rules as for a quest spot (RiskModel): not a vendor, trainer or innkeeper in a place that
            // is set aside after deaths or a lost fight, with creatures too strong for the bot around it, or (where the
            // module has the road map) behind a straight way through creatures far above it. Four of eight deaths of
            // one live window were "during vendor".
            if (!Risk::IsSafePlace(player, rt, service->spawn.mapId, service->spawn.x, service->spawn.y, service->spawn.z))
                continue;
            // The way there. One whose straight way leads back through a place the bot has just died at or run from (a
            // rogue that had left a gnoll camp twice was sent to the farmer behind it and died in the camp), or whose
            // way (the route of a long walk) passes creatures two or more levels above the bot (a level-16 rogue was
            // sent 520 yards to a fisherman past level-18 creatures and met three of them), is a later choice; one
            // behind creatures four or more levels above it is no choice.
            constexpr uint32 WayLevelsAbove = 2;
            constexpr uint32 WayLevelsRefused = 4;
            constexpr uint32 SetAsidePenalty = 3;
            bool const crosses = rt.CrossesAvoidedArea(service->spawn.mapId, player->GetPositionX(), player->GetPositionY(),
                service->spawn.x, service->spawn.y, player->GetLevel());
            uint32 const wayLevel = Risk::WayDanger(player, service->spawn.mapId, service->spawn.x, service->spawn.y, service->spawn.z);
            uint32 const above = wayLevel > player->GetLevel() ? wayLevel - player->GetLevel() : 0;
            if (!crosses && above < WayLevelsAbove)
            {
                float const walk = walkYards(service->spawn, distance);
                if (walk < 0.0f)
                    continue;
                if (walk <= distance * WalkDetourOk + WalkSlack && !nearestWalk)
                    return service;
                if (!nearestWalk || walk < nearestWalkYards)
                {
                    nearestWalk = service;
                    nearestWalkYards = walk;
                }
                if (++walkChecks >= MaxWalkChecks)
                    break;
                continue;
            }
            if (above >= WayLevelsRefused)
                continue;
            if (uint32 const penalty = above + (crosses ? SetAsidePenalty : 0); penalty < lastChoicePenalty)
            {
                lastChoice = service;
                lastChoicePenalty = penalty;
            }
        }
        return nearestWalk ? nearestWalk : lastChoice;
    }

    // Why a bot that should walk does not: what holds it, and where it stands relative to the ground. For the
    // activity log, so a standstill can be explained afterwards.
    std::string DescribeStandstill(Player* player)
    {
        std::string holds;
        auto add = [&](bool condition, char const* text)
        {
            if (condition)
                holds += (holds.empty() ? "" : ", ") + std::string(text);
        };
        add(player->HasUnitState(UNIT_STATE_STUNNED), "stunned");
        add(player->HasUnitState(UNIT_STATE_ROOT), "rooted");
        add(player->HasUnitState(UNIT_STATE_CONFUSED), "confused");
        add(player->HasUnitState(UNIT_STATE_FLEEING), "fleeing");
        add(player->HasUnitState(UNIT_STATE_CASTING), "casting");
        add(player->IsFalling(), "falling");
        add(player->IsSitState(), "sitting");
        add(player->isMoving(), "moving");
        add(IsSwimming(player), IsDiving(player) ? "under water" : "in water");
        add(player->IsMounted(), "mounted");
        add(player->GetTransport() != nullptr, "on a transport");
        add(player->IsBeingTeleported(), "being teleported");
        // (In a fight without a foe it can see: the spline of its steps ran and it did not move, "movement 0" being the
        // idle generator, round 239's Hordeboss for 3 min with the combat engine on.)
        if (player->IsInCombat())
        {
            Unit const* victim = player->GetVictim();
            holds += Acore::StringFormat("{}in combat ({} attackers{}{})", holds.empty() ? "" : ", ", uint32(player->getAttackers().size()),
                victim ? ", its target " : "", victim ? victim->GetName() : "");
        }
        if (!player->movespline->Finalized())
            holds += Acore::StringFormat("{}a spline {:.0f} yd long running", holds.empty() ? "" : ", ",
                player->GetExactDist(player->movespline->FinalDestination().x, player->movespline->FinalDestination().y,
                    player->movespline->FinalDestination().z));
        if (holds.empty())
            holds = "nothing holds it";

        float const z = player->GetPositionZ();
        float const floor = player->GetMap()->GetHeight(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(), z + 2.0f,
            true, 100.0f);
        float const surface = player->GetMap()->GetHeight(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(),
            MAX_HEIGHT, true, MAX_HEIGHT);
        std::string ground;
        if (floor <= INVALID_HEIGHT)
            ground = Acore::StringFormat("no floor under it (surface at {:.0f})", surface);
        else if (std::fabs(z - floor) > 2.5f)
            ground = Acore::StringFormat("{:.0f} yd above the floor under it (surface at {:.0f})", z - floor, surface);
        else
            ground = Acore::StringFormat("on the floor ({:.0f} yd below the surface)", std::max(0.0f, surface - z));
        return Acore::StringFormat("z {:.1f}, {}; {}; movement {}; rpg {}; ai {}", z, ground, holds,
            uint32(player->GetMotionMaster()->GetCurrentMovementGeneratorType()), ToString(BotDriver::GetRpgState(player)),
            BotDriver::AIState(player));
    }

    // The creature or game object of a spawn point in sight of the player.
    WorldObject* FindInSight(Player* player, SpawnPoint const& spot)
    {
        if (spot.gameObject)
            return player->FindNearestGameObject(spot.entry, SightDistance);
        Creature* creature = player->FindNearestCreature(spot.entry, SightDistance, true);
        return creature;
    }

    MotionTransport* FindShip(Map* map, uint32 entry)
    {
        if (!map)
            return nullptr;
        for (Transport* transport : map->GetAllTransports())
            if (transport && transport->GetEntry() == entry)
                if (MotionTransport* ship = dynamic_cast<MotionTransport*>(transport))
                    return ship;
        return nullptr;
    }

    float Dist2d(float x1, float y1, float x2, float y2)
    {
        float const dx = x1 - x2;
        float const dy = y1 - y2;
        return std::sqrt(dx * dx + dy * dy);
    }

    // Whether a quest in the character's log asks for the item (open, or complete and not handed in yet). Not a quest
    // the planner leaves alone: nothing works on it (the Naxxramas attunement of round 179).
    bool QuestLogAsksFor(Player* player, uint32 entry)
    {
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = player->GetQuestSlotQuestId(slot);
            QuestStatus const status = questId ? player->GetQuestStatus(questId) : QUEST_STATUS_NONE;
            if ((status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE) || sQuestSupport.IsLeftAlone(player, questId))
                continue;
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
                for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    if (quest->RequiredItemId[i] == entry && quest->RequiredItemCount[i])
                        return true;
        }
        return false;
    }

    bool IsDockedAt(MotionTransport const* ship, TransportStop const& stop)
    {
        return ship && ship->GetMapId() == stop.mapId &&
            Dist2d(ship->GetPositionX(), ship->GetPositionY(), stop.x, stop.y) <= DockedDistance;
    }

    // A spot on the ship's deck: next to a crew member (static passengers stand on the deck), else above its origin.
    void DeckSpot(MotionTransport const* ship, float& x, float& y, float& z)
    {
        for (WorldObject* passenger : ship->GetStaticPassengers())
        {
            if (passenger && passenger->IsInWorld() && passenger->GetTypeId() == TYPEID_UNIT)
            {
                x = passenger->GetPositionX() + 1.0f;
                y = passenger->GetPositionY();
                z = passenger->GetPositionZ() + 0.5f;
                return;
            }
        }
        x = ship->GetPositionX();
        y = ship->GetPositionY();
        z = ship->GetPositionZ() + 6.0f;
    }

    // A spot on the pier next to the docked ship: the first point towards the nearest friendly NPC of the harbour that
    // has solid ground under it, at most that NPC's spot.
    bool PierSpot(Player* player, MotionTransport const* ship, BotRuntime const& rt, float& x, float& y, float& z)
    {
        Map* map = ship->GetMap();
        ServicePoint const* nearest = nullptr;
        float best = 250.0f;
        for (ServiceType type : { ServiceType::Vendor, ServiceType::Innkeeper, ServiceType::FlightMaster, ServiceType::Repair })
            for (ServicePoint const& service : sWorldIndex.GetServices(type))
            {
                if (service.spawn.mapId != ship->GetMapId() || rt.IsAvoided(service.spawn))
                    continue;
                float const distance = Dist2d(service.spawn.x, service.spawn.y, ship->GetPositionX(), ship->GetPositionY());
                if (distance < best && WorldIndex::IsFriendlyFaction(service.factionTemplate, player))
                {
                    best = distance;
                    nearest = &service;
                }
            }
        if (!nearest)
            return false;

        for (float step = 15.0f; step < best; step += 5.0f)
        {
            float const f = step / best;
            float const px = ship->GetPositionX() + (nearest->spawn.x - ship->GetPositionX()) * f;
            float const py = ship->GetPositionY() + (nearest->spawn.y - ship->GetPositionY()) * f;
            float const ground = map->GetHeight(px, py, ship->GetPositionZ() + 20.0f);
            float const water = map->GetWaterLevel(px, py);
            if (ground > INVALID_HEIGHT && ground >= water)
            {
                x = px;
                y = py;
                z = ground + 0.5f;
                return true;
            }
        }
        x = nearest->spawn.x;
        y = nearest->spawn.y;
        z = nearest->spawn.z;
        return true;
    }

    // With the worldserver's AllFlightPaths every character may fly to every node of its team (the core marks it as
    // "taxi cheater" at login); else only to the nodes it has learned.
    bool KnowsNode(Player const* player, uint32 node)
    {
        return node && (player->isTaxiCheater() || player->m_taxi.IsTaximaskNodeKnown(node)) &&
            sObjectMgr->GetTaxiMountDisplayId(node, player->GetTeamId()) != 0;
    }

    // A flight route over nodes the bot knows (breadth-first over the taxi paths); empty if there is none.
    std::vector<uint32> FlightRoute(Player const* player, uint32 from, uint32 to, uint32& cost)
    {
        cost = 0;
        std::unordered_map<uint32, uint32> previous;
        std::deque<uint32> queue;
        previous[from] = 0;
        queue.push_back(from);
        while (!queue.empty() && !previous.count(to))
        {
            uint32 const node = queue.front();
            queue.pop_front();
            auto paths = sTaxiPathSetBySource.find(node);
            if (paths == sTaxiPathSetBySource.end())
                continue;
            for (auto const& [next, path] : paths->second)
            {
                if (previous.count(next) || !KnowsNode(player, next))
                    continue;
                previous[next] = node;
                queue.push_back(next);
            }
        }
        if (!previous.count(to))
            return {};

        std::vector<uint32> route;
        for (uint32 node = to; node; node = previous[node])
            route.insert(route.begin(), node);
        for (std::size_t i = 1; i < route.size(); ++i)
        {
            uint32 path = 0, price = 0;
            sObjectMgr->GetTaxiPath(route[i - 1], route[i], path, price);
            cost += price;
        }
        return route;
    }

    Trainer::Trainer const* TrainerOf(ServicePoint const* service)
    {
        return service ? sObjectMgr->GetTrainer(service->spawn.entry) : nullptr;
    }

    // Trainer spells the player can learn now and pay for.
    std::vector<Trainer::Spell const*> AffordableSpells(Trainer::Trainer const* trainer, Player const* player)
    {
        std::vector<Trainer::Spell const*> result;
        if (!trainer || !trainer->IsTrainerValidForPlayer(player))
            return result;

        uint64 budget = player->GetMoney();
        for (Trainer::Spell const& spell : trainer->GetSpells())
        {
            if (!trainer->CanTeachSpell(player, &spell))
                continue;
            if (spell.MoneyCost > budget)
                continue;
            budget -= spell.MoneyCost;
            result.push_back(&spell);
        }
        return result;
    }

    // Learns what the class trainer would teach the player now, without the trip, without an NPC and for free
    // (the owner, 2026-11-12: bots do not travel to trainers for spells, [quote left out]); as
    // Trainer::TeachSpell does, learn spells are cast and the rest is learned directly. A rank that needs one learned
    // in the same call comes in the next pass. Returns the number of spells learned.
    // In-place learning (GuildAdventure.AutoLearnClassSpells) needs a trainer that teaches the class, not one in reach.
    // The trainer whose list teaches the bot the most now (a start zone's trainer lacks the higher ranks: a bot whose
    // nearest trainer of its class stood in its start zone learned nothing more from some level on), then the one with
    // the longest list, then the nearest on the bot's map (a bot questing far from every trainer of its class, in
    // Westfall say, learned nothing with the reach limits of a trainer trip).
    ServicePoint const* ClassTrainerForLearning(Player const* player)
    {
        ServicePoint const* best = nullptr;
        std::size_t bestTeaches = 0, bestListed = 0;
        bool bestHere = false;
        float bestDistance = 0.0f;
        for (ServicePoint const& service : sWorldIndex.GetServices(ServiceType::ClassTrainer))
        {
            if (service.trainerClass != player->getClass() || !WorldIndex::IsFriendlyFaction(service.factionTemplate, player))
                continue;
            Trainer::Trainer const* trainer = TrainerOf(&service);
            if (!trainer || !trainer->IsTrainerValidForPlayer(player))
                continue;
            std::size_t teaches = 0;
            for (Trainer::Spell const& spell : trainer->GetSpells())
                if (trainer->CanTeachSpell(player, &spell))
                    ++teaches;
            std::size_t const listed = trainer->GetSpells().size();
            bool const here = service.spawn.mapId == player->GetMapId();
            float const distance = here ? Dist2d(player, service.spawn) : 0.0f;
            bool better = !best;
            if (!better && teaches != bestTeaches)
                better = teaches > bestTeaches;
            else if (!better && listed != bestListed)
                better = listed > bestListed;
            else if (!better && here != bestHere)
                better = here;
            else if (!better)
                better = here && distance < bestDistance;
            if (better)
            {
                best = &service;
                bestTeaches = teaches;
                bestListed = listed;
                bestHere = here;
                bestDistance = distance;
            }
        }
        return best;
    }

    uint32 LearnTrainerSpellsInPlace(Trainer::Trainer const* trainer, Player* player)
    {
        uint32 learned = 0;
        if (!trainer || !trainer->IsTrainerValidForPlayer(player))
            return learned;
        for (uint32 pass = 0; pass < 3; ++pass)
        {
            std::vector<Trainer::Spell const*> spells;
            for (Trainer::Spell const& spell : trainer->GetSpells())
                if (trainer->CanTeachSpell(player, &spell))
                    spells.push_back(&spell);
            if (spells.empty())
                break;
            for (Trainer::Spell const* spell : spells)
            {
                if (!spell->IsCastable())
                    player->learnSpell(spell->SpellId, false);
                else if (sScriptMgr->OnPlayerCanLearnSpell(player, spell->SpellId))
                    player->CastSpell(player, spell->SpellId, true);
                ++learned;
            }
        }
        return learned;
    }

    // A task the quest planner could start, with what its ranking needs.
    struct QuestCandidate
    {
        int32 bucket = 0;      // -1 nearby pickup, 0 work (turn-in, objective), 2 far pickup
        float distance = 0.0f;
        float progress = 0.0f;
        uint32 questId = 0;
        BotTask task;
    };

    // The work the quest log holds in a zone: a turn-in for a completed quest, else one task per open objective the
    // planner has a spot for. A quest it cannot act on (no objective spawn or POI in the zone, exploration and talk
    // quests NewRpg picked up in passing, blocked, set aside, out-levelled) yields nothing: it gets no task, and it does
    // not count as work that keeps the bot in the zone (CountEligibleQuests).
    // `waiting` (optional): the quests with an objective whose every spot the risk model refuses, with the lowest level
    // from which one of them would do (0: none in reach), and whether a second guild character beside the bot would make
    // one of them workable (QuestPartners: only then is a partner worth waiting for).
    struct WaitingObjective
    {
        uint32 questId = 0;
        uint8 retry = 0;
        bool partnerHelps = false;
        std::string why;           // "" = every spot of it near is too much for the bot
    };
    void CollectLogTasks(GuildBot const& bot, Player* player, BotRuntime const& rt, uint32 zoneId,
        std::vector<QuestCandidate>& candidates, std::vector<WaitingObjective>* waiting = nullptr)
    {
        // An objective that Risk::SaferSpot has no spot for: is it the spots themselves (then the quest can wait for a
        // level), or something passing (a place set aside for minutes, the way there)?
        auto noteWaiting = [&](uint32 questId, std::vector<SpawnPoint> const& spawns)
        {
            if (!waiting)
                return;
            std::vector<std::pair<float, SpawnPoint const*>> nearest;
            for (SpawnPoint const& spawn : spawns)
                if (spawn.mapId == player->GetMapId())
                    nearest.emplace_back(Dist2d(player, spawn), &spawn);
            constexpr std::size_t Looked = 12;
            std::size_t const looked = std::min(nearest.size(), Looked);
            std::partial_sort(nearest.begin(), nearest.begin() + looked, nearest.end(),
                [](auto const& a, auto const& b) { return a.first < b.first; });
            uint8 retry = 0;
            for (std::size_t i = 0; i < looked; ++i)
            {
                Risk::SpotVerdict const verdict = Risk::JudgeSpot(player, rt, *nearest[i].second);
                if (verdict.usable)
                    return;
                if (verdict.retryLevel && (!retry || verdict.retryLevel < retry))
                    retry = verdict.retryLevel;
            }
            if (!looked)
                return;
            // A partner raises what the bot may take on (the fight's cost, the number of foes), not the level cap or
            // the creatures far above the bot: 12 of 14 pairs of the first live round parted seconds after they met,
            // their targets above the level cap for both.
            // Nor the way there: the walk gate judges it for the bot alone (round 74: a pair met and parted 23 s later,
            // the leader's way to the Kurzen camp passing level-35 creatures), so a spot behind such a way does not count.
            constexpr std::size_t LookedWithPartner = 4;
            int32 const margin = sBotSettings.Get(player, Setting::WalksMaxLevelsAbove);
            bool partnerHelps = false;
            for (std::size_t i = 0; i < std::min(looked, LookedWithPartner) && !partnerHelps; ++i)
            {
                SpawnPoint const& spot = *nearest[i].second;
                if (!Risk::JudgeSpot(player, rt, spot, 1).usable)
                    continue;
                uint8 const way = sRoadMap.DangerAlong(spot.mapId, player->GetPositionX(), player->GetPositionY(), spot.x, spot.y,
                    player->GetTeamId(), RoadMap::EndStretch, player->GetPositionZ(), spot.z);
                partnerHelps = int32(way) - int32(player->GetLevel()) <= margin;
            }
            waiting->push_back({ questId, retry, partnerHelps });
        };
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = player->GetQuestSlotQuestId(slot);
            Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
            if (!quest)
                continue;

            QuestStatus const status = player->GetQuestStatus(questId);
            if (status == QUEST_STATUS_COMPLETE)
            {
                // A completed quest whose turn-in failed twice stays in the log (the owner can turn it in when playing the bot).
                auto turnInBlock = bot.questBlocks.find(questId);
                if (turnInBlock != bot.questBlocks.end() && turnInBlock->second.blocked)
                    continue;
                // The ender in the target zone; outside it only when it is close (quests that end in the next village).
                std::optional<SpawnPoint> ender = NearestSpawn(sWorldIndex.GetQuestEnders(questId), player, zoneId, true, MaxQuestNpcDistance, rt);
                if (!ender)
                    ender = NearestSpawn(sWorldIndex.GetQuestEnders(questId), player, 0, true, NearbyTurnInDistance, rt);
                if (!ender)
                    continue;

                QuestCandidate candidate;
                candidate.distance = Dist2d(player, *ender);
                candidate.progress = 1.0f;
                candidate.questId = questId;
                candidate.task.kind = TaskKind::TurnIn;
                candidate.task.questId = questId;
                candidate.task.hasSpot = true;
                candidate.task.spot = *ender;
                candidate.task.targetName = SpawnName(*ender);
                candidates.push_back(std::move(candidate));
                continue;
            }

            if (status != QUEST_STATUS_INCOMPLETE)
                continue;

            // Quests the bot out-levelled (grey) are not worked any more; NewRpg drops them when the log fills up.
            if (!NewRpgHelpers::IsQuestWorthDoing(player, quest))
                continue;

            auto block = bot.questBlocks.find(questId);
            if (block != bot.questBlocks.end() && block->second.blocked)
                continue;
            // A hard quest no guild partner stands beside the bot for (QuestPartners.h): kept in the log, skipped for now.
            if (sQuestPartners.IsSkipped(bot, player, questId))
                continue;
            if (rt.IsDangerous(questId, player->GetLevel()))
                continue;
            // A quest a guild character two or more levels above the bot died on in the last hours waits until the
            // bot has that level, unless a partner stands beside it: what killed a level-22 character is no work for a
            // level-19 one alone. With two deaths on it, it waits for every bot alone that is not two levels above
            // the highest of the dead.
            if (AdventureGuild const* ownGuild = sGuildAdventureMgr.FindGuildOfMember(player->GetGUID()))
            {
                uint8 diedAt = 0;
                uint32 const died = QuestDeaths(ownGuild->ownerAccount, questId, diedAt);
                bool const above = died && diedAt >= player->GetLevel() + QuestDeathLevelsAbove;
                bool const many = died >= QuestDeathsMany && player->GetLevel() < uint32(diedAt) + QuestDeathLevelsAbove;
                if ((above || many) && sSquadService.CountTogether(player, PartnerBesideYards) < 2)
                {
                    time_t& noted = sQuestDeathNotes[(uint64(player->GetGUID().GetCounter()) << 32) | questId];
                    time_t const time = GameTime::GetGameTime().count();
                    if (time - noted >= 30 * MINUTE)
                    {
                        noted = time;
                        LOG_INFO(ActivityLog, "activity: {} leaves quest {} \"{}\" alone for now: {} died on it within the last {} h, {}at "
                            "level {} (it is level {})", bot.name, questId, QuestTitle(questId),
                            died == 1 ? std::string("a guild character") : Acore::StringFormat("{} guild characters", died),
                            GuildDeathKeepSeconds / HOUR, died == 1 ? "" : "the highest ", uint32(diedAt), uint32(player->GetLevel()));
                    }
                    continue;
                }
            }
            // A quest the planner leaves alone (QuestSupport) that is in the log anyway (taken before it was listed or
            // learned, or by the owner): no work on it.
            if (sQuestSupport.IsLeftAlone(player, questId))
                continue;

            // An escort: stay with its NPC, before anything else (it walks off the moment the quest is taken).
            if (uint32 const escortEntry = sQuestSupport.GetEscortNpc(questId))
            {
                Creature* escort = player->FindNearestCreature(escortEntry, EscortSight, true);
                std::optional<SpawnPoint> spawn = NearestSpawn(sWorldIndex.GetCreatureSpawns(escortEntry), player, 0, false,
                    MaxQuestNpcDistance, rt);
                if (!escort && !spawn)
                    continue;
                QuestCandidate candidate;
                candidate.bucket = -2;
                candidate.distance = escort ? player->GetDistance2d(escort) : Dist2d(player, *spawn);
                candidate.questId = questId;
                candidate.task.kind = TaskKind::Escort;
                candidate.task.questId = questId;
                candidate.task.escortEntry = escortEntry;
                candidate.task.hasSpot = spawn.has_value();
                if (spawn)
                    candidate.task.spot = *spawn;
                else
                {
                    candidate.task.spot.mapId = uint16(player->GetMapId());
                    candidate.task.spot.x = escort->GetPositionX();
                    candidate.task.spot.y = escort->GetPositionY();
                    candidate.task.spot.z = escort->GetPositionZ();
                }
                candidate.task.targetName = escort ? escort->GetName() : SpawnName(*spawn);
                candidates.push_back(std::move(candidate));
                continue;
            }

            QuestStatusMap& statusMap = player->getQuestStatusMap();
            auto statusItr = statusMap.find(questId);
            if (statusItr == statusMap.end())
                continue;
            QuestStatusData const& progress = statusItr->second;

            // The quest's POI for an objective, if it lies in the zone (its centre).
            auto poiSpot = [&](int32 index) -> std::optional<SpawnPoint>
            {
                QuestPOIVector const* pois = sObjectMgr->GetQuestPOIVector(questId);
                if (!pois)
                    return std::nullopt;
                for (QuestPOI const& poi : *pois)
                {
                    if (poi.ObjectiveIndex != index || poi.MapId != player->GetMapId() || poi.points.empty())
                        continue;
                    float x = 0.0f, y = 0.0f;
                    for (QuestPOIPoint const& point : poi.points)
                    {
                        x += point.x;
                        y += point.y;
                    }
                    x /= float(poi.points.size());
                    y /= float(poi.points.size());
                    float const z = player->GetMap()->GetHeight(x, y, MAX_HEIGHT);
                    if (player->GetMap()->GetZoneId(PHASEMASK_NORMAL, x, y, z) != zoneId)
                        continue;
                    // A POI where nothing could be done a moment ago (an interaction found no target there) waits.
                    if (rt.IsInAvoidedArea(poi.MapId, x, y, player->GetLevel()))
                        continue;
                    SpawnPoint spot;
                    spot.mapId = uint16(player->GetMapId());
                    spot.x = x;
                    spot.y = y;
                    spot.z = z;
                    return spot;
                }
                return std::nullopt;
            };
            // Credit that comes from a script event (QuestEvents): a talk, an event at the quest giver, an NPC to lead.
            // A talk can also stand for a creature objective; that objective is then not one to kill.
            bool talkObjective[QUEST_OBJECTIVES_COUNT] = {};
            for (QuestEventPlan const& plan : sQuestEvents.GetPlans(questId))
            {
                if (plan.objective >= 0)
                {
                    talkObjective[plan.objective] = true;
                    if (progress.CreatureOrGOCount[plan.objective] >= quest->RequiredNpcOrGoCount[plan.objective])
                        continue;
                }
                else if (progress.Explored)
                    continue;

                std::optional<SpawnPoint> spot;
                if (plan.kind == QuestEventKind::Enter)
                {
                    // The area trigger whose script starts the event.
                    AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(plan.entry);
                    if (trigger && trigger->map == player->GetMapId() && Dist2d(player, trigger->x, trigger->y) <= MaxQuestNpcDistance)
                    {
                        SpawnPoint place;
                        place.spawnId = 0x40000000u | plan.entry; // a key of its own for the "skip for a while" list
                        place.entry = plan.entry;
                        place.mapId = uint16(trigger->map);
                        place.x = trigger->x;
                        place.y = trigger->y;
                        place.z = trigger->z;
                        if (!rt.IsAvoided(place))
                            spot = place;
                    }
                }
                else
                    spot = NearestSpawn(plan.gameObject ? sWorldIndex.GetGameObjectSpawns(plan.entry) :
                        sWorldIndex.GetCreatureSpawns(plan.entry), player, 0, false, MaxQuestNpcDistance, rt);
                if (!spot)
                    continue;
                // A place to clear is a camp and then its master: not for a bot below that creature's level (a level-16
                // druid died twice clearing the murlocs around Murkdeep, level 19).
                if (plan.kind == QuestEventKind::Clear)
                    if (CreatureTemplate const* wanted = sObjectMgr->GetCreatureTemplate(plan.entry))
                        if (player->GetLevel() < wanted->maxlevel)
                            continue;
                QuestCandidate candidate;
                candidate.distance = Dist2d(player, *spot);
                candidate.questId = questId;
                candidate.task.kind = TaskKind::Event;
                candidate.task.questId = questId;
                candidate.task.event = plan;
                candidate.task.hasSpot = true;
                candidate.task.spot = *spot;
                candidate.task.targetName = plan.kind == QuestEventKind::Enter ? "the place of its event" : SpawnName(*spot);
                if (plan.kind == QuestEventKind::Stay || plan.kind == QuestEventKind::Lead)
                {
                    // Running (the quest was taken a moment ago): before anything else.
                    candidate.task.eventFresh = rt.IsEventFresh(questId);
                    if (candidate.task.eventFresh)
                        candidate.bucket = -2;
                }
                if (plan.kind == QuestEventKind::Lead)
                {
                    std::optional<SpawnPoint> end = NearestSpawn(sWorldIndex.GetCreatureSpawns(plan.endEntry), player, 0, false,
                        MaxQuestNpcDistance, rt);
                    if (!end)
                        continue;
                    candidate.task.eventEnd = *end;
                }
                candidates.push_back(std::move(candidate));
            }

            auto addObjective = [&](int32 index, std::vector<SpawnPoint> const& spawns, uint32 have, uint32 need,
                uint32 fetchItem = 0) -> bool
            {
                QuestCandidate candidate;
                candidate.progress = need ? float(have) / float(need) : 0.0f;
                candidate.questId = questId;
                candidate.task.kind = TaskKind::Objective;
                candidate.task.questId = questId;
                candidate.task.objective = index;
                candidate.task.fetchItem = fetchItem;

                // Playing it safe after several deaths: no objectives for now (turn-ins and pickups go on; an item a vendor
                // sells may still be bought, so false).
                if (Risk::IsCautious(rt, player->GetLevel()))
                    return false;

                // (Among the spawns that are up: what the bot has just killed is no place to walk to.)
                std::vector<SpawnPoint> upBuffer;
                std::vector<SpawnPoint> const& up = WithoutDown(spawns, player, upBuffer);
                if (std::optional<SpawnPoint> spot = NearestSpawn(spawns, player, zoneId, false, MaxQuestNpcDistance, rt))
                {
                    // Not the nearest spawn but the safest in reach; none when all are too strong or in avoided areas. Then
                    // the objective is left for now, also its POI and the spawns across the border (true: handled).
                    spot = Risk::SaferSpot(up, player, rt, *spot, zoneId);
                    if (!spot)
                    {
                        noteWaiting(questId, spawns);
                        return true;
                    }
                    // Not a spawn whose straight way crosses a place the bot has just set aside (a pack it would not
                    // pull, a place it left a fight at): the walk would end at that place again. The objective waits
                    // while the place is set aside; other work comes first. (A longer walk takes a route of the road
                    // map, which goes around such places itself.)
                    if (Dist2d(player, *spot) <= sBotSettings.GetFloat(player, Setting::RoadsMinDistance) &&
                        rt.CrossesAvoidedArea(spot->mapId, player->GetPositionX(), player->GetPositionY(), spot->x, spot->y,
                            player->GetLevel()))
                        return true;
                    candidate.distance = Dist2d(player, *spot) + Risk::RankPenalty(player, quest, *spot);
                    candidate.task.hasSpot = true;
                    candidate.task.spot = *spot;
                    candidate.task.targetName = SpawnName(*spot);
                    candidates.push_back(std::move(candidate));
                    return true;
                }

                // A dungeon's quest (its zone is a dungeon's area: "Trouble at the Docks", Mad Magglish in the Wailing
                // Caverns): what it asks for lives in the dungeon, and a walk to its quest area outside is five minutes
                // until NewRpg gives the quest up, twice and it is blocked (Hordeboss, the test guild). It waits for a
                // dungeon run, which does it on the way.
                if (IsDungeonQuest(quest) && !player->GetMap()->Instanceable())
                    return true;
                // The target has spawns in the zone, and none of them is a place to go to now (down until its respawn,
                // or set aside: "nothing near: next spawn"): the objective waits for them, other work comes first. Its
                // quest area is no place to stand at: Stonehorn (40) stood 263 s at the quest area of "Tiger Mastery"
                // while Sin'Dall, the one creature its Paw drops from, was dead (respawn 300 s), until the rule for a
                // quest area that gets nowhere ended the task (the test guild, 2026-11-26).
                {
                    std::size_t looked = 0;
                    for (SpawnPoint const& spawn : spawns)
                    {
                        if (spawn.mapId != player->GetMapId() || Dist2d(player, spawn) > MaxQuestNpcDistance)
                            continue;
                        if (++looked > 16)
                            break;
                        if (sWorldIndex.GetSpawnZone(spawn) == zoneId)
                            return true;
                    }
                }
                // No spawn of the target in the zone: the quest's POI for this objective, if it lies in the zone; NewRpg
                // then walks to the POI itself.
                if (std::optional<SpawnPoint> poi = poiSpot(index))
                {
                    // A quest area the bot could not get to (or kept dying at) is left alone for the time. The area is
                    // wide and its middle is only a point: a place set aside within 70 yd of the middle counts, and
                    // so does one the straight way there leads through (a level-24 rogue was sent to "the quest area"
                    // of a mill four times between its deaths at the camp beside it).
                    bool setAside = rt.IsInAvoidedArea(player->GetMapId(), poi->x, poi->y, player->GetLevel());
                    for (int corner = 0; corner < 8 && !setAside; ++corner)
                        setAside = rt.IsInAvoidedArea(player->GetMapId(), poi->x + 70.0f * std::cos(float(corner) * float(M_PI) / 4.0f),
                            poi->y + 70.0f * std::sin(float(corner) * float(M_PI) / 4.0f), player->GetLevel());
                    setAside = setAside || rt.CrossesAvoidedArea(player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
                        poi->x, poi->y, player->GetLevel());
                    if (!setAside)
                    {
                        candidate.distance = Dist2d(player, poi->x, poi->y);
                        candidate.task.hasSpot = false;
                        candidate.task.questArea = true;
                        candidate.task.spot = *poi;
                        candidate.task.targetName = "the quest area";
                        candidates.push_back(std::move(candidate));
                        return true;
                    }
                }

                // Outside the zone, but close by: the targets of a zone's quests often live just across its border. Not
                // when they are well above the bot (the next zone's mobs).
                if (std::optional<SpawnPoint> spot = NearestSpawn(spawns, player, 0, false, NearbyObjectiveDistance, rt))
                {
                    // Judged like a spawn inside the zone (RiskModel): not one in a place that is set aside or with too
                    // much around it. Without this the planner chose such a spawn and the task's first step ended it,
                    // every second (two bots, 950 log lines in a quarter of an hour). None: the objective waits.
                    spot = Risk::SaferSpot(up, player, rt, *spot, 0);
                    if (!spot)
                        return true;
                    CreatureTemplate const* cinfo = spot->gameObject ? nullptr : sObjectMgr->GetCreatureTemplate(spot->entry);
                    if (!cinfo || cinfo->maxlevel <= player->GetLevel() + NearbyObjectiveLevels)
                    {
                        candidate.distance = Dist2d(player, *spot);
                        candidate.task.hasSpot = true;
                        candidate.task.spot = *spot;
                        candidate.task.targetName = SpawnName(*spot);
                        candidates.push_back(std::move(candidate));
                        return true;
                    }
                }
                return false;
            };

            // An objective killing and looting do not do (docs/orders.md "Quest interactions"): at the nearest spawn of
            // what the plan acts on or stands next to, else at the objective's POI.
            auto addInteraction = [&](int32 index, InteractionPlan const& plan, uint32 have, uint32 need)
            {
                QuestCandidate candidate;
                candidate.progress = need ? float(have) / float(need) : 0.0f;
                candidate.questId = questId;
                candidate.task.kind = TaskKind::Interact;
                candidate.task.questId = questId;
                candidate.task.objective = index;
                candidate.task.plan = plan;
                candidate.task.targetName = plan.describe;

                // Nothing to go to (combine what it carries): right where it stands.
                if (plan.anywhere)
                {
                    candidate.distance = 0.0f;
                    candidate.task.hasSpot = true;
                    candidate.task.spot.mapId = uint16(player->GetMapId());
                    candidate.task.spot.x = player->GetPositionX();
                    candidate.task.spot.y = player->GetPositionY();
                    candidate.task.spot.z = player->GetPositionZ();
                    candidates.push_back(std::move(candidate));
                    return;
                }
                std::vector<SpawnPoint> spawns;
                for (uint32 entry : QuestInteraction::SpotCreatures(plan))
                    for (SpawnPoint const& spawn : sWorldIndex.GetCreatureSpawns(entry))
                        spawns.push_back(spawn);
                for (uint32 entry : QuestInteraction::SpotGameObjects(plan))
                    for (SpawnPoint const& spawn : sWorldIndex.GetGameObjectSpawns(entry))
                        spawns.push_back(spawn);
                // The item works only near a creature (the Ooze Buster near a Zeppelin Power Core): only targets near one
                // (OrderInteraction takes no other there; a spot among oozes far from the cores ended "no target in sight").
                if (plan.nearCreature && !sWorldIndex.GetCreatureSpawns(plan.nearCreature).empty())
                {
                    std::vector<SpawnPoint> const& nearOnes = sWorldIndex.GetCreatureSpawns(plan.nearCreature);
                    float const reach = NearStandYards + plan.range - 4.0f;
                    spawns.erase(std::remove_if(spawns.begin(), spawns.end(), [&](SpawnPoint const& spawn)
                    {
                        return std::none_of(nearOnes.begin(), nearOnes.end(), [&](SpawnPoint const& one)
                        {
                            return one.mapId == spawn.mapId && std::hypot(one.x - spawn.x, one.y - spawn.y) < reach;
                        });
                    }), spawns.end());
                }
                // A bot that plays it safe after several deaths works no objectives for a while (RiskModel).
                if (Risk::IsCautious(rt, player->GetLevel()))
                    return;
                // What using the item summons is a fight there too (Risk::JudgeSummon): the Untapped Dowsing Widget of
                // "Gadgetzan Water Survey" calls a level-47 Centipaar Tunneler out of the water hole, four levels above
                // the level-43 paladin that went to use it. Too much: the objective waits until the level it names.
                if (plan.kind == InteractionKind::UseItem && plan.spellId)
                    if (Risk::SummonVerdict const summon = Risk::JudgeSummon(player, rt, plan.spellId); !summon.usable)
                    {
                        ItemTemplate const* item = sObjectMgr->GetItemTemplate(plan.itemId);
                        if (waiting)
                            waiting->push_back({ questId, summon.retryLevel, false, Acore::StringFormat("using {} summons {}, too much "
                                "for it at level {}", item ? item->Name1 : std::string("its item"), summon.what, uint32(player->GetLevel())) });
                        return;
                    }
                // An object whose use springs a trap that summons is judged the same way, spawn by spawn: of the Deepmoss
                // Eggs of "Deepmoss Spider Eggs" one kind springs two hatchlings that call their matriarch, the other
                // nothing. Only the objects whose trap the bot can take; none: the objective waits.
                if (plan.kind == InteractionKind::UseObject)
                {
                    Risk::SummonVerdict refused;
                    std::unordered_map<uint32, bool> trapOk;
                    spawns.erase(std::remove_if(spawns.begin(), spawns.end(), [&](SpawnPoint const& spawn)
                    {
                        if (!spawn.gameObject)
                            return false;
                        auto known = trapOk.find(spawn.entry);
                        if (known == trapOk.end())
                        {
                            bool ok = true;
                            GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(spawn.entry);
                            if (uint32 const trapId = object ? object->GetLinkedGameObjectEntry() : 0)
                                if (GameObjectTemplate const* trap = sObjectMgr->GetGameObjectTemplate(trapId))
                                    if (trap->type == GAMEOBJECT_TYPE_TRAP && trap->trap.spellId)
                                        if (Risk::SummonVerdict const summon = Risk::JudgeSummon(player, rt, trap->trap.spellId);
                                            !summon.usable)
                                        {
                                            ok = false;
                                            refused = summon;
                                        }
                            known = trapOk.emplace(spawn.entry, ok).first;
                        }
                        return !known->second;
                    }), spawns.end());
                    if (spawns.empty() && !refused.what.empty())
                    {
                        if (waiting)
                            waiting->push_back({ questId, refused.retryLevel, false, Acore::StringFormat("its objects spring a "
                                "trap that summons {}, too much for it at level {}", refused.what, uint32(player->GetLevel())) });
                        return;
                    }
                }
                std::optional<SpawnPoint> spot = NearestSpawn(spawns, player, zoneId, false, MaxQuestNpcDistance, rt);
                // Not the nearest spawn but the safest in reach, judged like a kill objective's (the creatures around it,
                // places set aside after deaths, the straight way there); none: the objective waits.
                if (spot)
                {
                    std::vector<SpawnPoint> upBuffer;
                    spot = Risk::SaferSpot(WithoutDown(spawns, player, upBuffer), player, rt, *spot, zoneId);
                    if (!spot)
                    {
                        noteWaiting(questId, spawns);
                        return;
                    }
                }
                // The POI only when there is no spawn at all: with the spawns set aside for a while (a chest waiting to
                // respawn), the POI next to them would send the bot back at once.
                else if (spawns.empty())
                {
                    spot = poiSpot(index);
                    if (spot && !Risk::IsSafePlace(player, rt, spot->mapId, spot->x, spot->y, spot->z))
                        return;
                }
                if (!spot)
                    return;
                candidate.distance = Dist2d(player, spot->x, spot->y) + Risk::RankPenalty(player, quest, *spot);
                candidate.task.hasSpot = true;
                candidate.task.spot = *spot;
                candidates.push_back(std::move(candidate));
            };

            // The interaction an objective needs, if the bot has the means for it (the quest item, a helpful spell).
            auto usablePlan = [&](int32 index) -> InteractionPlan const*
            {
                InteractionPlan const* plan = QuestInteraction::Resolve(player, quest, index);
                return plan ? QuestInteraction::Usable(player, *plan) : nullptr;
            };

            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                int32 const target = quest->RequiredNpcOrGo[i];
                uint32 const need = quest->RequiredNpcOrGoCount[i];
                if (!target || !need || progress.CreatureOrGOCount[i] >= need || talkObjective[i])
                    continue;
                if (InteractionPlan const* plan = usablePlan(i))
                {
                    addInteraction(i, *plan, progress.CreatureOrGOCount[i], need);
                    continue;
                }
                // The interaction's item is not in the bags but creatures drop it (the Grimtotem Key of "Prisoners of the
                // Grimtotems", one per cage): those creatures first, then the interaction. Retadin stood at a cage
                // without a key (the owner, 2026-11-14), the prisoner itself being the objective's creature.
                if (InteractionPlan const* plan = QuestInteraction::Resolve(player, quest, i);
                    plan && plan->kind == InteractionKind::UseItem && plan->itemId && !player->HasItemCount(plan->itemId, 1) &&
                    !sWorldIndex.GetItemSources(plan->itemId).empty() &&
                    addObjective(i, sWorldIndex.GetItemSources(plan->itemId), progress.CreatureOrGOCount[i], need, plan->itemId))
                    continue;
                // A credit that the kill of other creatures gives (KillCredit, a credit spell on their death): their
                // spawns too. The Captured Totem and the Restless Apparition stand nowhere; Retadin walked "the quest
                // area" of "The Grimtotem Weapon" for eleven minutes without a crocolisk to kill.
                std::vector<SpawnPoint> withSources;
                std::vector<SpawnPoint> const* spawns = target > 0 ? &sWorldIndex.GetCreatureSpawns(uint32(target)) :
                    &sWorldIndex.GetGameObjectSpawns(uint32(-target));
                if (target > 0)
                    if (std::vector<uint32> const& sources = QuestInteraction::CreditSources(uint32(target)); !sources.empty())
                    {
                        withSources = *spawns;
                        for (uint32 source : sources)
                            if (QuestInteraction::CanKill(player, source))
                            {
                                std::vector<SpawnPoint> const& more = sWorldIndex.GetCreatureSpawns(source);
                                withSources.insert(withSources.end(), more.begin(), more.end());
                            }
                        spawns = &withSources;
                    }
                addObjective(i, *spawns, progress.CreatureOrGOCount[i], need);
            }
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            {
                uint32 const item = quest->RequiredItemId[i];
                uint32 const need = quest->RequiredItemCount[i];
                if (!item || !need || progress.ItemCount[i] >= need)
                    continue;
                if (InteractionPlan const* plan = usablePlan(QUEST_OBJECTIVES_COUNT + i))
                {
                    addInteraction(QUEST_OBJECTIVES_COUNT + i, *plan, progress.ItemCount[i], need);
                    continue;
                }
                if (addObjective(QUEST_OBJECTIVES_COUNT + i, sWorldIndex.GetItemSources(item), progress.ItemCount[i], need))
                    continue;
                // A fish (fishing_loot_template): to the water of its zone, when the bot has the spell and a pole.
                if (Fishing::CanFish(player))
                    if (std::optional<SpawnPoint> water = Fishing::PlaceFor(player, rt, item))
                    {
                        QuestCandidate candidate;
                        candidate.distance = Dist2d(player, *water);
                        candidate.progress = float(progress.ItemCount[i]) / float(need);
                        candidate.questId = questId;
                        candidate.task.kind = TaskKind::Fish;
                        candidate.task.questId = questId;
                        candidate.task.objective = int32(QUEST_OBJECTIVES_COUNT + i);
                        candidate.task.fishItem = item;
                        candidate.task.hasSpot = true;
                        candidate.task.spot = *water;
                        candidate.task.targetName = "the water";
                        candidates.push_back(std::move(candidate));
                        continue;
                    }

                // Nothing to loot it from in reach: an item vendors sell is bought, from the guild's budget (GuildBudget:
                // the bot's own money first, the rest from the other guild characters, within the guild's limit).
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
                uint32 const missing = need - progress.ItemCount[i];
                // An item another quest rewards (the chapters of "The Green Hills of Stranglethorn", an antidote): that
                // quest comes first. It is picked up wherever its giver stands, also outside the target zone.
                bool viaQuest = false;
                for (uint32 givingId : sWorldIndex.GetQuestsRewarding(item))
                {
                    Quest const* giving = sObjectMgr->GetQuestTemplate(givingId);
                    auto givingBlock = bot.questBlocks.find(givingId);
                    if (!giving || (givingBlock != bot.questBlocks.end() && givingBlock->second.blocked) ||
                        player->GetQuestStatus(givingId) != QUEST_STATUS_NONE || player->GetQuestRewardStatus(givingId) ||
                        !NewRpgHelpers::IsQuestWorthDoing(player, giving) || !NewRpgHelpers::IsQuestCapableDoing(player, giving) ||
                        !player->CanTakeQuest(giving, false) || !player->CanAddQuest(giving, false))
                        continue;
                    std::optional<SpawnPoint> giver = NearestSpawn(sWorldIndex.GetQuestStarters(givingId), player, 0, true,
                        MaxQuestNpcDistance, rt);
                    if (!giver)
                        continue;
                    QuestCandidate candidate;
                    candidate.distance = Dist2d(player, *giver);
                    candidate.questId = givingId;
                    candidate.task.kind = TaskKind::Pickup;
                    candidate.task.questId = givingId;
                    candidate.task.hasSpot = true;
                    candidate.task.spot = *giver;
                    candidate.task.targetName = SpawnName(*giver);
                    candidates.push_back(std::move(candidate));
                    viaQuest = true;
                    break;
                }
                if (viaQuest)
                    continue;

                if (!proto || !proto->BuyPrice)
                    continue;
                uint32 const bundle = std::max<uint32>(1, proto->BuyCount);
                if (uint64(proto->BuyPrice) * ((missing + bundle - 1) / bundle) > GuildBudget::Available(player))
                    continue;
                if (std::optional<SpawnPoint> vendor = NearestSpawn(sWorldIndex.GetItemVendors(item), player, 0, true, MaxServiceDistance, rt))
                {
                    QuestCandidate candidate;
                    candidate.distance = Dist2d(player, *vendor);
                    candidate.progress = float(progress.ItemCount[i]) / float(need);
                    candidate.questId = questId;
                    candidate.task.kind = TaskKind::Vendor;
                    candidate.task.questId = questId;
                    candidate.task.hasSpot = true;
                    candidate.task.spot = *vendor;
                    candidate.task.targetName = SpawnName(*vendor);
                    candidate.task.buyItem = item;
                    candidate.task.buyCount = missing;
                    candidates.push_back(std::move(candidate));
                }
            }

            // Exploration: walk into the quest's area trigger (in the zone).
            if (!progress.Explored)
                if (InteractionPlan const* plan = QuestInteraction::ResolveExplore(player, quest))
                    if (AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(plan->areaTrigger))
                        if (trigger->map == player->GetMapId() &&
                            player->GetMap()->GetZoneId(PHASEMASK_NORMAL, trigger->x, trigger->y, trigger->z) == zoneId &&
                            !Risk::IsCautious(rt, player->GetLevel()) &&
                            Risk::IsSafePlace(player, rt, trigger->map, trigger->x, trigger->y, trigger->z))
                        {
                            QuestCandidate candidate;
                            candidate.questId = questId;
                            candidate.distance = Dist2d(player, trigger->x, trigger->y);
                            candidate.task.kind = TaskKind::Interact;
                            candidate.task.questId = questId;
                            candidate.task.plan = *plan;
                            candidate.task.hasSpot = true;
                            candidate.task.spot.mapId = uint16(trigger->map);
                            candidate.task.spot.x = trigger->x;
                            candidate.task.spot.y = trigger->y;
                            candidate.task.spot.z = trigger->z;
                            // (The ground in the trigger nearest to its middle: a middle in the air, beside a mesa or over a
                            // canyon, has no navmesh, and every walk to it was refused as one through the air.)
                            NavPoint inside;
                            if (trigger->radius > 0.0f && sDungeonNav.NearestOnMesh(trigger->map, trigger->x, trigger->y, trigger->z,
                                    trigger->radius - 1.0f, inside))
                            {
                                candidate.task.spot.x = inside.x;
                                candidate.task.spot.y = inside.y;
                                candidate.task.spot.z = inside.z;
                            }
                            candidate.task.targetName = "the place to explore";
                            candidates.push_back(std::move(candidate));
                        }
        }
    }

    // Items that hold a quest item (WorldIndex::IsQuestContainer: a necklace with the jewel in it, a crate) are opened:
    // their loot goes into the bags like a player's who right-clicks them. Locked ones are left.
    void OpenQuestContainers(GuildBot const& bot, Player* player)
    {
        std::vector<std::pair<uint8, uint8>> found;
        auto look = [&](uint8 bag, uint8 slot)
        {
            Item const* item = player->GetItemByPos(bag, slot);
            if (item && sWorldIndex.IsQuestContainer(item->GetEntry()) && !item->GetTemplate()->LockID)
                found.emplace_back(bag, slot);
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            look(INVENTORY_SLOT_BAG_0, slot);
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag const* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    look(bagSlot, uint8(slot));

        for (auto const& [bag, slot] : found)
        {
            Item* item = player->GetItemByPos(bag, slot);
            if (!item || player->GetFreeInventorySpace() < 1)
                continue;
            uint32 const entry = item->GetEntry();
            std::string const name = item->GetTemplate()->Name1;
            uint32 one = 1;
            player->DestroyItemCount(item, one, true);
            player->AutoStoreLoot(entry, LootTemplates_Item, true);
            LOG_INFO(ActivityLog, "activity: {} opens {} (it holds a quest item)", bot.name, name);
        }
    }

    // Quest log care. NewRpg empties the whole quest log, completed quests included, once fewer than two slots are free
    // (NewRpgBaseAction::OrganizeQuestLog), and it takes quests in passing, so the log fills with quests nobody works
    // on: hand-ins in another zone, quests picked up next door, quests the planner cannot act on. Before that happens
    // the planner abandons what is worth least until MinFreeQuestSlots are free again: quests the bot out-levelled or
    // failed, quests the planner leaves alone or has blocked for this bot, then quests without a task, other zones'
    // first. Completed quests and quests with a task stay.
    void CareForQuestLog(GuildBot const& bot, Player* player, BotRuntime const& rt, uint32 zoneId)
    {
        if (FreeQuestSlots(player) >= MinFreeQuestSlots)
            return;

        std::vector<QuestCandidate> work;
        CollectLogTasks(bot, player, rt, zoneId, work);
        std::unordered_set<uint32> withTask;
        for (QuestCandidate const& candidate : work)
            withTask.insert(candidate.questId);

        // (rank, quest): the lowest rank goes first; a quest the bot has progress on ranks behind every quest without
        // (10 added), so it is given up only when nothing else is left to give up.
        std::vector<std::pair<uint8, uint32>> victims;
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = player->GetQuestSlotQuestId(slot);
            Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
            if (!quest || withTask.count(questId) || questId == rt.task.questId)
                continue;
            QuestStatus const status = player->GetQuestStatus(questId);
            if (status == QUEST_STATUS_COMPLETE)
                continue;

            auto block = bot.questBlocks.find(questId);
            uint8 rank = 3;
            if (status == QUEST_STATUS_FAILED || !NewRpgHelpers::IsQuestWorthDoing(player, quest))
                rank = 0;
            else if (sQuestSupport.GetVerdict(questId) != QuestVerdict::Supported ||
                (block != bot.questBlocks.end() && block->second.blocked))
                rank = 1;
            else if (sWorldIndex.GetQuestZone(questId) != zoneId)
                rank = 2;
            // A hard quest kept for a guild partner (QuestPartners.h): last of those without progress.
            if (rank >= 2 && sQuestPartners.IsSkipped(bot, player, questId))
                rank = 4;
            if (status != QUEST_STATUS_FAILED && QuestMark(player, questId) > 1)
                rank += 10;
            victims.emplace_back(rank, questId);
        }
        std::sort(victims.begin(), victims.end());

        static char const* const Why[] = { "out-levelled or failed", "the planner leaves it alone",
            "another zone's quest without a task", "no task for it", "kept for a guild partner" };
        for (auto const& [rank, questId] : victims)
        {
            if (FreeQuestSlots(player) >= MinFreeQuestSlots)
                break;
            LOG_INFO(ActivityLog, "activity: {} abandons quest {} \"{}\" to keep the quest log open ({}{})", bot.name, questId,
                QuestTitle(questId), Why[rank % 10], rank >= 10 ? "; it had progress, nothing else was left to give up" : "");
            // (Not taken again for a while: its giver is often right there, QuestSupport::NoteDropped. A failed quest
            // gets its one new try, wowbot335 A21.)
            if (player->GetQuestStatus(questId) != QUEST_STATUS_FAILED)
                sQuestSupport.NoteDropped(player->GetGUID(), questId, uint32(GameTime::GetGameTime().count()) + DroppedQuestSeconds);
            BotDriver::AbandonQuest(player, questId);
        }
    }
}

bool GuildAdventure::BotRuntime::IsAvoided(SpawnPoint const& spawn) const
{
    auto itr = avoidSpawns.find((uint64(spawn.spawnId) << 1) | uint64(spawn.gameObject ? 1 : 0));
    return itr != avoidSpawns.end() && getMSTime() < itr->second;
}

void GuildAdventure::BotRuntime::Avoid(SpawnPoint const& spawn, uint32 durationMs)
{
    if (!spawn.spawnId)
        return;
    avoidSpawns[(uint64(spawn.spawnId) << 1) | uint64(spawn.gameObject ? 1 : 0)] = getMSTime() + durationMs;
}

bool GuildAdventure::BotRuntime::IsDangerous(uint32 questId, uint8 level) const
{
    auto itr = dangerousQuests.find(questId);
    return itr != dangerousQuests.end() && level <= itr->second.level && getMSTime() < itr->second.until;
}

bool GuildAdventure::BotRuntime::IsInAvoidedArea(uint32 mapId, float x, float y, uint8 level) const
{
    uint32 const now = getMSTime();
    for (AvoidArea const& area : avoidAreas)
    {
        if (area.mapId != mapId || level > area.level || now >= area.until)
            continue;
        float const dx = area.x - x;
        float const dy = area.y - y;
        if (dx * dx + dy * dy <= AvoidAreaRadius * AvoidAreaRadius)
            return true;
    }
    return false;
}

bool GuildAdventure::BotRuntime::CrossesAvoidedArea(uint32 mapId, float fromX, float fromY, float toX, float toY, uint8 level) const
{
    // The dangerous part of a set-aside place, and how much nearer to its middle a way must lead to count (a bot that
    // has just run out of a camp stands at its edge: a way that leads on outwards does not cross it).
    constexpr float CoreRadius = 60.0f;
    constexpr float NearerYards = 15.0f;
    uint32 const now = getMSTime();
    float const wx = toX - fromX;
    float const wy = toY - fromY;
    float const length2 = wx * wx + wy * wy;
    for (AvoidArea const& area : avoidAreas)
    {
        // Level 255: a spot the bot could not get to, no danger.
        if (area.mapId != mapId || area.level == 255 || level > area.level || now >= area.until)
            continue;
        float const ax = area.x - fromX;
        float const ay = area.y - fromY;
        float const along = length2 > 1.0f ? std::clamp((ax * wx + ay * wy) / length2, 0.0f, 1.0f) : 0.0f;
        float const cx = ax - along * wx;
        float const cy = ay - along * wy;
        float const closest = std::sqrt(cx * cx + cy * cy);
        if (closest <= CoreRadius && closest + NearerYards <= std::sqrt(ax * ax + ay * ay))
            return true;
    }
    return false;
}

void GuildAdventure::BotRuntime::AddAvoidArea(uint32 mapId, float x, float y, uint8 level, uint32 durationMs, bool deaths)
{
    uint32 const now = getMSTime();
    std::erase_if(avoidAreas, [now](AvoidArea const& area) { return now >= area.until; });
    avoidAreas.push_back({ mapId, x, y, level, now + durationMs, deaths });
}

bool GuildAdventure::BotRuntime::IsZoneUnreachable(uint32 zoneId) const
{
    auto itr = unreachableZones.find(zoneId);
    return itr != unreachableZones.end() && getMSTime() < itr->second;
}

char const* GuildAdventure::ToString(VoyagePhase phase)
{
    switch (phase)
    {
        case VoyagePhase::ToDock:    return "to dock";
        case VoyagePhase::Wait:      return "wait";
        case VoyagePhase::Board:     return "board";
        case VoyagePhase::Ride:      return "ride";
        case VoyagePhase::Disembark: return "disembark";
    }
    return "?";
}

char const* GuildAdventure::ToString(TaskKind kind)
{
    switch (kind)
    {
        case TaskKind::None:      return "none";
        case TaskKind::Travel:    return "travel";
        case TaskKind::Pickup:    return "pickup";
        case TaskKind::Objective: return "objective";
        case TaskKind::TurnIn:    return "turn-in";
        case TaskKind::Grind:     return "grind";
        case TaskKind::Vendor:    return "vendor";
        case TaskKind::Train:     return "train";
        case TaskKind::SafeSpot:  return "safe spot";
        case TaskKind::Hold:      return "hold";
        case TaskKind::Voyage:    return "voyage";
        case TaskKind::Discover:  return "discover";
        case TaskKind::Interact:  return "interact";
        case TaskKind::Escort:    return "escort";
        case TaskKind::Event:     return "event";
        case TaskKind::Fish:      return "fish";
    }
    return "none";
}

OrderService& OrderService::Instance()
{
    static OrderService instance;
    return instance;
}

// ---------------------------------------------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------------------------------------------

void OrderService::StepPullCheck(Player* player, BotRuntime& rt)
{
    constexpr uint32 PullAsideMs = 10 * MINUTE * IN_MILLISECONDS;
    float px = 0.0f, py = 0.0f;
    if (rt.pull.active || Escape::CheckPull(player, rt, px, py) != Escape::Pull::Refused)
        return;
    rt.pullHoldUntil = getMSTime() + MINUTE * IN_MILLISECONDS;
    rt.pullHoldMap = player->GetMapId();
    rt.pullHoldX = px;
    rt.pullHoldY = py;
    rt.AddAvoidArea(player->GetMapId(), px, py, player->GetLevel(), PullAsideMs, true);
    // (The spawn the task was for as well: the pack may stand more than the area's reach from it, and the planner
    // offered the same spawn again at once.)
    if (rt.task.hasSpot && (rt.task.kind == TaskKind::Objective || rt.task.kind == TaskKind::Interact ||
        rt.task.kind == TaskKind::Grind))
        rt.Avoid(rt.task.spot, PullAsideMs);
    // The second pull called off for one quest within half an hour: its targets stand in packs. The quest is set
    // aside like one the bot kept dying at (until the time is over or it is two levels on), so it turns to another
    // quest instead of walking from pack to pack (37 objectives ended this way in half an hour, with 9 hand-ins).
    if ((rt.task.kind == TaskKind::Objective || rt.task.kind == TaskKind::Interact) && rt.task.questId)
    {
        constexpr uint32 RefusedWindowMs = 30 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 RefusedAsideMs = 30 * MINUTE * IN_MILLISECONDS;
        uint32 const now = getMSTime();
        uint64 const key = (uint64(1) << 62) | (uint64(rt.task.questId) << 8);
        BotRuntime::FledTarget& refused = rt.fledTargets[key];
        if (refused.count && getMSTimeDiff(refused.at, now) > RefusedWindowMs)
            refused.count = 0;
        refused.at = now;
        if (++refused.count >= 2)
        {
            refused.count = 0;
            BotRuntime::Danger& danger = rt.dangerousQuests[rt.task.questId];
            danger.level = uint8(player->GetLevel() + 1);
            danger.until = now + RefusedAsideMs;
            LOG_INFO(ActivityLog, "activity: {} sets quest {} \"{}\" aside for {} min: two pulls called off, its targets stand in packs it "
                "cannot afford alone{}", rt.name, rt.task.questId, QuestTitle(rt.task.questId), RefusedAsideMs / (MINUTE * IN_MILLISECONDS),
                rt.pullPartnerHelps ? "; with a second guild character beside it the pull would do: the quest waits for a quest partner" : "");
            // A quest whose targets stand in packs that two characters could take is a hard quest (the owner, 2026-11-13: quest
            // partners for hard, elite and group quests): the guild bots that carry it pair up for it (QuestPartners.h).
            // (Quest 604 "The Bloodsail Buccaneers", ten swashbucklers: five of the guild's characters carried it without
            // progress, each turned away from the swashbucklers with their warlocks, imps and mages.)
            if (rt.pullPartnerHelps)
                sQuestPartners.OnTooHard(player, rt.task.questId, danger.level);
        }
    }
    if (rt.task.kind != TaskKind::None && rt.task.kind != TaskKind::Voyage)
        ClearTask(rt, 0, "a pull it cannot afford");
    BotDriver::Rest(player);
    // Out of the place the way it came, before the next task sends it through the rest of the camp.
    Escape::BackOut(player, rt);
}

void OrderService::StepGroundEffect(Player* player, BotRuntime& rt)
{
    if (!player->IsAlive() || !sGuildBotHolder.IsAIPlayed(player->GetGUID()))
    {
        rt.groundEffectSince = 0;
        return;
    }
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (now >= rt.nextAvoidAoeAssert)
    {
        BotDriver::KeepAvoidAoe(player);
        rt.nextAvoidAoeAssert = now + 5 * IN_MILLISECONDS;
    }
    std::string const effect = BotDriver::GroundEffectOn(player);
    if (effect.empty())
    {
        rt.groundEffectSince = 0;
        StepGroundEffectAhead(player, rt, now);
        return;
    }
    if (!rt.groundEffectSince)
        rt.groundEffectSince = now;
    // Through it quickly where no way leads around it (StepGroundEffectAhead): no step out, its walk goes on.
    DynamicObject const* inEffect = BotDriver::GroundEffectObject(player);
    if (rt.groundPassMode == 3 && now < rt.groundPassUntil && inEffect && inEffect->GetGUID() == rt.groundPassEffect)
        return;
    // A rest in a cloud is no rest (Retadin died twice in a Noxious Cloud in Maraudon, the second time out of any fight).
    if (rt.rest.active)
        EndRest(player, rt, "a ground effect");
    bool const stepped = StepOutOfGroundEffects(player) || BotDriver::StepOutOfGroundEffect(player);
    // (Its next look ahead at once: the walk that led it in would lead it in again.)
    rt.groundPassLookAt = 0;
    if (!rt.groundEffectLogAt || getMSTimeDiff(rt.groundEffectLogAt, now) >= 10 * IN_MILLISECONDS)
    {
        rt.groundEffectLogAt = now;
        LOG_INFO("module.ga.activity", "activity: {} stands in {} at {:.0f}% health{}: {}", player->GetName(), effect,
            player->GetHealthPct(), player->IsInCombat() ? " in a fight" : "", stepped ? "it steps out" : "no way out of it found");
    }
}

namespace
{
    // The hostile ground effects within `range` of the bot: dynamic objects of a spell that does harm, of a caster that is
    // no friend of it.
    std::vector<DynamicObject*> HostileGroundEffectsNear(Player* bot, float range)
    {
        std::list<WorldObject*> objects;
        Acore::AllWorldObjectsInRange check(bot, range);
        Acore::WorldObjectListSearcher<Acore::AllWorldObjectsInRange> searcher(bot, objects, check, GRID_MAP_TYPE_MASK_DYNAMICOBJECT);
        Cell::VisitObjects(bot, searcher, range);
        std::vector<DynamicObject*> effects;
        for (WorldObject* object : objects)
        {
            DynamicObject* effect = object->ToDynObject();
            if (!effect || !effect->IsInWorld() || effect->GetRadius() <= 0.0f)
                continue;
            Unit const* caster = effect->GetCaster();
            if (caster && !caster->IsHostileTo(bot))
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(effect->GetSpellId());
            if (!info || info->IsPositive())
                continue;
            effects.push_back(effect);
        }
        return effects;
    }

    // The way the bot's movement takes from where it stands, the next `ahead` yards (its path's own points), and where
    // the movement ends. False when it does not move.
    bool MovementAhead(Player* bot, float ahead, std::vector<G3D::Vector3>& way, G3D::Vector3& end)
    {
        way.clear();
        if (!bot->isMoving() || bot->movespline->Finalized())
            return false;
        auto const& path = bot->movespline->_Spline().getPoints();
        if (path.size() < 2)
            return false;
        end = path.back();
        G3D::Vector3 const here(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        std::size_t nearest = 0;
        float nearestDist = -1.0f;
        for (std::size_t i = 0; i < path.size(); ++i)
        {
            float const d = (path[i] - here).length();
            if (nearestDist < 0.0f || d < nearestDist)
            {
                nearest = i;
                nearestDist = d;
            }
        }
        way.push_back(here);
        float walked = 0.0f;
        for (std::size_t i = std::min(nearest + 1, path.size() - 1); i < path.size() && walked < ahead; ++i)
        {
            float const step = (path[i] - way.back()).length();
            if (step < 0.3f)
                continue;
            way.push_back(path[i]);
            walked += step;
        }
        return way.size() >= 2;
    }

    // How near the segment from a to b comes to the point (x, y), on the ground.
    float SegmentNear2d(G3D::Vector3 const& a, G3D::Vector3 const& b, float x, float y)
    {
        float const dx = b.x - a.x, dy = b.y - a.y;
        float const len2 = dx * dx + dy * dy;
        float t = len2 > 0.0f ? ((x - a.x) * dx + (y - a.y) * dy) / len2 : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        return std::hypot(a.x + t * dx - x, a.y + t * dy - y);
    }

    // A way around the effect's circle from where the bot stands to `exit` (a point of its way beyond the circle): the
    // shorter arc of the two, points on the ground every 30 degrees at the radius plus 3 yd, each in sight of the one
    // before and on about the bot's floor, none in another hostile effect. Empty when neither side has one.
    std::vector<G3D::Vector3> WayAround(Player* bot, DynamicObject const* effect, G3D::Vector3 const& exit,
        std::vector<DynamicObject*> const& others)
    {
        constexpr float Margin = 3.0f;
        constexpr float StepAngle = float(M_PI) / 6.0f;
        constexpr float MaxArc = 1.5f * float(M_PI);
        Map const* map = bot->GetMap();
        float const cx = effect->GetPositionX(), cy = effect->GetPositionY();
        float const radius = effect->GetRadius() + Margin;
        float const from = std::atan2(bot->GetPositionY() - cy, bot->GetPositionX() - cx);
        float const to = std::atan2(exit.y - cy, exit.x - cx);
        std::vector<G3D::Vector3> best;
        float bestArc = 0.0f;
        for (float const side : { 1.0f, -1.0f })
        {
            float arc = side * (to - from);
            while (arc < 0.0f)
                arc += 2.0f * float(M_PI);
            while (arc >= 2.0f * float(M_PI))
                arc -= 2.0f * float(M_PI);
            if (arc > MaxArc || (!best.empty() && arc >= bestArc))
                continue;
            std::vector<G3D::Vector3> points;
            G3D::Vector3 last(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            bool ok = true;
            for (float done = StepAngle; ok && done < arc; done += StepAngle)
            {
                float const angle = from + side * done;
                float const x = cx + std::cos(angle) * radius;
                float const y = cy + std::sin(angle) * radius;
                float const z = bot->GetMapHeight(x, y, last.z + 3.0f);
                if (z <= INVALID_HEIGHT || std::fabs(z - last.z) > 4.0f ||
                    !map->isInLineOfSight(last.x, last.y, last.z + 1.5f, x, y, z + 1.5f, bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                        VMAP::ModelIgnoreFlags::Nothing))
                {
                    ok = false;
                    break;
                }
                for (DynamicObject const* other : others)
                    if (other != effect && std::hypot(other->GetPositionX() - x, other->GetPositionY() - y) <= other->GetRadius() + 1.0f)
                        ok = false;
                last = G3D::Vector3(x, y, z);
                points.push_back(last);
            }
            if (!ok)
                continue;
            if (std::fabs(exit.z - last.z) > 4.0f || !map->isInLineOfSight(last.x, last.y, last.z + 1.5f, exit.x, exit.y, exit.z + 1.5f,
                    bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                continue;
            points.push_back(exit);
            best = std::move(points);
            bestArc = arc;
        }
        return best;
    }

    std::string GroundEffectName(DynamicObject const* effect)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(effect->GetSpellId());
        return info ? std::string(info->SpellName[LOCALE_enUS]) : std::string("a ground effect");
    }
}

bool OrderService::StepOutOfGroundEffects(Player* player)
{
    // mod-playerbots' step out looks at the one effect it stands in, and its spot lay in the next cloud: the owner's rogue in the
    // Clouds of Disease of three Diseased Ghouls in Scholomance stepped from one into the other and lost half its health
    // out of any fight, 2026-11-26. The nearest spot outside every hostile effect within 30 yd: 12 directions, 4 to 14 yd
    // away, on about its floor and in its sight.
    std::vector<DynamicObject*> const effects = HostileGroundEffectsNear(player, 30.0f);
    if (effects.empty())
        return false;
    Map const* map = player->GetMap();
    auto inside = [&](float x, float y)
    {
        for (DynamicObject const* effect : effects)
            if (std::hypot(effect->GetPositionX() - x, effect->GetPositionY() - y) <= effect->GetRadius() + 1.5f)
                return true;
        return false;
    };
    float const px = player->GetPositionX(), py = player->GetPositionY(), pz = player->GetPositionZ();
    for (float const yards : { 4.0f, 7.0f, 10.0f, 14.0f })
    {
        float bestX = 0.0f, bestY = 0.0f, bestZ = 0.0f;
        float bestScore = -1.0f;
        for (int step = 0; step < 12; ++step)
        {
            float const angle = float(step) * float(M_PI) / 6.0f;
            float const x = px + std::cos(angle) * yards;
            float const y = py + std::sin(angle) * yards;
            if (inside(x, y))
                continue;
            float const z = player->GetMapHeight(x, y, pz + 3.0f);
            if (z <= INVALID_HEIGHT || std::fabs(z - pz) > 4.0f ||
                !map->isInLineOfSight(px, py, pz + 1.5f, x, y, z + 1.5f, player->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                    VMAP::ModelIgnoreFlags::Nothing))
                continue;
            // (Of the spots at the same distance: the one nearest to what it fights, else to where it was going.)
            float score = 0.0f;
            if (Unit const* victim = player->GetVictim())
                score = victim->GetExactDist2d(x, y);
            else if (player->isMoving() && !player->movespline->Finalized())
            {
                G3D::Vector3 const end = player->movespline->FinalDestination();
                score = std::hypot(end.x - x, end.y - y);
            }
            if (bestScore < 0.0f || score < bestScore)
            {
                bestScore = score;
                bestX = x;
                bestY = y;
                bestZ = z;
            }
        }
        if (bestScore >= 0.0f)
        {
            BotDriver::PauseAI(player, 1200);
            player->GetMotionMaster()->Clear();
            player->GetMotionMaster()->MovePoint(0, bestX, bestY, bestZ, FORCED_MOVEMENT_RUN);
            return true;
        }
    }
    return false;
}

void OrderService::StepGroundEffectAhead(Player* player, BotRuntime& rt, uint32 now)
{
    // the owner, 2026-11-25: [quote left out].
    // The step out of an effect (StepGroundEffect) did nothing about the walk that led in: its mover (the follow of the
    // group's tank, the planner's walk, NewRpg) set it again at once, and the bot was in again (a hunter in a Cloud of
    // Disease in Scholomance stepped out four times in half a minute out of any fight, and its rest ended in it). So the
    // way ahead is looked at: around an effect it would lead through, while the effect lasts; at its edge while the
    // movement ends in it or no way leads around one that ends soon; through it quickly when no way leads around one that
    // lasts.
    constexpr float AheadYards = 25.0f;
    constexpr float LookYards = 40.0f;
    constexpr uint32 LookMs = 500;
    constexpr int32 WaitForMs = 15 * IN_MILLISECONDS;
    constexpr uint32 EdgeHoldMs = 1500;
    if (rt.groundPassMode && now >= rt.groundPassUntil)
        rt.groundPassMode = 0;
    // (In a fight the fight's own places count: mod-playerbots' positions and the step out of an effect.)
    if (player->IsInCombat() || player->GetTransport() || player->IsInFlight() || player->IsBeingTeleported())
        return;
    if (rt.groundPassMode == 1 && !player->movespline->Finalized())
        return; // on its way around
    // (While it waits at the edge it looks four times a second: a follow sets its walk again after the hold, and in half a
    // second it was in again, Scholomance on the j5 instance.)
    if (rt.groundPassLookAt && now < rt.groundPassLookAt && rt.groundPassMode != 2)
        return;
    rt.groundPassLookAt = now + LookMs;
    std::vector<G3D::Vector3> way;
    G3D::Vector3 end;
    if (!MovementAhead(player, AheadYards, way, end))
        return;
    std::vector<DynamicObject*> const effects = HostileGroundEffectsNear(player, LookYards);
    if (effects.empty())
        return;
    // The first effect its way leads into, and where along the way.
    DynamicObject* hit = nullptr;
    std::size_t hitSegment = 0;
    for (std::size_t i = 1; i < way.size() && !hit; ++i)
        for (DynamicObject* effect : effects)
            if (SegmentNear2d(way[i - 1], way[i], effect->GetPositionX(), effect->GetPositionY()) <= effect->GetRadius() + 1.0f &&
                std::fabs(way[i].z - effect->GetPositionZ()) <= 8.0f)
            {
                hit = effect;
                hitSegment = i;
                break;
            }
    if (!hit)
        return;
    float const radius = hit->GetRadius();
    float const cx = hit->GetPositionX(), cy = hit->GetPositionY();
    std::string const name = GroundEffectName(hit);
    int32 const left = hit->GetDuration();   // ms; 0 or less: no end
    bool const fresh = rt.groundPassEffect != hit->GetGUID();
    auto holdAtEdge = [&](char const* why)
    {
        // At the edge on its side: the mover sets the walk again after the hold, and the next look holds it again.
        float const angle = std::atan2(player->GetPositionY() - cy, player->GetPositionX() - cx);
        float const x = cx + std::cos(angle) * (radius + 3.5f);
        float const y = cy + std::sin(angle) * (radius + 3.5f);
        float const z = player->GetMapHeight(x, y, player->GetPositionZ() + 3.0f);
        BotDriver::PauseAI(player, EdgeHoldMs);
        player->StopMoving();
        player->GetMotionMaster()->Clear();
        if (z > INVALID_HEIGHT && std::fabs(z - player->GetPositionZ()) <= 4.0f && player->GetExactDist2d(x, y) > 1.0f &&
            std::hypot(player->GetPositionX() - cx, player->GetPositionY() - cy) > radius + 3.0f)
            player->GetMotionMaster()->MovePoint(0, x, y, z);
        if (fresh || rt.groundPassLogged != 2)
            LOG_INFO(ActivityLog, "activity: {} waits at the edge of {} ({:.0f} yd across, {}): {}", player->GetName(), name,
                2.0f * radius, left > 0 ? Acore::StringFormat("{} s left", left / IN_MILLISECONDS) : std::string("no end"), why);
        rt.groundPassEffect = hit->GetGUID();
        rt.groundPassMode = 2;
        rt.groundPassLogged = 2;
        rt.groundPassUntil = now + EdgeHoldMs;
    };
    // Its movement ends in the effect (the place beside its tank, its target): at the edge until the effect is gone or
    // the movement leads elsewhere.
    if (std::hypot(end.x - cx, end.y - cy) <= radius + 0.5f)
    {
        holdAtEdge("its walk ends in it");
        return;
    }
    // A point of its way beyond the circle to walk around to.
    G3D::Vector3 exit = end;
    for (std::size_t i = hitSegment; i < way.size(); ++i)
        if (std::hypot(way[i].x - cx, way[i].y - cy) > radius + 2.0f)
        {
            exit = way[i];
            break;
        }
    if (std::hypot(exit.x - cx, exit.y - cy) <= radius + 2.0f)
    {
        // (The look ahead ends in it: a point straight across.)
        float const angle = std::atan2(cy - player->GetPositionY(), cx - player->GetPositionX());
        exit.x = cx + std::cos(angle) * (radius + 3.0f);
        exit.y = cy + std::sin(angle) * (radius + 3.0f);
        exit.z = player->GetMapHeight(exit.x, exit.y, player->GetPositionZ() + 3.0f);
        if (exit.z <= INVALID_HEIGHT)
            exit.z = player->GetPositionZ();
    }
    std::vector<G3D::Vector3> around = WayAround(player, hit, exit, effects);
    if (!around.empty())
    {
        Movement::PointsArray points;
        points.emplace_back(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
        float yards = 0.0f;
        for (G3D::Vector3 const& point : around)
        {
            yards += (point - points.back()).length();
            points.push_back(point);
        }
        uint32 const walkMs = uint32(yards / std::max(player->GetSpeed(MOVE_RUN), 1.0f) * float(IN_MILLISECONDS)) + 500;
        BotDriver::PauseAI(player, std::min<uint32>(walkMs, 8 * IN_MILLISECONDS));
        player->StopMoving();
        player->GetMotionMaster()->Clear();
        player->GetMotionMaster()->MoveSplinePath(&points, FORCED_MOVEMENT_RUN);
        if (fresh || rt.groundPassLogged != 1)
            LOG_INFO(ActivityLog, "activity: {} walks around {} ({:.0f} yd across) on its way: {:.0f} yd instead of through it",
                player->GetName(), name, 2.0f * radius, yards);
        rt.groundPassEffect = hit->GetGUID();
        rt.groundPassMode = 1;
        rt.groundPassLogged = 1;
        rt.groundPassUntil = now + walkMs;
        return;
    }
    // No way around: an effect that ends soon is waited for at its edge, one that lasts is gone through quickly (no step
    // out on the way).
    if (left > 0 && left <= WaitForMs)
    {
        holdAtEdge("no way leads around it, and it ends soon");
        return;
    }
    if (rt.groundPassMode != 3 || fresh)
    {
        bool const logIt = fresh || rt.groundPassLogged != 3;
        float const across = 2.0f * radius + 4.0f;
        uint32 const crossMs = uint32(across / std::max(player->GetSpeed(MOVE_RUN), 1.0f) * float(IN_MILLISECONDS)) + 2 * IN_MILLISECONDS;
        if (logIt)
            LOG_INFO(ActivityLog, "activity: {} goes through {} ({:.0f} yd across, {}) quickly: no way leads around it", player->GetName(),
                name, 2.0f * radius, left > 0 ? Acore::StringFormat("{} s left", left / IN_MILLISECONDS) : std::string("no end"));
        rt.groundPassEffect = hit->GetGUID();
        rt.groundPassMode = 3;
        rt.groundPassLogged = 3;
        rt.groundPassUntil = now + crossMs;
    }
}

namespace
{
    // The planner's passes (docs/performance.md PF-1): a character is stepped once a period, at a phase of its own.
    constexpr uint32 PlanPeriodMs = 1000;
    constexpr uint32 FastPeriodMs = 250;
    // Entries of the due list not stepped for this long belong to characters that are gone.
    constexpr uint64 DueStaleMs = 5 * MINUTE * IN_MILLISECONDS;
    // At most one activity line for a slow planner tick in this long.
    constexpr uint64 SlowTickLogGapMs = 10 * IN_MILLISECONDS;

    uint32 PhaseOf(uint64 key, uint32 period)
    {
        return uint32((key * 2654435761ull) % period);
    }

    // The first time at or after `from` that lies at `phase` within the period.
    uint64 NextAtPhase(uint64 from, uint32 phase, uint32 period)
    {
        uint64 const base = from - from % period + phase;
        return base >= from ? base : base + period;
    }

    // The phase of a character: a squad member right after its leader (the leader is stepped first, as in the old pass,
    // where a guild's bots came one after the other).
    uint32 PlanPhaseOf(ObjectGuid guid)
    {
        ObjectGuid const leader = sSquadService.GetLeader(guid);
        return leader.IsEmpty() ? PhaseOf(guid.GetCounter(), PlanPeriodMs) : (PhaseOf(leader.GetCounter(), PlanPeriodMs) + 1) % PlanPeriodMs;
    }
}

void OrderService::StepFast(ObjectGuid guid, BotRuntime& rt)
{
    // An item for a weakened creature: the fight is watched four times a second as well (a warrior took a raptor from 40 %
    // to dead between two looks once a second).
    if (rt.task.kind == TaskKind::Interact && (rt.task.plan.weakenBelow || !rt.task.plan.beatEntries.empty()) && !rt.guid.IsEmpty())
        if (Player* fighter = sGuildBotHolder.GetPlannedPlayer(guid); fighter && fighter->IsInWorld() && fighter->IsInCombat())
            StepWeaken(fighter, rt);
    // The conductor of a dungeon group (package P2): the bot that leads a run inside.
    if (rt.dungeon.id && !rt.guid.IsEmpty())
        if (Player* lead = sGuildBotHolder.GetPlannedPlayer(guid); lead && lead->IsInWorld())
        {
            StepDungeonConductor(lead, rt);
            StepRaidCthunFast(lead, rt);
            StepDungeonHealer(lead, rt);
            StepDungeonMetricsTick(lead, rt);
        }
    // Out of a ground effect (the owner: [quote left out]).
    if (!rt.guid.IsEmpty())
        if (Player* standing = sGuildBotHolder.GetPlannedPlayer(guid); standing && standing->IsInWorld() && !standing->IsBeingTeleported())
        {
            // A ship's passenger that a teleport took off the deck keeps the core's "no path finding" state: a ship adds it
            // to its passengers (MotionTransport::AddPassenger) and only a removal "with all" takes it away again, which
            // Player::TeleportTo does not do. From then on every path the core computes for the bot is a straight line
            // ("not using a path"), the module's checks find it off the walkable ground on any ground, and the rescues
            // put it at a graveyard every 36 s: Probehunter, taken from The Lady Mehley at Menethil to its leader in
            // Northshire by a FOLLOW order, 45 rescues in round 207.
            if (!standing->GetTransport() && standing->HasUnitState(UNIT_STATE_IGNORE_PATHFINDING))
            {
                standing->ClearUnitState(UNIT_STATE_IGNORE_PATHFINDING);
                LOG_INFO(ActivityLog, "activity: {} is off its ship but still had the state of a ship's passenger (no path finding): "
                    "cleared", standing->GetName());
            }
            StepGroundEffect(standing, rt);
            // A Starfall whose stars would pull a creature in no fight ends (Owldudu, a balance druid at the level cap,
            // died three times in a day in Silithus with three to five creatures on it after a pull of one, round 170:
            // the stars fell on what stood 30 yd around it while it moved in its fight).
            if (sGuildBotHolder.IsAIPlayed(standing->GetGUID()))
            {
                // Bloodlord Mandokir's Threatening Gaze (Zul'Gurub): when it runs out on a member that gained any
                // threat on him meanwhile (a blow, a spell, a heal), he charges it and kills it with Guillotine, and
                // every kill makes him stronger (Level Up). A member he watches does nothing until the gaze is gone:
                // the owner's raid of 20 lost three members to it and then the raid, 2026-11-26.
                constexpr uint32 ThreateningGaze = 24314;
                // (The world thread's: StepFast runs in OrderService::Update.)
                static std::unordered_set<ObjectGuid> gazeHeld;
                if (standing->HasAura(ThreateningGaze))
                {
                    BotDriver::PauseAI(standing, 750);
                    if (standing->GetVictim())
                        standing->AttackStop();
                    if (standing->IsNonMeleeSpellCast(true))
                        standing->InterruptNonMeleeSpells(true);
                    if (gazeHeld.insert(standing->GetGUID()).second)
                    {
                        LOG_INFO(ActivityLog, "activity: {} stops attacking and casting while Bloodlord Mandokir "
                            "watches it", standing->GetName());
                    }
                }
                else
                    gazeHeld.erase(standing->GetGUID());
                Escape::StepStarfall(standing, rt);
                Escape::StepSealOfCommand(standing, rt);
                Escape::StepInterrupt(standing, rt);
                Escape::StepStopRunner(standing, rt);
                Escape::StepChaseRunner(standing, rt);
                Escape::StepKnockbackSide(standing, rt);
            }
        }
    if ((!rt.pullCheckOn && !rt.pull.active) || rt.guid.IsEmpty())
        return;
    Player* player = sGuildBotHolder.GetPlannedPlayer(guid);
    if (!player || !player->IsInWorld() || player->IsBeingTeleported())
    {
        rt.pull = BotRuntime::CarefulPull();
        return;
    }
    // A careful pull in progress (FightEscape.h): its steps, in and out of combat.
    if (rt.pull.active)
    {
        Escape::StepPull(player, rt);
        return;
    }
    if (rt.pullCheckOn && player->IsAlive() && !player->IsInCombat())
        StepPullCheck(player, rt);
    else if (rt.pullCheckOn && player->IsAlive())
        Escape::DropBystander(player, rt);
}

void OrderService::StepGuild(AdventureGuild& guild, bool pairSquads)
{
    if (pairSquads)
        sSquadService.AutoPair(guild);
    // Quest partners (QuestPartners.h) pair up and part on a clock of their own.
    sQuestPartners.Update(guild);
    // Recipes in the guild storage go to whoever can learn them (once a minute, like the squads' look).
    if (pairSquads)
        sProfessions.LearnStoredRecipes(guild);
}

void OrderService::Update(uint32 diff)
{
    Perf::Counters& counters = Perf::Get();
    Perf::Counters before;
    before.routeSearches = counters.routeSearches.load(std::memory_order_relaxed);
    before.routeUs = counters.routeUs.load(std::memory_order_relaxed);
    before.walkProbes = counters.walkProbes.load(std::memory_order_relaxed);
    before.walkUs = counters.walkUs.load(std::memory_order_relaxed);
    before.walkHits = counters.walkHits.load(std::memory_order_relaxed);
    before.flights = counters.flights.load(std::memory_order_relaxed);
    before.errands = counters.errands.load(std::memory_order_relaxed);
    before.zonePicks = counters.zonePicks.load(std::memory_order_relaxed);
    uint64 const startUs = Perf::NowUs();

    TickRecord record;
    record.tickMs = diff;
    if (GetSettings().plannerSpread)
        UpdateSpread(diff, record);
    else
        UpdateAllAtOnce(diff);
    RecordTick(record, startUs, before);
}

void OrderService::UpdateAllAtOnce(uint32 diff)
{
    // The pull check runs four times a second: a charge or an instant spell starts the fight within a fraction of a
    // second of the AI's choice, and a check once a second came too late for those.
    constexpr uint32 PullCheckMs = 250;
    _pullTimer += diff;
    if (_pullTimer >= PullCheckMs)
    {
        _pullTimer = 0;
        for (auto& [guid, rt] : _runtime)
            StepFast(guid, rt);
    }

    _timer += diff;
    if (_timer < TickMs)
        return;
    _timer = 0;

    // Automatic squads (GuildAdventure.Squads.Auto) are looked at once a minute.
    uint32 const now = getMSTime();
    bool const pairSquads = now >= _nextSquadPairing;
    if (pairSquads)
        _nextSquadPairing = now + MINUTE * IN_MILLISECONDS;

    for (auto& [owner, guild] : sGuildAdventureMgr.GetGuilds())
    {
        StepGuild(guild, pairSquads);
        for (auto& [guid, bot] : guild.bots)
            Step(guild, bot);

        // The leader is planned only while it has an order; without one the hold policy keeps it (GuildBotHolder).
        if (guild.leaderPlanned)
            Step(guild, guild.leaderPlan);
        else
            _runtime.erase(guild.leader);
    }
}

void OrderService::UpdateSpread(uint32 diff, TickRecord& record)
{
    Settings const& settings = GetSettings();
    Perf::Counters& counters = Perf::Get();
    uint64 const now = Perf::NowMs();

    // The quick checks, four times a second per character, each at a phase of its own.
    for (auto& [guid, rt] : _runtime)
    {
        uint32 const phase = PhaseOf(guid.GetCounter(), FastPeriodMs);
        if (!rt.fastDue)
            rt.fastDue = NextAtPhase(now, phase, FastPeriodMs);
        if (rt.fastDue > now)
            continue;
        rt.fastDue = NextAtPhase(now + FastPeriodMs / 4 + 1, phase, FastPeriodMs);
        StepFast(guid, rt);
    }

    // The planner's pass: what is due, oldest first.
    struct Due
    {
        uint64 due;
        uint32 owner;
        ObjectGuid guid;
        uint8 kind; // 0 the guild's own work, 1 a roster bot, 2 the leader
    };
    std::vector<Due> due;
    auto& guilds = sGuildAdventureMgr.GetGuilds();
    auto consider = [&](uint32 owner, ObjectGuid guid, uint8 kind)
    {
        uint64& at = _planDue[guid];
        if (!at)
            at = NextAtPhase(now, PlanPhaseOf(guid), PlanPeriodMs);
        if (at <= now)
            due.push_back({ at, owner, guid, kind });
    };
    for (auto& [owner, guild] : guilds)
    {
        uint64& at = _guildDue[owner];
        if (!at)
            at = NextAtPhase(now, PhaseOf(uint64(owner) + 0x5bd1e995ull, PlanPeriodMs), PlanPeriodMs);
        if (at <= now)
            due.push_back({ at, owner, ObjectGuid::Empty, 0 });
        for (auto const& [guid, bot] : guild.bots)
            consider(owner, guid, 1);
        if (!guild.leader.IsEmpty())
            consider(owner, guild.leader, 2);
    }
    std::stable_sort(due.begin(), due.end(), [](Due const& a, Due const& b) { return a.due < b.due; });

    // The budget: GuildAdventure.Planner.BudgetMs of this world tick, at least a quarter of the last one, and (when
    // set) at most SearchesPerTick expensive searches. What is left waits for the next tick, first in line. At least one
    // step per tick. A planner that has fallen behind by more than CatchUpLateMs may take as long as the last tick took:
    // a character's escape, rest and fight checks run in its step, and a bot stepped seconds late dies (the first run
    // with a cap of two searches per tick left steps 22 s late while 1600 bots looked for their zones).
    constexpr uint64 CatchUpLateMs = 500;
    uint64 const startUs = Perf::NowUs();
    uint64 budgetUs = std::max<uint64>(uint64(settings.plannerBudgetMs) * 1000, uint64(diff) * 250);
    if (!due.empty() && now - due.front().due > CatchUpLateMs)
        budgetUs = std::max<uint64>(budgetUs, uint64(diff) * 1000);
    uint64 const searchesAtStart = counters.Expensive();
    for (std::size_t i = 0; i < due.size(); ++i)
    {
        Due const& entry = due[i];
        if (record.stepped && (Perf::NowUs() - startUs >= budgetUs ||
            (settings.plannerSearchesPerTick && counters.Expensive() - searchesAtStart >= settings.plannerSearchesPerTick)))
        {
            record.deferred = uint16(std::min<std::size_t>(due.size() - i, 65535));
            break;
        }
        auto guildItr = guilds.find(entry.owner);
        if (guildItr == guilds.end())
            continue;
        AdventureGuild& guild = guildItr->second;
        if (!record.stepped)
            record.lateMs = uint32(std::min<uint64>(now - entry.due, 0xFFFFFFFFu));
        uint64 const stepStartUs = Perf::NowUs();
        switch (entry.kind)
        {
            case 0:
            {
                uint64& pairAt = _guildPairDue[entry.owner];
                bool const pairSquads = now >= pairAt;
                if (pairSquads)
                    pairAt = now + MINUTE * IN_MILLISECONDS;
                StepGuild(guild, pairSquads);
                _guildDue[entry.owner] = NextAtPhase(now + PlanPeriodMs / 4 + 1, PhaseOf(uint64(entry.owner) + 0x5bd1e995ull, PlanPeriodMs),
                    PlanPeriodMs);
                break;
            }
            case 1:
                if (GuildBot* bot = guild.FindBot(entry.guid))
                    Step(guild, *bot);
                _planDue[entry.guid] = NextAtPhase(now + PlanPeriodMs / 4 + 1, PlanPhaseOf(entry.guid), PlanPeriodMs);
                break;
            default:
                // The leader is planned only while it has an order; without one the hold policy keeps it (GuildBotHolder).
                if (guild.leader == entry.guid && guild.leaderPlanned)
                    Step(guild, guild.leaderPlan);
                else
                    _runtime.erase(entry.guid);
                _planDue[entry.guid] = NextAtPhase(now + PlanPeriodMs / 4 + 1, PlanPhaseOf(entry.guid), PlanPeriodMs);
                break;
        }
        ++record.stepped;
        if (entry.kind)
            NoteStepCost(record, uint32(std::min<uint64>(Perf::NowUs() - stepStartUs, 0xFFFFFFFFu)), entry.guid);
    }

    // Characters that left (benched, dismissed, the guild gone) leave their entries behind: they are pruned now and then.
    if (now >= _nextDuePrune)
    {
        _nextDuePrune = now + MINUTE * IN_MILLISECONDS;
        for (auto itr = _planDue.begin(); itr != _planDue.end();)
            itr = itr->second + DueStaleMs < now ? _planDue.erase(itr) : std::next(itr);
        for (auto itr = _guildDue.begin(); itr != _guildDue.end();)
            itr = !guilds.count(itr->first) ? _guildDue.erase(itr) : std::next(itr);
    }
}

void OrderService::NoteStepCost(TickRecord& record, uint32 us, ObjectGuid guid)
{
    // The three most expensive steps of the tick, most expensive first.
    for (std::size_t slot = 0; slot < 3; ++slot)
    {
        if (us <= record.slowest[slot].us)
            continue;
        for (std::size_t move = 2; move > slot; --move)
            record.slowest[move] = record.slowest[move - 1];
        auto itr = _runtime.find(guid);
        record.slowest[slot] = StepCost{ us, guid, itr != _runtime.end() ? itr->second.task.kind : TaskKind::None };
        return;
    }
}

void OrderService::RecordTick(TickRecord& record, uint64 startUs, Perf::Counters const& before)
{
    Perf::Counters& counters = Perf::Get();
    uint64 const nowUs = Perf::NowUs();
    auto delta = [](std::atomic<uint64> const& now, std::atomic<uint64> const& then)
    {
        return uint32(std::min<uint64>(now.load(std::memory_order_relaxed) - then.load(std::memory_order_relaxed), 0xFFFFFFFFu));
    };
    record.atMs = nowUs / 1000;
    record.plannerUs = uint32(std::min<uint64>(nowUs - startUs, 0xFFFFFFFFu));
    record.routes = delta(counters.routeSearches, before.routeSearches);
    record.routeUs = delta(counters.routeUs, before.routeUs);
    record.probes = delta(counters.walkProbes, before.walkProbes);
    record.probeUs = delta(counters.walkUs, before.walkUs);
    record.hits = delta(counters.walkHits, before.walkHits);
    record.flights = uint16(std::min<uint32>(delta(counters.flights, before.flights), 65535));
    record.errands = uint16(std::min<uint32>(delta(counters.errands, before.errands), 65535));
    record.zonePicks = uint16(std::min<uint32>(delta(counters.zonePicks, before.zonePicks), 65535));

    _perfTicks.push_back(record);
    while (!_perfTicks.empty() && _perfTicks.front().atMs + MINUTE * IN_MILLISECONDS < record.atMs)
        _perfTicks.pop_front();

    // A slow planner tick (GuildAdventure.Perf.SlowTickMs): one line, at most every few seconds.
    uint32 const slowMs = GetSettings().perfSlowTickMs;
    if (!slowMs || record.plannerUs < slowMs * 1000)
        return;
    if (record.atMs < _nextSlowTickLog)
    {
        ++_slowTicksQuiet;
        return;
    }
    _nextSlowTickLog = record.atMs + SlowTickLogGapMs;
    std::string steps;
    for (StepCost const& cost : record.slowest)
    {
        if (!cost.us)
            break;
        std::string name;
        if (auto itr = _runtime.find(cost.guid); itr != _runtime.end() && !itr->second.name.empty())
            name = itr->second.name;
        else if (!sCharacterCache->GetCharacterNameByGuid(cost.guid, name))
            name = cost.guid.ToString();
        steps += Acore::StringFormat("{}{} ({}) {:.1f} ms", steps.empty() ? "" : ", ", name, ToString(cost.kind), float(cost.us) / 1000.0f);
    }
    LOG_INFO(ActivityLog, "activity: the planner took {:.0f} ms in one world tick (tick {} ms; {} steps, {} left for the next tick; "
        "{} route searches in {:.0f} ms, {} walks asked of the path finder in {:.0f} ms, {} from the cache; {} flights, {} errands, "
        "{} zone picks looked for){}{}", float(record.plannerUs) / 1000.0f, record.tickMs, record.stepped, record.deferred, record.routes,
        float(record.routeUs) / 1000.0f, record.probes, float(record.probeUs) / 1000.0f, record.hits, record.flights, record.errands,
        record.zonePicks, steps.empty() ? "" : "; the most expensive steps: " + steps,
        _slowTicksQuiet ? Acore::StringFormat(" ({} more such ticks since the last line)", _slowTicksQuiet) : std::string());
    _slowTicksQuiet = 0;
}

std::vector<std::string> OrderService::DescribePerf() const
{
    std::vector<std::string> lines;
    Settings const& settings = GetSettings();
    lines.push_back(Acore::StringFormat("Planner: {} (budget {} ms per world tick, expensive searches per tick: {}); route cache {} ({} walks "
        "remembered); slow tick line from {} ms", settings.plannerSpread ? "spread over the ticks" : "all characters in one tick a second",
        settings.plannerBudgetMs, settings.plannerSearchesPerTick ? std::to_string(settings.plannerSearchesPerTick) : std::string("no cap"),
        settings.plannerRouteCache ? "on" : "off", WalkCacheSize(),
        settings.perfSlowTickMs));
    if (_perfTicks.size() < 2)
    {
        lines.push_back("No world ticks counted yet.");
        return lines;
    }
    float const seconds = std::max(0.001f, float(_perfTicks.back().atMs - _perfTicks.front().atMs) / 1000.0f);
    std::vector<uint32> planner;
    planner.reserve(_perfTicks.size());
    uint64 plannerUs = 0, routeUs = 0, probeUs = 0, tickMs = 0;
    uint64 stepped = 0, routes = 0, probes = 0, hits = 0, flights = 0, errands = 0, zones = 0;
    uint32 cutTicks = 0, maxLate = 0;
    StepCost slowest;
    for (TickRecord const& record : _perfTicks)
    {
        planner.push_back(record.plannerUs);
        plannerUs += record.plannerUs;
        tickMs += record.tickMs;
        stepped += record.stepped;
        routes += record.routes;
        routeUs += record.routeUs;
        probes += record.probes;
        probeUs += record.probeUs;
        hits += record.hits;
        flights += record.flights;
        errands += record.errands;
        zones += record.zonePicks;
        if (record.deferred)
            ++cutTicks;
        maxLate = std::max(maxLate, record.lateMs);
        if (record.slowest[0].us > slowest.us)
            slowest = record.slowest[0];
    }
    std::sort(planner.begin(), planner.end());
    auto pct = [&](float p) { return float(planner[std::min<std::size_t>(planner.size() - 1, std::size_t(p * float(planner.size())))]) / 1000.0f; };
    std::size_t const ticks = _perfTicks.size();
    lines.push_back(Acore::StringFormat("Last {:.0f} s: {} world ticks ({:.0f} a second, mean diff {:.1f} ms); the planner {:.1f} ms of every second, "
        "per tick mean {:.2f} / p95 {:.2f} / max {:.2f} ms", seconds, ticks, float(ticks) / seconds, float(tickMs) / float(ticks),
        float(plannerUs) / 1000.0f / seconds, float(plannerUs) / 1000.0f / float(ticks), pct(0.95f), float(planner.back()) / 1000.0f));
    lines.push_back(Acore::StringFormat("Steps: {:.0f} a second; {} ticks left steps for the next one (budget), the oldest step was up to {} ms late",
        float(stepped) / seconds, cutTicks, maxLate));
    lines.push_back(Acore::StringFormat("Route searches: {:.1f} a second, {:.2f} ms each ({:.1f} ms of every second); walks asked of the path "
        "finder {:.1f} a second, {:.2f} ms each; from the cache {:.1f} a second ({:.0f} % of the walks)", float(routes) / seconds,
        routes ? float(routeUs) / 1000.0f / float(routes) : 0.0f, float(routeUs) / 1000.0f / seconds, float(probes) / seconds,
        probes ? float(probeUs) / 1000.0f / float(probes) : 0.0f, float(hits) / seconds, probes + hits ? 100.0f * float(hits) / float(probes + hits) : 0.0f));
    lines.push_back(Acore::StringFormat("Expensive searches: flights {:.1f}, errands {:.1f}, zone picks {:.1f} a second", float(flights) / seconds,
        float(errands) / seconds, float(zones) / seconds));
    if (slowest.us)
    {
        std::string name;
        if (auto itr = _runtime.find(slowest.guid); itr != _runtime.end() && !itr->second.name.empty())
            name = itr->second.name;
        else if (!sCharacterCache->GetCharacterNameByGuid(slowest.guid, name))
            name = slowest.guid.ToString();
        lines.push_back(Acore::StringFormat("The slowest step of the minute: {} ({}) {:.1f} ms", name, ToString(slowest.kind),
            float(slowest.us) / 1000.0f));
    }
    return lines;
}

void OrderService::Step(AdventureGuild& guild, GuildBot& bot)
{
    ObjectGuid const guid = bot.guid;

    // AI-driven, or controlled in watch mode (the owner's client watches the AI play it, docs/take-control.md §5.12):
    // both are planned the same way.
    Player* player = sGuildBotHolder.GetPlannedPlayer(guid);
    if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld())
    {
        // Offline or controlled: keep sampling a controlled bot, forget the plan of an offline one.
        if (Player* controlled = ObjectAccessor::FindConnectedPlayer(guid))
        {
            auto itr = _runtime.find(guid);
            if (itr != _runtime.end())
            {
                Sample(bot, controlled, itr->second);
                ClearTask(itr->second, 0, "the owner plays the bot");
                // (A rest the AI had begun is over: it stayed open for the seven minutes its owner played, and the
                // line then read "rested 425 s".)
                if (itr->second.rest.active)
                    EndRest(controlled, itr->second, "the owner plays it");
                itr->second.why.clear();
                // Where the owner takes it is no jump of the planner's (watch mode plans from there).
                itr->second.hasLastPosition = false;
            }
        }
        else
            _runtime.erase(guid);
        return;
    }

    BotRuntime& rt = _runtime[guid];
    // The time since its last step (PF-1): about a second, more when the planner stretches its period under load.
    {
        uint64 const stepNow = Perf::NowMs();
        rt.stepGapMs = rt.lastStepAt ? uint32(std::clamp<uint64>(stepNow - rt.lastStepAt, 100, 10 * IN_MILLISECONDS)) : TickMs;
        rt.lastStepAt = stepNow;
    }
    if (rt.guid.IsEmpty())
    {
        rt.since = getMSTime();
        rt.nextStatus = rt.since + StatusLogMs;
        rt.name = bot.name;
        rt.playerClass = player->getClass();
        LOG_INFO(ActivityLog, "activity: {} entered the world: level {} in {} at ({:.0f}, {:.0f}) map {}, order {}",
            bot.name, uint32(player->GetLevel()), ZoneName(player->GetZoneId()), player->GetPositionX(),
            player->GetPositionY(), player->GetMapId(), ToToken(bot.order.type));
        // A character that enters the world dead (it died before a restart or a logout) died then, not now: no second
        // death for the log, the counters and the risk model.
        if (!player->IsAlive())
        {
            rt.dead = true;
            LOG_INFO(ActivityLog, "activity: {} entered the world dead (its death was counted when it died)", bot.name);
        }
        // What it learned from its fights before the restart (RiskModel.h).
        Risk::RestoreFightStats(rt, guid);
        // In the middle of a camp (it was there when the server stopped): out of it first (OrderService::StepRest).
        constexpr float LoginCampYards = 30.0f;
        // (Also when they are on it already: creatures notice a character in the moment it is added to the map, before
        // the planner's first step. A paladin saved at 44 % health among Infected Mossflayers was in a fight against three
        // a second after a restart, found no open way and died against five, round 152. That fight is ended here.)
        if (player->IsAlive())
            if (uint32 const hostiles = Escape::HostilesAround(player, LoginCampYards))
            {
                if (player->IsInCombat())
                {
                    if (Pet* pet = player->GetPet())
                        pet->CombatStop(true);
                    player->CombatStop(true);
                }
                rt.loginRetreat = true;
                // It did not walk in among them: they respawned around it with the restart, or while it was offline
                // (a level-18 warlock entered the world on a murloc shore after a restart, was attacked within
                // seconds and died on the run twelve seconds later). For a few seconds creatures do not see it, as
                // they do not see a player whose client still loads: time to walk out. The player's own event ends
                // it, whoever plays the character by then.
                constexpr uint32 LoginGraceMs = 12 * IN_MILLISECONDS;
                player->SetImmuneToNPC(true);
                player->m_Events.AddEventAtOffset([player] { player->SetImmuneToNPC(false); }, Milliseconds(LoginGraceMs));
                LOG_INFO(ActivityLog, "activity: {} entered the world with {} hostile creature{} within {:.0f} yd: it looks for a quiet "
                    "place first (creatures do not see it for {} s)", bot.name, hostiles, hostiles == 1 ? "" : "s", LoginCampYards,
                    LoginGraceMs / IN_MILLISECONDS);
                // Those seconds are for walking out, and for nothing else: a level-36 paladin found no place to rest
                // inside Stromgarde, its rest ended at once, its objective task began in the same second, and it
                // walked 50 yards to its target while nobody saw it; when they did it stood on a Syndicate Conjuror
                // with five around it. It backs away from what stands near at once (the legs of a keep-away), and
                // no task starts before the time is over (below).
                rt.loginGraceUntil = std::max<uint32>(getMSTime() + LoginGraceMs, 1);
                Escape::BackOut(player, rt, "it entered the world in");
            }
        // Its rotation follows its talents, not what playerbots stored for it long ago (BotDriver::SyncStrategies).
        if (std::string const changed = BotDriver::SyncStrategies(player); !changed.empty())
            LOG_INFO(ActivityLog, "activity: {} plays by its talents again: combat strategies {}", bot.name, changed);
        // The places it died at before a restart stay set aside (RiskModel.h).
        if (uint32 const places = Risk::RestorePlaces(rt, guid))
            LOG_INFO(ActivityLog, "activity: {} still keeps away from {} place{} where it died", bot.name, places, places == 1 ? "" : "s");
        // What it waited with before a restart or a logout (ga_bot_wait).
        if (uint32 const waits = RestoreBotWaits(guid, rt))
        {
            uint32 const sinceLanding = rt.CrossedLately() ? getMSTimeDiff(rt.lastCrossingAt, getMSTime()) / (MINUTE * IN_MILLISECONDS) : 0;
            LOG_INFO(ActivityLog, "activity: {} still waits with {} thing{} from before it entered the world: {}{} zone{}, {} "
                "hand-in{} given up", bot.name, waits, waits == 1 ? "" : "s",
                rt.CrossedLately() ? Acore::StringFormat("it went ashore {} min ago and stays on its side of the sea, ", sinceLanding) :
                    std::string(), rt.unreachableZones.size(), rt.unreachableZones.size() == 1 ? " waits" : "s wait", rt.errandSkip.size(),
                rt.errandSkip.size() == 1 ? "" : "s");
        }
    }
    rt.guid = guid;
    SaveBotWaits(guid, rt);

    // A bot that crafts from the guild storage stands still until its queue ends (CraftService); the plan resumes from
    // where it is afterwards.
    // (Standing there is no standstill for the rescue: its clock starts anew. UpdateBot, which holds that clock while
    // nothing walks, is not called meanwhile, and a smith that had crafted for five minutes at the forge of Light's Hope
    // was "rescued" for five minutes without moving one second after its next walk began, round 145.)
    if (sCraftService.IsCrafting(guid))
    {
        Sample(bot, player, rt);
        TrackActivity(player, rt, true);
        ClearTask(rt, 0, "crafting from the guild storage");
        rt.why = sCraftService.Describe(guid);
        rt.hasLastPosition = false;
        rt.strandSince = 0;
        return;
    }
    // Likewise one that puts enchantments on its own gear for its skill (Professions::EnchantForSkill), writes one on a
    // vellum for the guild or puts a scroll of the guild storage on its gear.
    if (sProfessions.StepEnchanting(player))
    {
        Sample(bot, player, rt);
        TrackActivity(player, rt, true);
        ClearTask(rt, 0, "enchanting");
        rt.why = sProfessions.DescribeEnchanting(guid);
        rt.hasLastPosition = false;
        rt.strandSince = 0;
        return;
    }

    UpdateBot(guild, bot, player, rt);
}

void OrderService::UpdateBot(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    Sample(bot, player, rt);
    TrackActivity(player, rt, false);
    TrackBreath(player, rt);
    // The piece of gear its crafting queue made for the guild (the queue has ended: UpdateGuildBot skips a crafting bot).
    if (rt.gearCraftItem)
        FinishGearCraft(bot, player, rt);
    // Squads (OrderSquads.cpp): whom it follows, whom it waits for; Reconcile takes the master and party from it.
    UpdateSquad(guild, bot, player, rt);
    Reconcile(bot, player, rt);

    // Activity log: whom the bot fights (named on its death line) and a status line every few minutes.
    if (player->IsInCombat())
    {
        Unit* foe = player->GetVictim();
        if (!foe && !player->getAttackers().empty())
            foe = *player->getAttackers().begin();
        if (Creature* creature = foe ? foe->ToCreature() : nullptr)
        {
            rt.lastFoeEntry = creature->GetEntry();
            rt.lastFoeLevel = creature->GetLevel();
            rt.lastFoeAt = getMSTime();
        }
    }
    TrackFight(player, rt);
    StopLeftoverAutoShot(player, rt);
    LeaveTravelForm(player, rt);
    StepSelfBuff(player, rt);
    StepSelfHeal(player, rt);
    StepPotion(player, rt);
    Escape::StepDefend(player, rt);
    if (!Escape::StepBack(player, rt))
        StepWand(player, rt);
    // A level-up brought a form or the first talent point: the rotation follows (a feral druid is a bear until it has
    // Cat Form).
    if (rt.syncStrategies && !player->IsInCombat())
    {
        rt.syncStrategies = false;
        if (std::string const changed = BotDriver::SyncStrategies(player); !changed.empty())
            LOG_INFO(ActivityLog, "activity: {} plays by its talents again: combat strategies {}", bot.name, changed);
    }
    if (getMSTime() >= rt.nextStatus)
        LogStatus(bot, player, rt);

    // A pull the bot cannot afford (FightEscape.h CheckPull): it lets go of the target; a pack's place is set aside
    // and the task ends, so the planner takes something else. Not for a bot that follows (its fights are its
    // leader's). The check itself runs from Update, more often than this tick.
    bool const inDungeon = player->GetMap()->IsDungeon();
    rt.pullCheckOn = bot.order.type != OrderType::Follow && !rt.squad.following && !inDungeon;
    // A dungeon group's fight rules (OrderDungeons.cpp), stepped by the bot that leads the run.
    if (inDungeon && bot.order.type == OrderType::Dungeon)
        StepDungeonFight(guild, bot, player, rt);

    // Leaving a fight the bot would lose (FightEscape.h): nothing else happens while it runs.
    // (Not in a dungeon: its creatures are elites and stand in groups, and a group that came for them fights them.)
    Escape::Result const escaped = inDungeon ? Escape::Result::None :
        Escape::Step(player, rt, bot.order.type == OrderType::Follow || (rt.squad.following && rt.squad.besideLeader));
    switch (escaped)
    {
        case Escape::Result::Running:
            // Attacked while it rested: the rest is over (its log line said "ended by: died" for a bot that died running).
            if (rt.rest.active)
                EndRest(player, rt, "a fight");
            rt.why = "Running from a fight it would lose";
            PauseStuck(rt);
            return;
        case Escape::Result::Aside:
            if (rt.rest.active)
                EndRest(player, rt, "moved on");
            // Time spent keeping away does not count against the task's stuck clocks, as time spent fighting or resting
            // does not: a level-42 priest kept away from Brimgore, a rare elite on his round in the Wyrmbog, twice for
            // 82 and 102 s on its way to Firemane Scalebanes, and its objective ended "objective spot not reached" after
            // 210 s, 2026-11-26.
            PauseStuck(rt);
            // (The task's own line comes back when the task goes on: after a walk around a pack the status read
            // "Keeping away from creatures it cannot take together" for as long as the task ran, 268 s in one case.)
            if (rt.whyBeforeAside.empty() && rt.why.rfind("Keeping away", 0) != 0 && rt.why.rfind("Getting out of a camp", 0) != 0)
                rt.whyBeforeAside = rt.why;
            rt.why = rt.escape.leaving ? "Getting out of a camp it cannot take" : (rt.escape.pack ? "Keeping away from creatures it cannot "
                "take together" : "Keeping away from a creature far above its level");
            return;
        case Escape::Result::GotAway:
            // Not straight back into it: the place is set aside. The task ends and NewRpg forgets where it was going
            // (it would walk on in a straight line from wherever the run ended), so the next walk is planned anew.
            // (A voyage goes on: the bot stays with its ship.)
            if (rt.task.kind != TaskKind::None && rt.task.kind != TaskKind::Voyage)
            {
                // The same target fled from twice within half an hour: it is set aside like an objective (15 minutes,
                // twice as long each further time), so the planner takes something else instead of trying it at once.
                constexpr uint32 FledWindowMs = 30 * MINUTE * IN_MILLISECONDS;
                constexpr uint32 FledAsideMs = 15 * MINUTE * IN_MILLISECONDS;
                constexpr uint32 FledAsideMaxMs = 2 * HOUR * IN_MILLISECONDS;
                BotTask const& task = rt.task;
                if (task.hasSpot && task.kind != TaskKind::Travel && task.kind != TaskKind::Grind)
                {
                    uint32 const now = getMSTime();
                    // An objective counts by its quest (its foes stand at many spawns), anything else by its NPC or spot.
                    bool const byQuest = task.kind == TaskKind::Objective && task.questId;
                    uint64 const key = !byQuest && task.spot.spawnId ? ((uint64(task.spot.spawnId) << 1) | uint64(task.spot.gameObject ? 1 : 0)) :
                        ((uint64(1) << 63) | (uint64(task.questId) << 8) | uint64(task.kind));
                    BotRuntime::FledTarget& fled = rt.fledTargets[key];
                    if (fled.count && getMSTimeDiff(fled.at, now) > FledWindowMs)
                        fled.count = 0;
                    fled.at = now;
                    // A close call counts at once: a level-23 warrior got away from a named level-24 foe with 13 % of
                    // its health, rested, walked back and died.
                    constexpr float CloseCallHealthPct = 35.0f;
                    if (++fled.count >= 2 || player->GetHealthPct() < CloseCallHealthPct)
                    {
                        uint32 const asideMs = std::min<uint32>(FledAsideMaxMs, FledAsideMs << std::min<uint32>(fled.count >= 2 ? fled.count - 2 : 0, 3));
                        if (byQuest)
                        {
                            // Like the death rule: until the time is over or the bot is two levels on.
                            BotRuntime::Danger& danger = rt.dangerousQuests[task.questId];
                            danger.level = uint8(player->GetLevel() + 1);
                            danger.until = now + asideMs;
                        }
                        else if (task.spot.spawnId)
                            rt.Avoid(task.spot, asideMs);
                        else
                            rt.AddAvoidArea(task.spot.mapId, task.spot.x, task.spot.y, player->GetLevel(), asideMs, false);
                        LOG_INFO("module.ga.activity", "activity: {} sets {} (target {}{}) aside for {} min: it left a fight there or on the way {} times",
                            bot.name, ToString(task.kind), task.targetName.empty() ? "-" : task.targetName,
                            byQuest ? Acore::StringFormat(", quest {}", task.questId) : std::string(), asideMs / (MINUTE * IN_MILLISECONDS),
                            uint32(fled.count));
                    }
                }
                ClearTask(rt, 0, "left a fight");
            }
            BotDriver::Rest(player);
            break;
        case Escape::Result::WentAround:
            // Around a pack on its way, or out of a camp it had no business in: the task goes on from where the bot
            // stands now (that is no jump the planner did not cause: 146 yards out of the Kurzen compound ended the
            // travel that had just begun, "jumped", and began it again).
            rt.hasLastPosition = false;
            if (!rt.whyBeforeAside.empty() && rt.task.kind != TaskKind::None)
                rt.why = rt.whyBeforeAside;
            rt.whyBeforeAside.clear();
            break;
        case Escape::Result::Clear:
        case Escape::Result::ClearOfPack:
        {
            rt.whyBeforeAside.clear();
            // A pack, or a roaming creature far above the bot, on the way to the task's target: the task ends. The
            // second time within ten minutes its target is set aside for a quarter of an hour, so the bot does not
            // go at the same pack or past the same rare again and again (the place is set aside too, but a straight
            // walk of a few hundred yards does not go around it).
            bool const pack = escaped == Escape::Result::ClearOfPack;
            // The hand-in of a finished quest is the most valuable walk a bot has: it waits where it is for what stands
            // on its way to move on (a pair of raptors wanders) and tries again, four times before the ender is set
            // aside, and then for five minutes. (A level-26 mage's only hand-in was set aside for a quarter of an hour
            // after two tries 14 s apart, and the bot spent the time at spawns that had no target: live round 52.)
            bool const handIn = rt.task.kind == TaskKind::TurnIn;
            uint32 waitMs = 0;
            // A task kept away from one strong creature on its way (Stitches on the road to Darkshire): it waits for the
            // creature to pass and goes on. Ended and begun again each time, a level-30 druid's travels out of
            // Darkshire were "begun four times without getting nearer" within a minute, twice, two zones were written
            // off for half an hour and it set out across the sea; its voyage's walk then ended "stuck". Every task, not
            // only journeys: a level-43 paladin's quest task at a Tanaris water hole ended each time it kept away from a
            // rare elite there, and the zone was written off (the maintainers' ruling: one creature on its way does not
            // end the task). Not for one that keeps to its place and stands where the task leads (asideStood): it does
            // not pass, the target counts as one the bot cannot get to.
            bool const stood = rt.asideStood;
            rt.asideStood = false;
            // A pack on its round waits like one creature on its way: it passes. (Northwatch Hold: six marines walk the
            // yard; a level-23 tauren's walk out ended each time they came, the zone was written off and it went home by
            // hearthstone.)
            bool const walkingPack = pack && rt.packWalked;
            rt.packWalked = false;
            if ((!pack || walkingPack) && !stood && rt.task.kind != TaskKind::None && !player->GetTransport() &&
                !sRoadMap.IsNearTransportNode(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), 250.0f))
            {
                std::string who;
                Escape::RoamerNear(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetLevel(), &who);
                if (who.empty())
                    if (Creature const* seen = ObjectAccessor::GetCreature(*player, rt.threatSeen))
                        who = Acore::StringFormat("{} (level {})", seen->GetName(), uint32(seen->GetLevel()));
                if (PauseForRoamer(player, rt, who))
                {
                    BotDriver::Rest(player);
                    break;
                }
            }
            // A voyage's walk to its dock, on land and away from the dock, too: it was let walk on, and a level-54 priest's
            // walk from Felwood through Timbermaw Hold kept away from the furbolgs' camp every seven seconds for 393 s
            // until the stuck rule ended it, and the next voyage took the same route (2026-11-27). On board or at the
            // dock the voyage waits for its ship whatever stands about.
            bool const voyageWalk = rt.task.kind == TaskKind::Voyage && !player->GetTransport() &&
                !sRoadMap.IsNearTransportNode(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), 250.0f);
            if (rt.task.kind != TaskKind::None && (rt.task.kind != TaskKind::Voyage || voyageWalk))
            {
                constexpr uint32 PackWindowMs = 10 * MINUTE * IN_MILLISECONDS;
                uint32 const PackAsideMs = (handIn ? 5 : 15) * MINUTE * IN_MILLISECONDS;
                uint32 const packTries = handIn ? 4 : 2;
                constexpr uint32 HandInWaitMs = 25 * IN_MILLISECONDS;
                BotTask const& task = rt.task;
                // (A walk of a gather round, a hunt or a fishing trip counts with its place: a warrior rode fifteen
                // times in twenty-two minutes for one Mithril Deposit beside the Bloodsail camp and kept away from the
                // camp each time. Its route led into the camp; the straight line to the vein, which the round's look
                // at set-aside places follows, did not.)
                if (task.hasSpot && task.kind != TaskKind::Voyage && (task.kind != TaskKind::Travel || task.gatherRound))
                {
                    uint32 const now = getMSTime();
                    uint64 const key = (uint64(1) << 61) | (task.spot.spawnId ? ((uint64(task.spot.spawnId) << 1) | uint64(task.spot.gameObject ? 1 : 0)) :
                        ((uint64(task.questId) << 8) | uint64(task.kind)));
                    BotRuntime::FledTarget& kept = rt.fledTargets[key];
                    if (kept.count && getMSTimeDiff(kept.at, now) > PackWindowMs)
                        kept.count = 0;
                    kept.at = now;
                    // (A grind target at once: there are others, and the planner offered four of them behind the same
                    // pack within half a minute.)
                    if (handIn)
                        waitMs = HandInWaitMs;
                    // A giver or an ender that has creatures of its own around it, and the pack stood on the last
                    // stretch: the place is guarded, and waiting 25 seconds does not change that. It is set aside for
                    // two hours or until the bot has a level more, across restarts (Risk::SetPlaceAside). A priest
                    // rode 355 seconds to an ender inside Thunder Axe Fortress and 229 seconds to a chest among the
                    // naga, turned round at the gate both times, and would have set out again after half an hour.
                    constexpr float GuardedWithinYards = 250.0f;
                    constexpr uint32 GuardedAsideMs = 2 * HOUR * IN_MILLISECONDS;
                    if ((task.kind == TaskKind::TurnIn || task.kind == TaskKind::Pickup) && pack && task.spot.mapId == player->GetMapId() &&
                        player->GetDistance2d(task.spot.x, task.spot.y) <= GuardedWithinYards &&
                        Risk::GuardsNear(player, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z, 40.0f))
                    {
                        waitMs = 0;
                        kept.count = 0;
                        Risk::SetPlaceAside(player, rt, task.spot.mapId, task.spot.x, task.spot.y, GuardedAsideMs);
                        LOG_INFO("module.ga.activity", "activity: {} sets the place of {} (target {}) aside until level {} or for {} min: "
                            "a pack it cannot take stood on the last {:.0f} yd of its way there, and creatures have their place around it",
                            bot.name, ToString(task.kind), task.targetName.empty() ? "-" : task.targetName, uint32(player->GetLevel()) + 1,
                            GuardedAsideMs / (MINUTE * IN_MILLISECONDS), player->GetDistance2d(task.spot.x, task.spot.y));
                    }
                    // (A creature that keeps to its place and whose reach the way leads into: at once, it will not leave.)
                    else if (stood || ++kept.count >= packTries || task.kind == TaskKind::Grind || task.gatherRound)
                    {
                        waitMs = 0;
                        // A walk to a place to gather at that a pack ended counts with the refused ones: the next place of
                        // the round lies by the same camp more often than not (a rogue's Herbalism catch-up on the
                        // Southern Savage Coast set out for eleven herbs by the Bloodsail compound in ten minutes, each
                        // walk ended by a pack within 5 to 125 s, until it found nothing at all, 2026-11-27). Three in a
                        // row pause the gathering (a place visited counts them again from nought).
                        if (task.gatherRound)
                            ++rt.gatherRefusals;
                        if (task.spot.spawnId)
                            rt.Avoid(task.spot, PackAsideMs);
                        else
                            rt.AddAvoidArea(task.spot.mapId, task.spot.x, task.spot.y, player->GetLevel(), PackAsideMs, false);
                        if (stood)
                            LOG_INFO("module.ga.activity", "activity: {} sets {} (target {}) aside for {} min: a creature far above "
                                "its level keeps to its place on its way there", bot.name, ToString(task.kind),
                                task.targetName.empty() ? "-" : task.targetName, PackAsideMs / (MINUTE * IN_MILLISECONDS));
                        else
                            LOG_INFO("module.ga.activity", "activity: {} sets {} (target {}) aside for {} min: {} stood on its "
                                "way there {} times", bot.name, ToString(task.kind), task.targetName.empty() ? "-" : task.targetName,
                                PackAsideMs / (MINUTE * IN_MILLISECONDS), pack ? "a pack it cannot take" : "a creature far above its level",
                                uint32(kept.count));
                    }
                }
                // An errand's travel that a pack stops twice within the window: the hand-in is given up for a while (a
                // warlock set out for Bookie Herod's Strongbox at the Kurzen camp five times in two minutes).
                if ((task.kind == TaskKind::Travel || task.kind == TaskKind::Voyage) && task.errand && rt.errandQuest)
                {
                    uint32 const now = getMSTime();
                    BotRuntime::FledTarget& kept = rt.fledTargets[(uint64(2) << 61) | uint64(rt.errandQuest)];
                    if (kept.count && getMSTimeDiff(kept.at, now) > PackWindowMs)
                        kept.count = 0;
                    kept.at = now;
                    if (++kept.count >= 2)
                    {
                        kept.count = 0;
                        DropErrand(bot, rt, pack ? "a pack it cannot take stood on its way there twice" :
                            "a creature far above its level stood on its way there twice");
                    }
                }
                // On a route, before its first node: what stood on the way stands between the bot and that node (a
                // level-27 rogue's route out of Redridge began at the node "Stonewatch Keep", inside the Blackrock
                // camp; twice it kept away from the sentries, the third time the same route killed it). The second
                // time the node is no start of its routes for half an hour.
                if (task.route.found && !task.route.points.empty())
                {
                    RoutePoint const& first = task.route.points.front();
                    if (Dist2d(player, first.x, first.y) > 25.0f && Dist2d(player, task.routeStartX, task.routeStartY) < 300.0f &&
                        rt.NoteRefusedStart(first.x, first.y, getMSTime()))
                        LOG_INFO("module.ga.activity", "activity: {} does not begin its routes at the road node at ({:.0f}, {:.0f}) for "
                            "{} min: twice {} stood between it and the node", bot.name, first.x, first.y,
                            RefusedStartMs / (MINUTE * IN_MILLISECONDS), pack ? "a pack it cannot take" : "a creature far above its level");
                }
                ClearTask(rt, waitMs, pack ? "a pack on its way" : "kept away from a creature far above its level");
                if (waitMs)
                    rt.why = "Waiting for the way to its hand-in to clear";
            }
            BotDriver::Rest(player);
            break;
        }
        default:
            break;
    }
    // A careful pull (FightEscape.h): the bot brings one creature out of its group; nothing else moves it meanwhile.
    if (rt.pull.active)
    {
        if (rt.rest.active)
            EndRest(player, rt, "a fight");
        rt.why = "Pulling a creature out of its group";
        return;
    }
    // A squad member runs with its leader when the leader runs.
    if (StepSquadFlee(player, rt))
        return;

    uint32 const zone = player->GetZoneId();
    if (!rt.lastZone)
        rt.lastZone = zone;
    if (!rt.lastLevel)
        rt.lastLevel = player->GetLevel();

    // A jump the planner did not cause (a GM teleport, playerbots' stuck recovery): the plan belongs to the old place,
    // so plan again. Voyages jump on purpose (ships change maps, boarding hops, portals). A death is no jump: releasing
    // the spirit puts the ghost at the graveyard, and the bot comes back to life where its ghost stands (at the corpse
    // or at a spirit healer), so the task survives the death and its deaths can be counted.
    //
    // A flight the planner booked is no jump either. With instant flight paths (worldserver InstantFlightPaths) the
    // core puts the character at the last node at once: it is never "in flight", and NewRpg drops its TRAVEL_FLIGHT
    // status on the spot (the taxi call reports "no flight started"). Once the bot has stood at the flight master of its
    // flight, "on the ground and far from it" means landed, whatever NewRpg's status says: the task walks on from the
    // landing spot. A flight NewRpg gave up before the bot reached the flight master is simply forgotten.
    bool landed = false;
    if (rt.flightBooked)
    {
        bool const sameMap = player->GetMapId() == rt.flightMap;
        float const fromMaster = sameMap ? Dist2d(player, rt.flightX, rt.flightY) : 0.0f;
        bool const farAway = !sameMap || fromMaster > JumpDistance;
        bool const inTheAir = player->IsInFlight() || player->IsBeingTeleported();
        bool const flightStatus = BotDriver::GetRpgState(player) == RpgState::TravelFlight;
        if (sameMap && fromMaster <= FlightMasterReach)
            rt.flightAtMaster = true;
        if (rt.flightAtMaster && farAway && !inTheAir && player->IsAlive())
        {
            rt.flightBooked = false;
            landed = true;
            // Whatever NewRpg was sent to before the teleport belongs to the place it left: a walk from the flight
            // master of Bloodvenom Post that went on after the teleport took Hordeboss 77 yd from the master of
            // Orgrimmar into the rock under the city, where it fell for seven minutes until a rescue (2026-11-26).
            BotDriver::Rest(player);
            rt.task.lastAssert = 0;
            // The walk on from the landing is a new walk: its way is asked again from here (a route planned before the
            // flight, or a short walk's path asked at the flight master, says nothing about the landing).
            rt.task.routePlanned = false;
            rt.task.route.found = false;
            rt.task.nextRoutePlan = 0;
            rt.task.shortProbed = false;
            LOG_INFO(ActivityLog, "activity: {} landed in {} at ({:.0f}, {:.0f}) map {} after its flight", bot.name,
                ZoneName(player->GetZoneId()), player->GetPositionX(), player->GetPositionY(), player->GetMapId());
        }
        else if (!flightStatus && !inTheAir && (!rt.flightAtMaster || !farAway) &&
            getMSTimeDiff(rt.flightBookedAt, getMSTime()) > FlightTeleportWaitMs)
            rt.flightBooked = false;
    }
    if (rt.hasLastPosition && player->IsAlive() && !player->IsBeingTeleported())
    {
        float const dx = player->GetPositionX() - rt.lastX;
        float const dy = player->GetPositionY() - rt.lastY;
        bool const jumped = player->GetMapId() != rt.lastMap || dx * dx + dy * dy > JumpDistance * JumpDistance;
        if (jumped && !landed && rt.task.kind != TaskKind::Voyage && !player->IsInFlight())
        {
            ++rt.jumps;
            LOG_INFO("module", "mod-guild-adventure: {} jumped from map {} ({:.0f}, {:.0f}) to map {} ({:.0f}, {:.0f}) during task {}; "
                "planning again", bot.name, rt.lastMap, rt.lastX, rt.lastY, player->GetMapId(), player->GetPositionX(),
                player->GetPositionY(), ToString(rt.task.kind));
            ClearTask(rt, 0, "jumped");
            rt.zoneChosenAt = 0;
            rt.waitUntil = 0;
        }
    }
    if (!player->IsBeingTeleported())
    {
        rt.hasLastPosition = true;
        rt.lastMap = player->GetMapId();
        rt.lastX = player->GetPositionX();
        rt.lastY = player->GetPositionY();
    }

    // Nothing to plan while the bot is busy with something only it can finish.
    if (player->IsAlive())
        rt.seenAlive = true;
    if (!player->IsAlive())
    {
        if (!rt.dead)
        {
            rt.dead = true;
            if (rt.rest.active)
                EndRest(player, rt, "died");
            rt.whyBeforeDeath = rt.why;
            // Dead when it entered the world (it was a ghost when the server stopped): no new death.
            if (rt.seenAlive)
                OnDied(guild, bot, player, rt);
            else
                LOG_INFO(ActivityLog, "activity: {} entered the world dead at ({:.0f}, {:.0f}) map {}", bot.name, player->GetPositionX(),
                    player->GetPositionY(), player->GetMapId());
        }
        rt.why = player->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? "Dead: running back to the corpse" : "Dead";
        // A resurrection somebody offers it (a druid's Rebirth in a fight, a priest's or a paladin's spell after it) is
        // accepted at once, as a player clicks Accept. mod-playerbots accepts it in its AI's update, and the AI of a dead
        // character of a dungeon run is held so that it keeps its spirit with its body: the dead of the owner's raid in
        // Zul'Gurub let the druids' Rebirths go by (2026-11-26, the owner: [quote left out]).
        if (player->isResurrectRequested() && !player->HasPreventResurectionAura() && !player->IsBeingTeleported() &&
            sGuildBotHolder.IsAIPlayed(player->GetGUID()))
        {
            Player* by = nullptr;
            if (Group* party = player->GetGroup())
                for (GroupReference* ref = party->GetFirstMember(); ref; ref = ref->next())
                    if (Player* member = ref->GetSource(); member && player->isResurrectRequestedBy(member->GetGUID()))
                        by = member;
            LOG_INFO(ActivityLog, "activity: {} accepts the resurrection {} offers it{}", bot.name, by ? by->GetName() : "somebody",
                by && by->IsInCombat() ? " in a fight" : "");
            player->ResurectUsingRequestData();
            rt.dungeon.deadSince = 0;
            rt.dungeon.revivedAt = std::max<uint32>(getMSTime(), 1);
            rt.hasLastPosition = false;
            PauseStuck(rt);
            return;
        }
        // In a dungeon run the group brings its dead back (OrderDungeons.cpp): no walk from a graveyard.
        if (StepDungeonDead(guild, bot, player, rt))
        {
            PauseStuck(rt);
            return;
        }
        // A death loop far above its level (RiskModel): the bot gives the place up and goes home.
        if (rt.rescue && Risk::Rescue(player, rt))
        {
            rt.spiritRes = false;
            LOG_INFO(ActivityLog, "activity: {} was taken home from a place that keeps killing it or that it cannot stand in (the "
                "spirit healer's price, then its hearthstone)", bot.name);
        }
        else if (rt.spiritRes && player->HasPlayerFlag(PLAYER_FLAGS_GHOST))
            StepSpiritRes(bot, player, rt);
        if (rt.task.kind != TaskKind::None)
        {
            rt.task.startedAt += rt.stepGapMs;
            if (rt.task.nearSince)
                rt.task.nearSince += rt.stepGapMs;
        }
        PauseStuck(rt);
        return;
    }
    rt.dungeon.deadSince = 0;
    if (rt.dead)
    {
        // Alive again: the task (if it survived the death) carries on, and so does its why line.
        rt.dead = false;
        rt.why = std::move(rt.whyBeforeDeath);
        rt.whyBeforeDeath.clear();
    }
    // On a ship or zeppelin without a voyage (the order changed during the crossing, the owner played the bot on board):
    // ride to the next dock and go ashore there. A following bot stays with whoever it follows.
    if (Transport* transport = player->GetTransport(); transport && rt.task.kind != TaskKind::Voyage &&
        !player->IsBeingTeleported() && bot.order.type != OrderType::Follow && !rt.squad.following)
        if (MotionTransport* ship = transport->ToMotionTransport())
            StartShoreLeave(bot, player, rt, ship);

    // The profession plan: what its trainers would teach the bot now is learned where it stands.
    if (uint32 const clock = getMSTime(); player->IsAlive() && sBotSettings.IsOn(player, Setting::ProfessionsLearnInPlace) && clock >= rt.nextProfessionCheck)
    {
        rt.nextProfessionCheck = clock + ProfessionCheckMs;
        sProfessions.LearnPlanned(bot, player);
    }

    SupplyQuestItems(guild, bot, player, rt);

    // Air. playerbots does not watch a diving bot's breath (three minutes, then it drowns): it comes up in time.
    if (player->IsAlive() && !player->GetTransport() && CheckBreath(player, rt))
    {
        rt.lastZone = zone;
        return;
    }

    // On a lift of its route: the ride goes on until the other stop (OrderService::StepLift; the planner does nothing
    // else meanwhile).
    if (rt.task.liftAt && rt.task.liftRiding && player->GetTransport() && !player->IsBeingTeleported() && StepLift(player, rt))
    {
        rt.lastZone = zone;
        return;
    }
    // On a lift without a ride of its own (StepStrayLiftPassenger): off at the platform's next stop.
    if (Transport* transport = player->GetTransport())
    {
        if (transport->IsStaticTransport() && !(rt.task.liftAt && rt.task.liftRiding) && StepStrayLiftPassenger(player, rt, transport))
        {
            rt.lastZone = zone;
            return;
        }
    }
    else
        rt.strayLiftSince = 0;
    // On a ship the voyage task keeps running (it waits for the other dock); anything else on a transport, a taxi or
    // a teleport is waited out.
    bool const onVoyage = rt.task.kind == TaskKind::Voyage && player->GetTransport() && !player->IsBeingTeleported() &&
        !player->IsInCombat();
    if (!onVoyage && (player->IsInCombat() || player->IsInFlight() || player->IsBeingTeleported() || player->GetTransport()))
    {
        if (rt.rest.active)
            EndRest(player, rt, player->IsInCombat() ? "a fight" : "moved on");
        if (player->IsInFlight() && (rt.task.kind == TaskKind::Travel || rt.task.kind == TaskKind::Voyage))
            rt.why = Acore::StringFormat("Travelling to {}: flying", rt.task.targetName);
        // In a fight the planner does not plan, but a bot whose every step towards its enemy is cancelled by the ground
        // guard (the enemy shoots from a level below) must not stand there: it gets the way out.
        if (player->IsInCombat() && !player->GetTransport() && HopDownToFight(player, rt))
            ; // put beside what it fights in a dungeon
        else if (player->IsInCombat() && !player->GetTransport() &&
            sGroundGuard.GetRefusals(player->GetGUID(), 45 * IN_MILLISECONDS, false).count >= 3)
        {
            sGroundGuard.GetRefusals(player->GetGUID(), 45 * IN_MILLISECONDS, true);
            if (!GroundWayOut(player, rt, "fights something it cannot get to"))
                LeaveUnreachableFoe(player, rt); // it leaves what it cannot get to
        }
        // The fight is the interaction of an item for a weakened creature: the item when the creature is low enough.
        if (player->IsInCombat())
            StepWeaken(player, rt);
        // A fight profile under which it does nothing for 10 s goes out (FightProfiles.h, FC9).
        if (player->IsInCombat())
            FightProfiles::WatchFight(player);
        // Time spent fighting does not count against the task's time limits.
        if (rt.task.kind != TaskKind::None)
        {
            rt.task.startedAt += rt.stepGapMs;
            if (rt.task.nearSince)
                rt.task.nearSince += rt.stepGapMs;
        }
        // A fight came of the grind's targets (the grind step does not run during it, so it cannot see the fight
        // itself: four fights at a spot of Living Decay and the spot was "empty, no fight came of its targets" after 77 s,
        // round 145).
        rt.task.grindPickSince = 0;
        PauseStuck(rt);
        rt.strandSince = 0;
        rt.lastZone = zone;
        return;
    }

    if (CheckStranded(bot, player, rt))
    {
        rt.lastZone = zone;
        return;
    }

    // The dungeon role (DungeonRoles.h): pieces for its dungeon set go from the bags into its role store, and a
    // dungeon set that is on without a run comes off again.
    sRoleService.Step(guild, bot, player, IsOnDungeonRun(guild, bot));

    // A death knight recruit placed in the capital (DeathKnights.h): its starting campaign counts as done at its first
    // plan, before its talent points are spent (outside Acherus it has those of its level). Roster bots only.
    if (bot.guid != guild.leader && DeathKnights::IsStartPending(player) && player->IsAlive() && !player->IsInCombat())
    {
        bool const withRewards = sBotSettings.GetForGuild(guild.ownerAccount, Setting::DeathKnightsSkipStart) != 2;
        std::string const done = DeathKnights::FinishStart(player, withRewards);
        LOG_INFO(ActivityLog, "activity: {} {}", bot.name, done);
        RecordEvent(guild, "INFO", bot.guid, Acore::StringFormat("{} skipped the death knight starting area and begins in {}.",
            bot.name, player->GetTeamId(true) == TEAM_HORDE ? "Orgrimmar" : "Stormwind"), uint8(player->GetLevel()),
            player->GetZoneId());
        rt.nextSpec = 0;
        rt.nextRuneforge = getMSTime() + ClassAbilitiesCheckMs;
    }

    // Talent points along the bot's template (SpecService): after a login and a level-up, out of combat. The leader
    // spends his own until the owner chose a template for him. Before the class abilities: a hunter's exotic pet asks
    // for the talent.
    if (uint32 const now = getMSTime(); (now >= rt.nextSpec || sSpecService.HasPendingReset(bot.guid)) &&
        (bot.guid != guild.leader || sSpecService.HasAssignment(bot.guid)) &&
        player->IsAlive() && !player->IsInCombat())
    {
        rt.nextSpec = now + ClassAbilitiesCheckMs;
        if (std::string const done = sSpecService.Maintain(player); !done.empty())
            LOG_INFO(ActivityLog, "activity: {} (level {}) {}", bot.name, uint32(player->GetLevel()), done);
    }

    // What the class quests teach (the hunter's pet, stances, forms, totems): on the first plan after a login, after
    // a level-up and every few minutes (a pet or a totem that could not be given at that moment). The leader plays
    // his class quests himself.
    if (uint32 const now = getMSTime(); now >= rt.nextClassAbilities && bot.guid != guild.leader)
    {
        // A hunter that is due its good pet and had none at its side is looked at again sooner.
        rt.nextClassAbilities = now + (ClassAbilities::IsPetPending(player) ? 30 * IN_MILLISECONDS : ClassAbilitiesCheckMs);
        if (std::string const given = ClassAbilities::Grant(player); !given.empty())
            LOG_INFO("module", "mod-guild-adventure: {} (level {}) got what its class quests teach: {}", bot.name,
                uint32(player->GetLevel()), given);
    }

    // A hunter's pet spends its free talent points (ClassAbilities::SpendPetTalents): a look every few seconds covers a
    // new pet, a pet called back, its level-ups and a login. The leader too, while the AI plays him: a pet whose talents
    // he chose himself is left alone.
    if (uint32 const now = getMSTime(); now >= rt.nextPetTalents && player->getClass() == CLASS_HUNTER && !player->IsInCombat())
    {
        rt.nextPetTalents = now + PetTalentsCheckMs;
        if (std::string const spent = ClassAbilities::SpendPetTalents(player); !spent.empty())
            LOG_INFO(ActivityLog, "activity: {} (level {}) {}", bot.name, uint32(player->GetLevel()), spent);
    }

    // A death knight's runes (DeathKnights::Runeforge): a weapon without an enchantment gets the best rune it knows, as
    // at a runeforge. A look every few minutes and right after a login covers a new weapon and the dungeon set.
    if (uint32 const now = getMSTime(); now >= rt.nextRuneforge && player->getClass() == CLASS_DEATH_KNIGHT && !player->IsInCombat())
    {
        rt.nextRuneforge = now + ClassAbilitiesCheckMs;
        if (std::string const forged = DeathKnights::Runeforge(player); !forged.empty())
            LOG_INFO(ActivityLog, "activity: {} (level {}) {}", bot.name, uint32(player->GetLevel()), forged);
    }

    // mod-playerbots' own drink and food actions keep to the leader's thresholds (BotDriver.h). Every tick: an AI
    // that is new (a hand-over from its owner, the end of watch mode) has mod-playerbots' own again, and until the
    // next look at the supplies, up to a minute, it drank below 85 % mana as it used to.
    BotDriver::GuardConsumables(player);
    // Its fight profile (FightProfiles.h): the one of its slot goes in between fights, the group slot on a dungeon or
    // raid map and in a dungeon run; a strategy reset puts the class's rotation strategies back, the profile goes in again.
    FightProfiles::Keep(player, player->GetMap()->IsDungeon() || IsOnDungeonRun(guild, bot));

    // Supplies from the guild storage: food and drink when the bot runs low, equipment that is an upgrade for it.
    if (uint32 const now = getMSTime(); now >= rt.nextSupply)
    {
        rt.nextSupply = now + SupplyCheckMs;
        // Ammunition it must not carry is sold at vendor price: what its weapon does not shoot, and what is over its
        // stock, the worst kind first (Ammunition.h).
        if (std::string const sold = Ammo::Trim(player); !sold.empty())
            LOG_INFO(ActivityLog, "activity: {} sold ammunition at vendor price: {}", bot.name, sold);
        if (std::string const taken = StorageSupply::Step(player); !taken.empty())
            LOG_INFO(ActivityLog, "activity: {} took from the guild storage: {}", bot.name, taken);
        // A scroll of its main stat and elixirs out of the guild storage, while it works on a task with fights (not
        // for a walk, a hand-in, a vendor trip or a farm order's creatures far below it): each piece is a log line.
        if (bot.order.type != OrderType::Farm && (rt.task.kind == TaskKind::Objective || rt.task.kind == TaskKind::Grind ||
            rt.task.kind == TaskKind::Escort || rt.task.kind == TaskKind::Event || rt.squad.following))
            StorageSupply::UseBuffs(player);
        // A rogue's poisons out of the guild storage (RoguePoisons.h; a line per take).
        Poisons::TakeFromStorage(player);
        // A bag for an empty bag slot out of the guild storage.
        if (std::string const bag = TakeBagFromStorage(player); !bag.empty())
            LOG_INFO(ActivityLog, "activity: {} put on a bag out of the guild storage: {}", bot.name, bag);
        // Armour it carries goes on where it wears nothing (GearUpkeep.h).
        if (std::string const worn = Gear::FillEmptySlots(player); !worn.empty())
            LOG_INFO(ActivityLog, "activity: {} put on what it carried where it wore nothing: {}", bot.name, worn);
        // A scroll of an enchantment out of the guild storage on a piece it wears (the owner, 2026-11-25: blue quality or
        // better, item level 60 or more, the best for its role): Professions::UseEnchantScroll, the cast in its queue
        // (StepEnchanting). Where nothing hostile is near, not in a dungeon, not on a ship or a flight.
        if (now >= rt.nextScrollUse && sBotSettings.IsOn(player, Setting::ProfessionsEnchantScrolls) && !player->IsInCombat() &&
            !player->IsInFlight() && !player->GetTransport() && !player->GetMap()->IsDungeon() && !IsOnDungeonRun(guild, bot) &&
            IsQuietToCraft(player))
        {
            rt.nextScrollUse = now + ScrollUseMs;
            if (std::string used; sProfessions.UseEnchantScroll(player, used))
                LOG_INFO(ActivityLog, "activity: {} takes a scroll out of the guild storage for its gear: {}", bot.name, used);
        }
        // Gear far below its level: the owner should know (two minutes after a login, so the storage and a vendor
        // had their turn; then at most every half hour).
        constexpr uint32 GearWarningFirstMs = 2 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 GearWarningGapMs = 30 * MINUTE * IN_MILLISECONDS;
        if (!rt.nextGearWarning)
            rt.nextGearWarning = now + GearWarningFirstMs;
        else if (now >= rt.nextGearWarning)
        {
            rt.nextGearWarning = now + GearWarningGapMs;
            if (std::string const weak = Gear::Warning(player); !weak.empty())
            {
                LOG_INFO(ActivityLog, "activity: {} (level {}) wears gear far below its level: {}", bot.name,
                    uint32(player->GetLevel()), weak);
                RecordEvent(guild, "GEAR", bot.guid, Acore::StringFormat("{}'s gear is far below its level ({}): {}.", bot.name,
                    uint32(player->GetLevel()), weak), player->GetLevel(), player->GetZoneId());
            }
            // The template is the owner's choice (docs/specs.md), so only a hint: a healer or a tank levels slowly alone.
            if (uint8 const role = BotDriver::SoloRole(player); role && !rt.squad.following && player->GetLevel() >= 10 &&
                !rt.dungeon.id && !sRoleService.IsDungeonSetOn(bot.guid))
                LOG_INFO(ActivityLog, "activity: {} (level {}) plays a {} spec: alone it levels slowly (a fight against one foe of "
                    "its level costs it about {:.0f}% of what it has); a damage template or a squad suits it better", bot.name,
                    uint32(player->GetLevel()), role == 2 ? "healing" : "tank", Escape::FoeCost(rt, player->GetLevel(), 0) * 100.0f);
        }
    }

    // Rest between fights: below the start values the bot eats, drinks and waits before anything else goes on.
    // A loot roll of its group does not wait for a bot that rests (its own AI is paused then).
    if (player->GetGroup())
        BotDriver::VoteOnRolls(player);
    if (StepRest(bot, player, rt))
    {
        // Time spent resting does not count against the task's stuck clocks, as time spent fighting does not: a
        // squad member without drink rested 112 s at the flight master of Thunder Bluff and its travel after its
        // leader ended "stuck" five seconds after the rest, before its flight (j5, 2026-11-26); a priest's quest task
        // with six rests in three minutes ended so too (live, 00:53).
        PauseStuck(rt);
        rt.lastZone = zone;
        return;
    }
    // After a login among creatures: until they see it again it goes nowhere on a task (above, at the login).
    if (rt.loginGraceUntil)
    {
        if (getMSTime() < rt.loginGraceUntil && !player->IsInCombat() && player->IsAlive())
        {
            BotDriver::PauseAI(player, 1500);
            rt.why = "Waiting a moment after its login among creatures";
            rt.lastZone = zone;
            return;
        }
        rt.loginGraceUntil = 0;
    }

    // The ground guard keeps cancelling movements towards the task's target (GroundGuard, core patch CP-10): there is
    // no way to it on the ground from here (a mob in the tunnel under the bot's feet, an NPC a floor below). The target
    // is set aside and the planner sends the bot somewhere else. Movements that have nothing to do with the task (NewRpg
    // trying a quest giver it passes) are left to playerbots, which gives such an NPC up by itself after a while.
    constexpr uint32 GroundRefusalLimit = 3;
    constexpr uint32 GroundRefusalWindowMs = 45 * IN_MILLISECONDS;
    constexpr float GroundNpcRadius = 80.0f;
    constexpr float GroundSpotRadius = 200.0f;
    GroundRefusals const refusals = sGroundGuard.GetRefusals(player->GetGUID(), GroundRefusalWindowMs, false);
    // Refused steps towards a creature the bot's AI goes for (not the task's target): the AI chases what it cannot get to.
    // It lets go of the creature, and the refusals do not count against the task's walk. (A warlock stood ten minutes on
    // the shore east of Booty Bay: its AI kept going for a Bloodsail Raider in the water below the ledge, and each of its
    // travels and its voyage counted the refused steps as their own, "finds no way on foot", three zones set aside.)
    if (refusals.count >= 1)
    {
        Unit* aim = BotDriver::IntendedTarget(player);
        if (aim && aim->GetTypeId() == TYPEID_UNIT && aim->GetMapId() == refusals.mapId && aim->GetVictim() != player &&
            aim->GetExactDist(refusals.x, refusals.y, refusals.z) <= 8.0f &&
            !(rt.task.hasSpot && rt.task.spot.mapId == aim->GetMapId() &&
                aim->GetExactDist2d(rt.task.spot.x, rt.task.spot.y) <= 8.0f))
        {
            sGroundGuard.GetRefusals(player->GetGUID(), GroundRefusalWindowMs, true);
            uint32 const letNow = std::max<uint32>(getMSTime(), 1);
            if (rt.letGoOf != aim->GetGUID() || getMSTimeDiff(rt.letGoAt, letNow) > MINUTE * IN_MILLISECONDS)
                LOG_INFO(ActivityLog, "activity: {} lets go of {} (level {}) at ({:.0f}, {:.0f}, {:.0f}): its AI went for it, and no way "
                    "on the ground leads there from ({:.0f}, {:.0f}, {:.0f})", bot.name, aim->GetName(), uint32(aim->GetLevel()),
                    aim->GetPositionX(), aim->GetPositionY(), aim->GetPositionZ(), player->GetPositionX(), player->GetPositionY(),
                    player->GetPositionZ());
            rt.letGoOf = aim->GetGUID();
            rt.letGoAt = letNow;
            BotDriver::DropTarget(player);
            rt.lastZone = zone;
            return;
        }
    }
    // (Not while it swims to its target: its legs are its own, and a swim that gets no nearer is given up by itself.)
    if (refusals.count >= GroundRefusalLimit && !rt.task.swim.empty())
        sGroundGuard.GetRefusals(player->GetGUID(), GroundRefusalWindowMs, true);
    // A target on a level the path finder has no way to: set aside at the first refusal (the guild leader spent 88 s and
    // three refusals on each target on a scaffold of the Searing Gorge's Cauldron, and a restart forgot them all).
    else if (refusals.count >= 1 && rt.task.swim.empty() &&
        SetAsideOtherLevel(guild, bot, player, rt, refusals.mapId, refusals.x, refusals.y))
    {
        sGroundGuard.GetRefusals(player->GetGUID(), GroundRefusalWindowMs, true);
        rt.lastZone = zone;
        return;
    }
    // The first refused step of a straight walk: its way asked again from here, or the planner's own steps at once.
    else if (refusals.count >= 1 && rt.task.swim.empty() &&
        RetryRefusedWalk(bot, player, rt, refusals.solid > GroundPassSolid || refusals.air > GroundPassAir))
    {
        sGroundGuard.GetRefusals(player->GetGUID(), GroundRefusalWindowMs, true);
        rt.lastZone = zone;
        return;
    }
    // A refused step straight onto the task's own spot within sight, with no way at all and no whole path of the path
    // finder there: the spot is set aside at the first refusal. NewRpg walks to a spot this near itself and tries again
    // every half minute, so the three refusals of 45 s below never came: a warlock of level 9 stood five minutes in the
    // Burning Blade Coven under two Razormane Scout spawns that lie in the rock beside the cave, "no progress at this
    // spawn" after three minutes, then the second one, then the rescue (Durnok, 2026-11-27). (Not in the water: the swim.)
    else if (constexpr float OwnSpotYards = 5.0f; refusals.count >= 1 && rt.task.swim.empty() && rt.task.hasSpot &&
        (rt.task.kind == TaskKind::Objective || rt.task.kind == TaskKind::Grind) && !rt.task.ownSteps &&
        rt.task.spot.mapId == refusals.mapId && rt.task.spot.mapId == player->GetMapId() &&
        Dist2d(refusals.x, refusals.y, rt.task.spot.x, rt.task.spot.y) <= OwnSpotYards &&
        Dist2d(player, rt.task.spot.x, rt.task.spot.y) <= SightDistance &&
        (refusals.solid > GroundPassSolid || refusals.air > GroundPassAir) && !IsSwimming(player) && !IsDiving(player) &&
        !WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), rt.task.spot.x,
            rt.task.spot.y, rt.task.spot.z))
    {
        sGroundGuard.GetRefusals(player->GetGUID(), GroundRefusalWindowMs, true);
        LOG_INFO(ActivityLog, "activity: {} cannot get to its spot ({:.0f}, {:.0f}, {:.0f}) {:.0f} yd away on the ground: its step there "
            "runs {:.0f} yd through solid ground and {:.0f} yd through the air, and the path finder has no way; set aside for {} min",
            bot.name, rt.task.spot.x, rt.task.spot.y, rt.task.spot.z, Dist2d(player, rt.task.spot.x, rt.task.spot.y), refusals.solid,
            refusals.air, AvoidSpawnMs / (MINUTE * IN_MILLISECONDS));
        rt.AddAvoidArea(rt.task.spot.mapId, rt.task.spot.x, rt.task.spot.y, 255, AvoidSpawnMs, false);
        rt.Avoid(rt.task.spot, AvoidSpawnMs);
        ClearTask(rt, IN_MILLISECONDS, "no way on the ground");
        BotDriver::Rest(player);
        rt.lastZone = zone;
        return;
    }
    else if (refusals.count >= GroundRefusalLimit)
    {
        sGroundGuard.GetRefusals(player->GetGUID(), GroundRefusalWindowMs, true);
        // The bot itself stands off the navmesh: no target is the problem, and setting them aside one by one would
        // keep it there until the five minutes of "stranded" are over.
        // (Not a bot in the water before its swim was looked for: the water has no navmesh, the swim may lead on.)
        bool const swimFirst = (IsSwimming(player) || IsDiving(player)) && rt.task.hasSpot && !rt.task.swimLooked;
        // (A walk down off a slope or a ledge first: a hunter fought a Living Decay at a Rich Thorium Vein in the Eastern
        // Plaguelands, its chase climbed in short steps 17 yd up the slope beside the vein, and it was rescued, round 179.)
        if (!swimFirst && IsOffNavmesh(player) && (LiftOutOfGround(player, rt, "it had its movements cancelled") ||
            StepOffLedge(player, rt, "it has its movements cancelled and stands off the walkable ground") ||
            Rescue(player, rt, "has its movements cancelled and stands off the walkable ground", 0.0f)))
        {
            rt.lastZone = zone;
            return;
        }
        // A step with no way at all (far through solid ground or the air), and no way to any road either (a hunter
        // dropped into the pool under the Cliffspring falls and had four targets set aside one after the other before
        // the five minutes of "stranded" were over): back to a road at once, whatever the task.
        bool const noWay = (refusals.solid > GroundPassSolid || refusals.air > GroundPassAir) && !player->GetTransport() &&
            rt.task.kind != TaskKind::None && rt.task.kind != TaskKind::Voyage;
        // A step to the task's target itself with no way on the ground: perhaps the water leads there (a ship's deck off
        // the coast): the walk looks for a swim once (StepSwim) before anything is set aside.
        constexpr float SwimArmYards = 30.0f;
        // (And any step with no way of a bot in the water, to a target it does not dive for: the way ashore.)
        if (noWay && rt.task.hasSpot && !rt.task.swimLooked && rt.task.spot.mapId == refusals.mapId &&
            (Dist2d(refusals.x, refusals.y, rt.task.spot.x, rt.task.spot.y) <= SwimArmYards ||
                ((IsSwimming(player) || IsDiving(player)) && !IsAtWaterTarget(player, rt))))
        {
            rt.task.swimArmed = true;
            rt.lastZone = zone;
            return;
        }
        // (Not a bot that dives at its objective: a hunter was "cut off" and put at the graveyard five times in ten
        // minutes at the Giant Clams off Stranglethorn's coast. A target it cannot get to is set aside by the walk.)
        if (noWay && !IsAtWaterTarget(player, rt) && IsCutOff(player) && CutOffWayOut(player, rt, "is cut off from every road"))
        {
            rt.lastZone = zone;
            return;
        }
        // A step of the task's route has no way on the ground (a node the navmesh does not reach): the node goes out of
        // the graph and the task walks on without a route. The guard stays on.
        if (rt.task.route.found)
        {
            // Only a step along the route says something about a node. The walk to its first node fails because of
            // where the bot stands (on a roof, a ledge): that took "Stormwind City" and "Ratchet flightMaster" out.
            std::string const nodes = rt.task.routeIndex > 0 ?
                sRoadMap.BlockNodesNear(refusals.mapId, refusals.x, refusals.y, refusals.z, RouteNodeRadius) : std::string();
            LOG_INFO(ActivityLog, "activity: {} leaves its route at ({:.0f}, {:.0f}, {:.0f}): no way on the ground to its next "
                "point ({:.0f}, {:.0f}, {:.0f}){}", bot.name, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                refusals.x, refusals.y, refusals.z, nodes.empty() ? "" : "; out of the road map: " + nodes);
            rt.task.route.found = false;
            rt.task.routeBlocked = true;
            rt.task.lastAssert = 0;
            rt.lastZone = zone;
            return;
        }
        bool related = false;
        switch (rt.task.kind)
        {
            case TaskKind::Pickup:
            case TaskKind::TurnIn:
            case TaskKind::Vendor:
            case TaskKind::Train:
            case TaskKind::Discover:
            case TaskKind::Event:
                related = rt.task.hasSpot && rt.task.spot.mapId == refusals.mapId &&
                    Dist2d(refusals.x, refusals.y, rt.task.spot.x, rt.task.spot.y) <= GroundNpcRadius;
                break;
            case TaskKind::Objective:
            case TaskKind::Grind:
            case TaskKind::Interact: // (an object 54 yd off and 37 yd of rock between: "gives that up" nine times in 518 s, 2026-11-27)
                // Near the task's spot, or a step that has no way at all (NewRpg walking to a quest area 550 yd off
                // as one straight line): the spot is set aside either way.
                related = rt.task.spot.mapId == refusals.mapId &&
                    (Dist2d(refusals.x, refusals.y, rt.task.spot.x, rt.task.spot.y) <= GroundSpotRadius ||
                     refusals.solid > GroundPassSolid || refusals.air > GroundPassAir);
                break;
            default:
                break;
        }
        // A step with no way while roads can be reached, and it is not the task's own target that has none (that is set
        // aside below; a stepping stone would only bring the bot back to it: thirteen in a row for one bot): the walk
        // cannot start from where the bot stands (the path finder has no long path out of the upper floor of SI:7 in
        // Stormwind, and a normal one down to the street). A stepping stone towards a road, then the task plans anew.
        // A few per bot in ten minutes.
        if (noWay && !related)
        {
            constexpr uint32 StoneWindowMs = 10 * MINUTE * IN_MILLISECONDS;
            constexpr uint8 StonesPerWindow = 4;
            uint32 const stoneNow = std::max<uint32>(getMSTime(), 1);
            if (!rt.stoneWindowAt || getMSTimeDiff(rt.stoneWindowAt, stoneNow) > StoneWindowMs)
            {
                rt.stoneWindowAt = stoneNow;
                rt.stonesInWindow = 0;
            }
            if (rt.stonesInWindow < StonesPerWindow && TakeSteppingStone(player, rt, refusals.x, refusals.y))
            {
                ++rt.stonesInWindow;
                rt.lastZone = zone;
                return;
            }
        }
        // A travel (or the walk to a dock) whose next step has no way on the ground: NewRpg's long-distance walk ran into
        // a dead end (a city wall, a cliff). Until the planner routes over roads the bot takes the way it always took
        // from there, the straight line, without standing two minutes first.
        if ((rt.task.kind == TaskKind::Travel || rt.task.kind == TaskKind::Voyage) && !player->GetTransport())
        {
            // A step that runs far through a hill or the air is no dead end to step over: there is no way on foot from
            // here towards the target. The travel counts as stuck at once (three times and the zone or the errand is
            // set aside), instead of standing two minutes with the ground check "off" that would not let it through.
            if (refusals.solid > GroundPassSolid || refusals.air > GroundPassAir)
            {
                LOG_INFO(ActivityLog, "activity: {} finds no way on foot towards {} from ({:.0f}, {:.0f}, {:.0f}) map {} (its step "
                    "would run {:.0f} yd through solid ground and {:.0f} yd through the air)", bot.name, rt.task.targetName,
                    player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetMapId(), refusals.solid,
                    refusals.air);
                BotDriver::Rest(player);
                // No way to any road either (a ledge it dropped onto, a mountain top a straight walk led it to): no
                // travel will ever start from here. Back to the nearest road at once.
                if (IsCutOff(player) && CutOffWayOut(player, rt, "is cut off from every road"))
                {
                    rt.lastZone = zone;
                    return;
                }
                OnTravelStuck(bot, rt, Dist2d(player, rt.task.spot));
                rt.lastZone = zone;
                return;
            }
            if (!GroundWayOut(player, rt, "travels and stands"))
            {
                BotDriver::Rest(player);
                OnTravelStuck(bot, rt, Dist2d(player, rt.task.spot));
                rt.lastZone = zone;
                return;
            }
        }
        // Anything else that keeps being cancelled (NewRpg's own approach to a target, a walk the planner does not
        // know): after a few rounds the bot gets the way out too. The guard never stops a bot for long.
        constexpr uint8 GroundStrikeLimit = 3;
        constexpr uint32 GroundStrikeWindowMs = 3 * MINUTE * IN_MILLISECONDS;
        bool const travelling = rt.task.kind == TaskKind::Travel || rt.task.kind == TaskKind::Voyage;
        if (!related && !travelling)
        {
            uint32 const strikeNow = getMSTime();
            if (!rt.groundStrikeAt || getMSTimeDiff(rt.groundStrikeAt, strikeNow) > GroundStrikeWindowMs)
                rt.groundStrikes = 0;
            rt.groundStrikeAt = strikeNow;
            if (++rt.groundStrikes >= GroundStrikeLimit)
            {
                rt.groundStrikes = 0;
                if (!GroundWayOut(player, rt, "keeps being stopped"))
                {
                    BotDriver::DropTarget(player);
                    BotDriver::Rest(player); // NewRpg gives up the walk it kept starting
                }
            }
        }
        // Before the target is given up: the planner's own steps take the walk once (they know the hand-listed crossings of
        // gaps in the path finder's ground, BridgeGap).
        constexpr float BridgeLookYards = 400.0f;
        if (related && !rt.task.bridgeLooked && !rt.task.bridges && rt.task.hasSpot && rt.task.spot.mapId == player->GetMapId() &&
            (rt.task.kind == TaskKind::Pickup || rt.task.kind == TaskKind::TurnIn || rt.task.kind == TaskKind::Vendor ||
             rt.task.kind == TaskKind::Train) && Dist2d(player, rt.task.spot) <= BridgeLookYards)
        {
            rt.task.bridgeLooked = true;
            rt.task.ownSteps = true;
            rt.task.tookOver = true; // the walk stays with the planner
            rt.task.ownFailStreak = 0;
            rt.task.ownStepAt = 0;
            related = false;
        }
        if (related)
        {
            LOG_INFO(ActivityLog, "activity: {} cannot get to ({:.0f}, {:.0f}, {:.0f}) map {} on the ground ({} movements cancelled); "
                "set aside for {} min", bot.name, refusals.x, refusals.y, refusals.z, refusals.mapId, refusals.count,
                AvoidSpawnMs / (MINUTE * IN_MILLISECONDS));
            if (rt.task.kind == TaskKind::Objective || rt.task.kind == TaskKind::Grind || rt.task.kind == TaskKind::Interact)
            {
                rt.AddAvoidArea(refusals.mapId, refusals.x, refusals.y, 255, AvoidSpawnMs, false);
                // Also the task's own spot (a quest area has no spawn to skip): else the same task starts again.
                rt.AddAvoidArea(rt.task.spot.mapId, rt.task.spot.x, rt.task.spot.y, 255, AvoidSpawnMs, false);
            }
            if (rt.task.hasSpot)
                rt.Avoid(rt.task.spot, AvoidSpawnMs);
            NoteNpcUnreached(guild, bot.name, rt.task, rt.task.kind == TaskKind::TurnIn && rt.errandQuest && rt.errandQuest == rt.task.questId);
            ClearTask(rt, 5 * IN_MILLISECONDS, "no way on the ground");
            BotDriver::Rest(player);
        }
    }

    // Grey and failed quests leave the quest log, whatever the order is.
    CareForStaleQuests(guild, bot, player, rt);
    // What the bot made for a craft order of the guild goes into the guild storage when its crafting has ended.
    sCraftOrders.Step(guild, player);

    // A bot on the profession mode leaves its squad (the owner, 2026-11-14: [quote left out]).
    if (uint32 const craftNow = std::max<uint32>(getMSTime(), 1); bot.order.type == OrderType::Craft && craftNow >= rt.nextCraftSquadLook)
    {
        rt.nextCraftSquadLook = craftNow + 30 * IN_MILLISECONDS;
        LeaveSquadForCrafting(guild, bot);
    }

    // A squad member follows its leader instead of playing its order; a leader that waits for a member stands.
    // A quest partner on its way to its leader walks to it (QuestPartners.h).
    if (rt.squad.meeting)
    {
        StepPartnerMeet(guild, bot, player, rt);
        rt.lastZone = player->GetZoneId();
        return;
    }
    if (rt.squad.following)
    {
        if (StepMemberVendorVisit(bot, player, rt))
        {
            rt.lastZone = player->GetZoneId();
            return;
        }
        RunSquadMember(guild, bot, player, rt);
        rt.lastZone = player->GetZoneId();
        return;
    }
    if (StepTrailOut(player, rt))
    {
        rt.lastZone = zone;
        return;
    }
    if (StepSquadWait(player, rt))
    {
        rt.lastZone = zone;
        return;
    }

    // A voyage under way (also going ashore after an order change on board) ends before IDLE plans its walk to the inn:
    // RunIdle would replace it at once. AUTO and LEVEL_ZONE step their tasks first anyway.
    if (bot.order.type == OrderType::Idle && rt.task.kind == TaskKind::Voyage && StepTask(guild, bot, player, rt))
    {
        rt.lastZone = player->GetZoneId();
        return;
    }

    // playerbots loots what lies beside the bot's way (a chest, a pumpkin of a pumpkin patch): it gets the bot off its
    // mount for it, and its mount strategy, which sees a long walk ahead, puts the bot back on before the loot is
    // opened. Four bots stood so at one Ripe Pumpkin beside the way from eastern Elwynn to Darkshire, (-9814, -884),
    // mounting and dismounting for two to five minutes each until their walk was "stuck" (the mount's cast hid them
    // from the walk's stall check). While such a target waits within reach the bot stays off its mount; one it has
    // not looted after half a minute is put off, and it rides on.
    {
        constexpr float LootHoldYards = 30.0f;
        constexpr uint32 LootHoldMs = 30 * IN_MILLISECONDS;
        uint32 const lootNow = std::max<uint32>(getMSTime(), 1);
        std::string lootName;
        WorldObject* lootObject = nullptr;
        bool lootWaits = BotDriver::HasLootTarget(player, LootHoldYards, &lootName, &lootObject);
        // Not a quest object whose trap summons what the bot cannot take (the trapped Kodo Bones beside the untrapped
        // ones of "Bone Collector": mod-playerbots' loot opens quest objects beside the way).
        if (lootWaits && lootObject && lootObject->GetTypeId() == TYPEID_GAMEOBJECT &&
            TrapRefused(player, rt, lootObject->ToGameObject()))
        {
            LOG_INFO(ActivityLog, "activity: {} leaves {} beside its way alone: its trap summons what it cannot take", bot.name, lootName);
            BotDriver::DeferLootTarget(player);
            lootWaits = false;
        }
        // What it held for in vain twice is left alone for a quarter of an hour: mod-playerbots takes it up again after its
        // own short retry delay, and a bot whose way passes it again and again stood beside it each time. (Graveborn and
        // Ainuha, levels 35 and 37, at the Food Crate of Nesingwary's camp in Stranglethorn: off the mount for it, the walk
        // stood 10 s until the planner's own steps took it away, and the next look brought it back; Graveborn's travel
        // ended "stuck" after 371 s, 86 yd from where it began, 2026-11-26.)
        constexpr uint32 LootIgnoreMs = 15 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 LootAgainMs = 10 * MINUTE * IN_MILLISECONDS;
        if (lootWaits && lootObject)
        {
            auto const ignored = rt.lootIgnored.find(lootObject->GetGUID());
            if (ignored != rt.lootIgnored.end() && lootNow < ignored->second)
            {
                BotDriver::DeferLootTarget(player);
                lootWaits = false;
            }
            else if (!rt.lootHoldSince && lootObject->GetGUID().IsGameObject() && rt.lootHoldGuid == lootObject->GetGUID() && rt.lootHoldEndedAt &&
                getMSTimeDiff(rt.lootHoldEndedAt, lootNow) < LootAgainMs)
            {
                rt.lootIgnored[lootObject->GetGUID()] = lootNow + LootIgnoreMs;
                LOG_INFO(ActivityLog, "activity: {} leaves {} beside its way at ({:.0f}, {:.0f}) alone for {} min: it held for it before "
                    "and did not loot it", bot.name, lootName, lootObject->GetPositionX(), lootObject->GetPositionY(),
                    LootIgnoreMs / (MINUTE * IN_MILLISECONDS));
                BotDriver::DeferLootTarget(player);
                lootWaits = false;
            }
        }
        if (!lootWaits)
        {
            // (A hold ended in vain when the object it was for is still there to loot: a chest it opened is gone.)
            if (rt.lootHoldSince)
            {
                GameObject const* held = rt.lootHoldGuid.IsGameObject() ? ObjectAccessor::GetGameObject(*player, rt.lootHoldGuid) : nullptr;
                rt.lootHoldEndedAt = held && held->isSpawned() && held->getLootState() == GO_READY ? lootNow : 0;
            }
            rt.lootHoldSince = 0;
        }
        else if (!rt.lootHoldSince)
        {
            rt.lootHoldSince = lootNow;
            rt.lootHoldGuid = lootObject ? lootObject->GetGUID() : ObjectGuid::Empty;
            rt.lootHoldEndedAt = 0;
            if (player->IsMounted())
                LOG_INFO(ActivityLog, "activity: {} gets off its mount for {} beside its way at ({:.0f}, {:.0f}) and stays off it until "
                    "that is looted", bot.name, lootName, player->GetPositionX(), player->GetPositionY());
        }
        else if (getMSTimeDiff(rt.lootHoldSince, lootNow) >= LootHoldMs)
        {
            LOG_INFO(ActivityLog, "activity: {} has not looted what lies beside its way at ({:.0f}, {:.0f}) map {} in {} s: it leaves it "
                "and goes on", bot.name, player->GetPositionX(), player->GetPositionY(), player->GetMapId(), LootHoldMs / IN_MILLISECONDS);
            if (lootObject && lootObject->GetGUID().IsGameObject())
                rt.lootIgnored[lootObject->GetGUID()] = lootNow + LootIgnoreMs;
            BotDriver::DeferLootTarget(player);
            rt.lootHoldSince = 0;
            rt.lootHoldEndedAt = 0;
            lootWaits = false;
        }
        if (rt.lootIgnored.size() > 32)
            for (auto it = rt.lootIgnored.begin(); it != rt.lootIgnored.end();)
                it = lootNow >= it->second ? rt.lootIgnored.erase(it) : std::next(it);
        rt.lootWaits = lootWaits;
        // The last yards to a place where hostile creatures stand are walked (FightEscape.h).
        Escape::WalksLastYards(player, rt);
        // (The autopilot sets the mount strategy itself, RunAdventure: a bot that leads an NPC walks.)
        if (bot.order.type != OrderType::Auto && bot.order.type != OrderType::LevelZone)
            BotDriver::SetMountAllowed(player, !lootWaits && !rt.onFoot);
    }

    // After fishing: its weapon back into its hand.
    if (rt.fishWeapon && rt.task.kind != TaskKind::Fish)
        RestoreWeapon(player, rt);

    // Inside a dungeon without a run (OrderDungeons.cpp): out first.
    if (LeaveDungeonWithoutRun(guild, bot, player, rt))
    {
        rt.lastZone = player->GetZoneId();
        return;
    }

    switch (bot.order.type)
    {
        case OrderType::Follow:
            RunFollow(bot, player, rt);
            break;
        case OrderType::Idle:
            RunIdle(bot, player, rt);
            break;
        case OrderType::Auto:
        case OrderType::LevelZone:
            // (AUTO: a gathering skill too low for everything its zone has is caught up first; RunAdventure begins
            // a catch-up between two tasks, here it goes on.)
            if (rt.catchUp.skill && CatchUpGathering(guild, bot, player, rt))
                break;
            RunAdventure(guild, bot, player, rt);
            break;
        case OrderType::Craft:
            RunProfessions(guild, bot, player, rt);
            break;
        case OrderType::Dungeon:
            RunDungeon(guild, bot, player, rt);
            break;
        case OrderType::Farm:
            RunFarm(guild, bot, player, rt);
            break;
        default:
            rt.why = Acore::StringFormat("Order {} is not available yet", ToToken(bot.order.type));
            break;
    }

    rt.lastZone = player->GetZoneId();
}

void OrderService::Reconcile(GuildBot const& bot, Player* player, BotRuntime& rt)
{
    // Watch mode: the owner's own client character. It has no master (it would follow itself), and its party is the
    // owner's: following bots join it, and it is not taken out of it for an autonomous order.
    bool const watched = sGuildBotHolder.IsWatched(bot.guid);

    // Master: only FOLLOW follows someone (the character the owner plays), and a squad member its squad leader.
    Player* wantedMaster = nullptr;
    Player* const squadLeader = rt.squad.following ? ObjectAccessor::FindConnectedPlayer(rt.squad.leader) : nullptr;
    if (squadLeader && squadLeader->IsInWorld())
        wantedMaster = squadLeader;
    else if (bot.order.type == OrderType::Follow && !watched)
        wantedMaster = sGuildBotHolder.GetOwnerCharacter(bot.guid);
    if (BotDriver::GetMaster(player) != wantedMaster)
        BotDriver::SetMaster(player, wantedMaster);

    // Party: a following bot joins the owner's party, every other bot plays alone.
    uint32 const now = getMSTime();
    if ((!watched || squadLeader) && now >= rt.nextGroupCheck)
    {
        rt.nextGroupCheck = now + GroupCheckMs;
        if (squadLeader && wantedMaster)
            KeepSquadGroup(player, rt, wantedMaster);
        else if (wantedMaster)
        {
            // (Not while the owner's character is in a dungeon group: its party is that group's, and a bot that
            // follows the owner is no part of it.)
            if (sSquadService.GetLeader(wantedMaster->GetGUID()).IsEmpty())
                BotDriver::JoinGroupOf(player, wantedMaster);
            else if (player->GetGroup() && player->GetGroup() == wantedMaster->GetGroup())
                BotDriver::LeaveNormalGroup(player);
        }
        else if (player->GetGroup() && !rt.squad.leads)
            BotDriver::LeaveNormalGroup(player);
    }

    StrategyProfile profile = StrategyProfile::Adventure;
    if (squadLeader && wantedMaster)
    {
        // A squad member follows; while its leader is dead it stays where the leader fell (no walk after the ghost).
        profile = squadLeader->IsAlive() ? StrategyProfile::Follow : StrategyProfile::Hold;
        // At the Twin Emperors the halves hold their places (StepTwinsPull, StepRaidTwins): mod-playerbots' follow walked
        // the half at Vek'lor's throne back to the leader at the other one, 2026-11-23.
        if (BotRuntime const* leaderRt = FindRuntime(squadLeader->GetGUID()); leaderRt && ((!leaderRt->dungeon.twinSide.empty() &&
            (leaderRt->dungeon.twinPlacesSince || leaderRt->dungeon.twinPulledAt)) ||
            (leaderRt->dungeon.twinBugsSince && !leaderRt->dungeon.twinBugsDone)))
            profile = StrategyProfile::Hold;
        // (And at C'Thun from the pull on: the raid waits out of the Eye's sight, then keeps its places on the rings.)
        if (BotRuntime const* leaderRt = FindRuntime(squadLeader->GetGUID()); leaderRt && !leaderRt->dungeon.cthunSlot.empty())
            profile = StrategyProfile::Hold;
    }
    else switch (bot.order.type)
    {
        case OrderType::Follow:
            profile = wantedMaster ? StrategyProfile::Follow : StrategyProfile::Hold;
            break;
        case OrderType::Idle:
            profile = rt.task.kind == TaskKind::SafeSpot ? StrategyProfile::Travel : StrategyProfile::Hold;
            break;
        // The profession mode: it walks (to a place of nodes, a vendor, the inn) or hunts for skins with the targets the
        // planner picks; else it stands and crafts.
        case OrderType::Craft:
            profile = rt.task.kind == TaskKind::Travel || rt.task.kind == TaskKind::Grind || rt.task.kind == TaskKind::Vendor ||
                rt.task.kind == TaskKind::SafeSpot || (rt.task.kind == TaskKind::Fish && !rt.task.nearSince) ?
                StrategyProfile::Travel : StrategyProfile::Hold;
            break;
        default:
            switch (rt.task.kind)
            {
                case TaskKind::Hold:
                    profile = StrategyProfile::Hold;
                    break;
                case TaskKind::Voyage:
                    profile = rt.task.voyage == VoyagePhase::ToDock ||
                        (rt.task.voyage == VoyagePhase::Wait && !player->GetTransport() &&
                         Dist2d(player, rt.task.link.from.x, rt.task.link.from.y) > DockReachedDistance) ?
                        StrategyProfile::Travel : StrategyProfile::Hold;
                    break;
                // Walking somewhere: no detours after quest mobs.
                case TaskKind::Travel:
                case TaskKind::Pickup:
                case TaskKind::TurnIn:
                case TaskKind::Vendor:
                case TaskKind::Train:
                case TaskKind::SafeSpot:
                case TaskKind::Discover:
                case TaskKind::Event:
                // With an escort: no grinding at what stands beside the way (the NPC walks on meanwhile).
                case TaskKind::Escort:
                // Grinding: the planner picks what is fought (playerbots' own grinding takes creatures up to four
                // levels above the bot: a level-3 character sent to grind next to Goldshire fought level-6 wolves).
                case TaskKind::Grind:
                    profile = StrategyProfile::Travel;
                    break;
                // A quest interaction: walk there without detours, then stand still for the use or the casts (the
                // planner moves the last yards itself); fights back when attacked.
                case TaskKind::Interact:
                    profile = rt.task.nearSince && !rt.task.leading ? StrategyProfile::Hold : StrategyProfile::Travel;
                    break;
                // Fishing: walk to the water, then stand (fights back when attacked).
                case TaskKind::Fish:
                    profile = rt.task.nearSince ? StrategyProfile::Hold : StrategyProfile::Travel;
                    break;
                default:
                    profile = StrategyProfile::Adventure;
                    break;
            }
            break;
    }

    // Next to a pull it just let go of (CheckPull): it walks on without pulling anything by itself, until it is away
    // from there (playerbots' grinding would turn to the same creature again).
    if (rt.pullHoldUntil)
    {
        constexpr float PullHoldYards = 70.0f;
        if (getMSTime() >= rt.pullHoldUntil || player->GetMapId() != rt.pullHoldMap ||
            Dist2d(player, rt.pullHoldX, rt.pullHoldY) > PullHoldYards)
            rt.pullHoldUntil = 0;
        else if (profile == StrategyProfile::Adventure)
            profile = StrategyProfile::Travel;
    }

    // Resting, or a squad leader waiting for a member: nothing walks or pulls (the bot still fights back).
    if (rt.rest.active || !rt.squad.waitFor.empty())
        profile = StrategyProfile::Hold;

    // Self-healing: playerbots resets strategies when a master leaves; the next tick puts the profile back.
    if (!BotDriver::HasProfile(player, profile))
        BotDriver::ApplyProfile(player, profile);

    // No teleport-for-level after repeated deaths (the design log section 1): a living bot's death count is reset.
    if (player->IsAlive())
        BotDriver::ResetDeathCount(player);
    // No teleport to the destination when a long walk makes no progress: UpdateStuck gives such targets up.
    BotDriver::HoldOffStuckTeleport(player);
}

void OrderService::Sample(GuildBot const& /*bot*/, Player* player, BotRuntime& rt)
{
    uint32 const now = Now();
    if (now < rt.nextSample)
        return;
    rt.nextSample = now + SampleGapSec;

    rt.samples.push_back({ now, TotalXp(player), player->GetMoney() });
    while (!rt.samples.empty() && rt.samples.front().time + SampleWindowSec < now)
        rt.samples.pop_front();
}

// A task whose bot should be on its way (CheckStranded's standstill, TrackActivity's "stood with a walk to do").
static bool TaskWalks(BotTask const& task)
{
    switch (task.kind)
    {
        case TaskKind::Travel:
        case TaskKind::Pickup:
        case TaskKind::TurnIn:
        case TaskKind::Vendor:
        case TaskKind::Train:
        case TaskKind::Discover:
        case TaskKind::Objective:
        case TaskKind::Grind:
            return true;
        case TaskKind::Voyage:
            return task.voyage == VoyagePhase::ToDock;
        default:
            return false;
    }
}

// How active the bot is (the owner, 2026-11-25: [quote left out]). Summed per status window (BotRuntime::Tally) and
// written on the status line: the time it stood still out of a fight, a rest and a cast, and how much of that it had a
// walk to do; the yards it went against how far it got (the status line's "moved", a straight line); the stretches of
// 15 yd that turned back on the one before; and in fights the turns to another creature while the one before lived.
// "busy": it crafts or enchants (UpdateGuildBot leaves UpdateBot out then): no standstill, the look only moves on.
void OrderService::TrackActivity(Player* player, BotRuntime& rt, bool busy)
{
    constexpr float StillYards = 0.5f;    // less than this since the last look: it stood
    constexpr float JumpYards = 50.0f;    // more than this: a teleport, not a walk
    constexpr uint32 GapCapMs = 5000;     // a longer gap (a restart, a pause of the planner) counts as this much
    constexpr float LegYards = 15.0f;
    constexpr float TurnBackRadians = 150.0f * float(M_PI) / 180.0f;
    constexpr float AtTargetYards = 10.0f;

    uint32 const now = getMSTime();
    float const x = player->GetPositionX();
    float const y = player->GetPositionY();
    bool const sameMap = rt.hasActivity && rt.activityMap == player->GetMapId();
    uint32 const gap = sameMap ? std::min(getMSTimeDiff(rt.activityAt, now), GapCapMs) : 0;
    float const step = sameMap ? std::hypot(x - rt.activityX, y - rt.activityY) : 0.0f;
    bool const carried = player->IsInFlight() || player->GetTransport() || player->IsBeingTeleported();
    bool const fights = player->IsInCombat();

    auto resetLeg = [&]()
    {
        rt.legX = x;
        rt.legY = y;
        rt.hasLegHeading = false;
    };
    if (!sameMap || carried || step > JumpYards)
        resetLeg();
    else if (step >= StillYards)
    {
        rt.window.walkedYards += step;
        rt.total.walkedYards += step;
        // Turns back: on its own way only (a fight's chase circles its foe).
        if (fights)
            resetLeg();
        else if (std::hypot(x - rt.legX, y - rt.legY) >= LegYards)
        {
            float const heading = std::atan2(y - rt.legY, x - rt.legX);
            if (rt.hasLegHeading)
            {
                float turn = std::fabs(heading - rt.legHeading);
                if (turn > float(M_PI))
                    turn = 2.0f * float(M_PI) - turn;
                if (turn > TurnBackRadians)
                {
                    ++rt.window.turnBacks;
                    ++rt.total.turnBacks;
                }
            }
            rt.legHeading = heading;
            rt.hasLegHeading = true;
            rt.legX = x;
            rt.legY = y;
        }
    }
    else if (!busy && !fights && player->IsAlive() && !rt.rest.active && !player->IsNonMeleeSpellCast(false))
    {
        rt.window.stoodMs += gap;
        rt.total.stoodMs += gap;
        // With a walk to do: a task that walks, its target farther off, nothing it waits for on purpose (away from a
        // creature, for one on its way, the planner's own wait).
        bool const waits = rt.escape.active || (rt.task.roamerWaitUntil && now < rt.task.roamerWaitUntil) ||
            (rt.waitUntil && now < rt.waitUntil);
        std::size_t const bucket = waits ? BotRuntime::Tally::StoodWaiting :
            std::min<std::size_t>(std::size_t(rt.task.kind), BotRuntime::Tally::StoodWaiting - 1);
        rt.window.stoodByKindMs[bucket] += gap;
        rt.total.stoodByKindMs[bucket] += gap;
        if (TaskWalks(rt.task) && !waits && (!rt.task.hasSpot || Dist2d(player, rt.task.spot) > AtTargetYards))
        {
            rt.window.stoodWalkMs += gap;
            rt.total.stoodWalkMs += gap;
        }
    }

    // Target switches: the fight turned to another creature while the one before was still alive and fighting.
    if (Unit* victim = fights ? player->GetVictim() : nullptr)
    {
        if (!rt.lastVictim.IsEmpty() && victim->GetGUID() != rt.lastVictim)
        {
            Unit* before = ObjectAccessor::GetUnit(*player, rt.lastVictim);
            if (before && before->IsAlive() && before->IsInCombat())
            {
                ++rt.window.targetSwitches;
                ++rt.total.targetSwitches;
            }
        }
        rt.lastVictim = victim->GetGUID();
    }
    else if (!fights)
        rt.lastVictim.Clear();

    rt.hasActivity = true;
    rt.activityMap = player->GetMapId();
    rt.activityAt = now;
    rt.activityX = x;
    rt.activityY = y;
}

// ---------------------------------------------------------------------------------------------------------------
// Orders
// ---------------------------------------------------------------------------------------------------------------

void OrderService::RunFollow(GuildBot& bot, Player* player, BotRuntime& rt)
{
    ClearTask(rt, 0, "following");
    if (Player* master = BotDriver::GetMaster(player))
        rt.why = Acore::StringFormat("Following {}", master->GetName());
    else if (sGuildBotHolder.IsWatched(bot.guid))
        rt.why = "Holding position: you watch this character, so there is no one to follow";
    else
        rt.why = "Waiting for you to come online (holding position)";
}

void OrderService::RunIdle(GuildBot& /*bot*/, Player* player, BotRuntime& rt)
{
    uint32 const now = getMSTime();
    if (rt.task.kind == TaskKind::SafeSpot)
    {
        float const distance = Dist2d(player, rt.task.spot);
        if (distance <= 15.0f || UpdateStuck(player, rt, rt.task.spot.x, rt.task.spot.y))
        {
            BotTask hold;
            hold.kind = TaskKind::Hold;
            hold.targetName = rt.task.targetName;
            StartTask(player, rt, hold);
        }
        else
            WalkTo(player, rt, rt.task.spot.mapId, rt.task.spot.x, rt.task.spot.y, rt.task.spot.z);
        rt.why = Acore::StringFormat("Idle: walking to {} ({})", rt.task.targetName, RoughDistance(distance));
        return;
    }

    if (rt.task.kind == TaskKind::Hold)
    {
        rt.why = rt.task.targetName.empty() ? "Idle: holding position" : Acore::StringFormat("Idle at {}", rt.task.targetName);
        return;
    }

    // The nearest inn of the bot's region; without one the bot holds where it is.
    if (ServicePoint const* inn = NearestService(ServiceType::Innkeeper, player, rt))
    {
        // (At the inn already: it holds there; a walk of 0 yd every 30 s said "at the inn" and nothing else.)
        if (inn->spawn.mapId == player->GetMapId() && Dist2d(player, inn->spawn) <= 15.0f)
        {
            BotTask hold;
            hold.kind = TaskKind::Hold;
            hold.targetName = Acore::StringFormat("the inn ({})", inn->name);
            StartTask(player, rt, hold);
            rt.why = Acore::StringFormat("Idle at {}", hold.targetName);
            return;
        }
        BotTask task;
        task.kind = TaskKind::SafeSpot;
        task.hasSpot = true;
        task.spot = inn->spawn;
        task.targetName = Acore::StringFormat("the inn ({})", inn->name);
        // Name the inn after its area ("the inn in Goldshire"); the spawn is on the bot's map.
        uint32 const area = player->GetMap()->GetAreaId(PHASEMASK_NORMAL, inn->spawn.x, inn->spawn.y, inn->spawn.z);
        if (std::string const areaName = GetAreaName(area); !areaName.empty())
            task.targetName = Acore::StringFormat("the inn in {}", areaName);
        StartTask(player, rt, task);
        // (Along a route, as every walk of the planner: one straight NewRpg move to an inn 933 yd away was refused by
        // the ground check every 30 s for half an hour, from a pocket of Hive'Zora in Silithus, round 149.)
        WalkTo(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z);
        rt.task.lastAssert = now;
        rt.why = Acore::StringFormat("Idle: walking to {}", rt.task.targetName);
        return;
    }

    BotTask hold;
    hold.kind = TaskKind::Hold;
    StartTask(player, rt, hold);
    rt.why = "Idle: holding position";
}

void OrderService::RunAdventure(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    uint32 const now = getMSTime();

    uint8 const level = player->GetLevel();

    // It found nothing it can safely get to twice from one place (below): the hearthstone only when it is still there
    // three minutes later, between two tasks and out of a fight; a bot that left by the zone it picked has moved on.
    {
        constexpr uint32 StayedMs = 3 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 PendingMs = 15 * MINUTE * IN_MILLISECONDS;
        constexpr float StayedYards = 300.0f;
        if (rt.nothingSafeHearthAt)
        {
            uint32 const since = getMSTimeDiff(rt.nothingSafeHearthAt, now);
            bool const stayed = player->GetMapId() == rt.nothingSafeMap &&
                Dist2d(player, rt.nothingSafeX, rt.nothingSafeY) <= StayedYards;
            if (!stayed || since > PendingMs)
                rt.nothingSafeHearthAt = 0;
            // (Not while it fights where it stands: a bot that grinds there has work. Thunderhoof (52) ground the dinosaurs
            // of Un'Goro at 65000 experience an hour, its quests there behind places it had set aside, and took its
            // hearthstone to Feralas 74 s after its last fight, round 260.)
            else if (since >= StayedMs && rt.task.kind == TaskKind::None && !player->IsInCombat() &&
                (!rt.lastFightEndAt || getMSTimeDiff(rt.lastFightEndAt, now) >= StayedMs))
            {
                rt.nothingSafeHearthAt = 0;
                if (HearthHome(player, rt, Acore::StringFormat("it found nothing it can safely get to from here twice and is still "
                        "here {} min later", since / (MINUTE * IN_MILLISECONDS)), StrandedHearthYards))
                {
                    rt.nothingSafeAt = 0;
                    return;
                }
            }
        }
    }

    // Level cap (AUTO limit): stop levelling and wait at the nearest inn. RunIdle keeps its own walk and hold, so this
    // comes before the task step (which would end the walk at once). A voyage under way is finished first.
    if (bot.order.type == OrderType::Auto && bot.order.levelCap && level >= bot.order.levelCap &&
        rt.task.kind != TaskKind::Voyage)
    {
        RunIdle(bot, player, rt);
        rt.why = Acore::StringFormat("Reached the level cap {}: {}", uint32(bot.order.levelCap), rt.why);
        return;
    }

    // The server's level cap with no quest left (below): wait at the inn like IDLE; the zones are looked at again when
    // the pause ends.
    if (rt.capIdleUntil && rt.task.kind != TaskKind::Voyage)
    {
        bool stay = IsAtLevelCap(player);
        if (stay && now >= rt.capIdleUntil)
        {
            // Look again from the inn (no walk back to the zone for nothing): a zone with a quest to pick up by now?
            rt.capIdleUntil = now + CapIdleRecheckMs;
            uint32 const zoneId = bot.order.type == OrderType::LevelZone ? bot.order.zoneId : ChooseZone(guild, bot, player, 0);
            // (A zone across the sea is counted from afar, as ChooseZone counted it: counted as from here it has none,
            // and three bots at the cap stayed at their inns with Eastern Plaguelands picked every five minutes, round 142.)
            stay = !zoneId || (CountEligibleQuests(player, zoneId) == 0 &&
                (zoneId == player->GetZoneId() || CountEligibleQuests(player, zoneId, true) == 0));
            // (No quest anywhere for it: the planner looks again at its professions and at grinding, the owner 2026-11-21;
            // the wait was for good before, the five minutes are now the pause between two looks.)
            stay = false;
        }
        if (stay)
        {
            RunIdle(bot, player, rt);
            rt.why = Acore::StringFormat("Level {} and no quests left for it: {}", uint32(level), rt.why);
            return;
        }
        rt.capIdleUntil = 0;
        rt.zoneChosenAt = 0; // the zone is picked again at once (the pick that found the quests may lie across the sea)
        ClearTask(rt, 0, "looks at its quests, professions and grinding again");
    }

    // The quest log is cared for wherever the bot is and whatever it does, not only when its zone is planned: on an
    // errand, a travel or a voyage it fills up as well (quests taken in passing, quests a script hands out).
    CareForQuestLog(bot, player, rt, rt.targetZone ? rt.targetZone : bot.order.zoneId);

    // Boxed in where it stands: home by hearthstone.
    if (LeaveBoxedPlace(player, rt))
        return;

    // Keep going with the current task while it runs.
    // The follow movement of an escort task that is over (the next task may not move the bot at once).
    if (rt.task.kind != TaskKind::Escort && player->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
        player->GetMotionMaster()->Clear();

    // Escorts do not wait: NewRpg takes quests in passing, and an escort NPC walks off the moment its quest is taken.
    // A task that is not the escort ends when such an NPC is next to the bot; a failed escort is given back at once, so
    // it can be taken again.
    if (rt.task.kind != TaskKind::Escort && rt.task.kind != TaskKind::Voyage)
    {
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = player->GetQuestSlotQuestId(slot);
            uint32 const escortEntry = questId && !sQuestSupport.IsLeftAlone(player, questId) ? sQuestSupport.GetEscortNpc(questId) : 0;
            if (!escortEntry)
                continue;
            QuestStatus const status = player->GetQuestStatus(questId);
            if (status == QUEST_STATUS_FAILED)
            {
                LOG_INFO(ActivityLog, "activity: {} lost the escort of quest {} \"{}\"", bot.name, questId, QuestTitle(questId));
                FailQuest(guild, bot, player, questId, "escort_failed");
            }
            else if (status == QUEST_STATUS_INCOMPLETE && rt.task.kind != TaskKind::None)
            {
                // (Not for an escort the bot will not follow: blocked after two failures, it is in no plan, and its NPC
                // walking off with somebody else ended the bot's pickup every second for ten seconds, 2026-11-25.)
                if (auto block = bot.questBlocks.find(questId); block != bot.questBlocks.end() && block->second.blocked)
                    continue;
                Creature* escort = player->FindNearestCreature(escortEntry, SightDistance, true);
                if (escort && escort->isMoving())
                {
                    ClearTask(rt, 0, "an escort set off");
                    break;
                }
            }
        }
    }

    // Quest events do not wait either: a quest whose event starts when it is taken (a defence at the giver, an NPC that
    // follows) ends the running task the moment it appears in the log.
    if (TrackEventQuests(player, rt))
        ClearTask(rt, 0, "a quest event started");
    // Leading a follower is done on foot (a mounted bot rides away from it).
    // Nor does a bot ride while something to loot waits next to it (UpdateBot).
    // Nor on the last yards to a place where hostile creatures stand (Escape::WalksLastYards, looked at in UpdateBot).
    BotDriver::SetMountAllowed(player, !rt.lootWaits && !rt.onFoot && !(rt.task.kind == TaskKind::Event &&
        rt.task.event.kind == QuestEventKind::Lead && rt.task.eventFresh));

    if (GatherDetour(bot, player, rt))
        return;
    if (rt.task.kind != TaskKind::None && StepTask(guild, bot, player, rt))
        return;
    // (The task ended and left a journey in its place, StartJourney: that is the task now.)
    if (rt.task.kind == TaskKind::Travel && rt.task.journey)
        return;
    if (now < rt.waitUntil)
        return;
    // Wherever the bot is: a quest container in its bags is opened, a quest event that just started is played, a talk
    // partner next to it is talked to.
    OpenQuestContainers(bot, player);
    if (PlanFreshEvent(bot, player, rt) || PlanNearbyTalk(bot, player, rt))
        return;

    // Target zone.
    if (bot.order.type == OrderType::LevelZone)
    {
        rt.targetZone = bot.order.zoneId;
        CheckOutleveled(guild, bot, player, rt);
        rt.targetZone = bot.order.zoneId;
    }
    // (Looked at again by the clock only in the zone itself: on the way there the pick holds, whatever the distances
    // look like from where a fight or a death left the bot. A zone that cannot be reached, a refused walk, a level-up
    // ask for a new pick themselves.)
    // (And at once when the bot stands outside a zone it has set aside for itself: its pick was that zone while it
    // stood in it, an objective just across the border took it out, and the hold below, meant for an ordered zone
    // that cannot be reached, kept it standing with "Cannot find a way to Arathi Highlands; trying again in 40 min",
    // beside the Dark Iron dwarves of Dun Modr, a bridge away from a zone it had set aside for its creatures.)
    else if (!rt.targetZone || !rt.zoneChosenAt || (now - rt.zoneChosenAt >= ZoneRecheckMs && player->GetZoneId() == rt.targetZone) ||
        (player->GetZoneId() != rt.targetZone && rt.IsZoneUnreachable(rt.targetZone)))
    {
        uint32 const chosen = ChooseZone(guild, bot, player, 0);
        rt.zoneChosenAt = now;
        if (chosen != rt.targetZone && chosen && GetSettings().debug)
            LOG_INFO("module", "mod-guild-adventure: {} (AUTO) picks {} at level {}", bot.name, ZoneName(chosen), uint32(level));
        rt.targetZone = chosen;
    }

    // Between two tasks: a gathering skill of its plan that is too low for everything its zone has is caught up before
    // the next quest task (CatchUpGathering). Looked at once the zone is picked, wherever the bot stands.
    if (bot.order.type == OrderType::Auto && rt.task.kind == TaskKind::None && CatchUpGathering(guild, bot, player, rt))
        return;

    // A rogue with no poison at all and a bare weapon takes the poison vendor of the town it stands in (PlanPoisonStop).
    if (rt.task.kind == TaskKind::None && PlanPoisonStop(player, rt))
        return;

    ZoneTemplate const* zone = rt.targetZone ? sGuildAdventureMgr.GetZone(rt.targetZone) : nullptr;
    // LEVEL_ZONE into a zone the bot repeatedly got stuck on the way to: wait (AUTO already picked another zone).
    if (zone && player->GetZoneId() != zone->zoneId && rt.IsZoneUnreachable(zone->zoneId))
    {
        BotTask hold;
        hold.kind = TaskKind::Hold;
        StartTask(player, rt, hold);
        rt.why = Acore::StringFormat("Cannot find a way to {}; trying again in {} min", zone->name,
            (rt.unreachableZones[zone->zoneId] - now) / (MINUTE * IN_MILLISECONDS) + 1);
        rt.waitUntil = now + HoldReplanMs;
        return;
    }
    if (!zone)
    {
        // No zone it can get to from here: first the hand-ins it can reach from here (an errand that went ashore across
        // the sea goes on: a level-51 paladin landed in Rut'theran for a hand-in in Darnassus, found no zone on that side
        // and used its hearthstone at once). The hearthstone only after the minutes below (the owner: the hearthstone only in
        // the utmost emergency).
        if (bot.order.type == OrderType::Auto && PlanErrand(guild, bot, player, rt, rt.targetZone, true))
            return;
        // Still none after a while: whatever it waits with is over (the zones it found no way into, the hour on its
        // side of the sea, the pier it was refused at), so the next look may take the ship back or a flight; home by
        // hearthstone from anywhere but its inn; and back on a road when no road is in its reach. (A level-31 bot stood
        // on a pier of Theramore for half an hour: its inn stood there, its ways inland were refused, and the landing
        // hour forbade the ship back.)
        constexpr uint32 NoZoneOutMs = 3 * MINUTE * IN_MILLISECONDS;
        if (bot.order.type == OrderType::Auto)
        {
            if (!rt.noZoneSince)
                rt.noZoneSince = now ? now : 1;
            else if (getMSTimeDiff(rt.noZoneSince, now) >= NoZoneOutMs)
            {
                rt.noZoneSince = now ? now : 1;
                // What it waited with may open a zone: the zones are looked at again first, and the hearthstone comes
                // half a minute later when there is still none. (A level-54 warrior in Silithus had five zones waiting;
                // they were dropped and its stone used in the same step, and from its inn in Feralas it picked Felwood
                // with 5 quests, a flight Cenarion Hold had as well, 2026-11-27.)
                constexpr uint32 NoZoneLookAgainMs = 30 * IN_MILLISECONDS;
                bool const dropped = !rt.unreachableZones.empty() || rt.CrossedLately() || rt.errandCrossing || rt.seaClosedUntil > now;
                LOG_INFO(ActivityLog, "activity: {} has had no zone it can get to for {} min at ({:.0f}, {:.0f}) map {}: what it waits "
                    "with is dropped ({} zones, the hour on its side of the sea {}), and {}", bot.name,
                    NoZoneOutMs / (MINUTE * IN_MILLISECONDS), player->GetPositionX(), player->GetPositionY(), player->GetMapId(),
                    rt.unreachableZones.size(), rt.CrossedLately() ? "too" : "was over",
                    dropped ? "it looks at the zones again (a way out in 30 s if none)" : "it looks for a way out");
                rt.unreachableZones.clear();
                rt.lastCrossingAt = 0;
                rt.errandCrossing = 0;
                rt.seaClosedUntil = 0;
                rt.zoneChosenAt = 0;
                if (dropped)
                {
                    uint32 const back = NoZoneOutMs - NoZoneLookAgainMs;
                    rt.noZoneSince = now > back ? now - back : 1;
                    rt.waitUntil = now;
                    return;
                }
                if (HearthHome(player, rt, "no zone is reachable from where it stands", 100.0f))
                    return;
                if (IsCutOff(player) && CutOffWayOut(player, rt, "has no zone it can get to and is cut off from every road"))
                    return;
            }
        }
        BotTask hold;
        hold.kind = TaskKind::Hold;
        StartTask(player, rt, hold);
        rt.why = bot.order.type == OrderType::Auto ?
            Acore::StringFormat("No zone for level {} is reachable from {}", uint32(level), ZoneName(player->GetZoneId())) :
            "The ordered zone is unknown";
        rt.waitUntil = now + HoldReplanMs;
        return;
    }

    rt.noZoneSince = 0;

    // Errands: completed quests whose ender is outside the target zone (OrderErrands.cpp). A bot outside its zone (on
    // its way there, or on an errand already) does every hand-in it can reach first; in the zone only a batch counts.
    // (Not for the leader of a quest partner squad: the quest it was formed for comes first, QuestPartners.h.)
    if (!(rt.squad.partnerQuest && rt.squad.leads) && PlanErrand(guild, bot, player, rt, zone->zoneId, player->GetZoneId() != zone->zoneId))
        return;

    // On its way to its zone with its inn in that zone and far from here: the hearthstone is the way, as it is a
    // player's way back from an errand in a city or across the sea. (With instant flights what a travel costs is the
    // walk to the flight master and from the landing: travels with a flight took 88 s on average on 2026-11-13,
    // voyages 168 s; the stone takes ten.)
    auto hearthToZone = [&]()
    {
        return player->GetZoneId() != zone->zoneId && TravelHearthAllowed(player) &&
            sMapMgr->GetZoneId(PHASEMASK_NORMAL, player->m_homebindMapId, player->m_homebindX, player->m_homebindY, player->m_homebindZ) ==
                zone->zoneId &&
            HearthHome(player, rt, Acore::StringFormat("its inn is in {}, the zone it is going to", zone->name), DefaultHearthYards);
    };

    Region const here = GetRegion(player);
    Region const there = GetZoneRegion(zone->continent, zone->zoneId, player->GetTeamId());
    if (here != there && hearthToZone())
        return;
    // Already in the zone, but not in the region of its hub (an Alliance bot on the mainland of Feralas, whose hub is
    // Feathermoon on its island): what the zone has on this side comes before the ferry.
    if (here != there && player->GetZoneId() == zone->zoneId)
    {
        if (PlanUpkeep(bot, player, rt))
            return;
        CareForQuestLog(bot, player, rt, zone->zoneId);
        if (PlanQuests(bot, player, rt, zone->zoneId))
            return;
    }
    if (here != there)
    {
        ZoneHub const* hub = sWorldIndex.GetHub(zone->zoneId, player->GetTeamId());
        if (TravelLink const* link = hub ? sWorldIndex.NextTravelLink(here, there, player->GetTeamId(), player->GetMapId(),
            player->GetPositionX(), player->GetPositionY(), hub->mapId, hub->x, hub->y) : nullptr)
        {
            StartVoyage(player, rt, *link);
            StepTask(guild, bot, player, rt);
            return;
        }

        BotTask hold;
        hold.kind = TaskKind::Hold;
        StartTask(player, rt, hold);
        rt.why = Acore::StringFormat("Cannot reach {} from {} (no ship or zeppelin for us goes there)", zone->name, ToString(here));
        rt.waitUntil = now + HoldReplanMs;
        return;
    }

    // At a hub, right after a hand-in: what gives skill in a planned profession is crafted from the bags and the guild
    // storage (the owner, 2026-11-12: [quote left out]). The planner leaves a crafting bot alone until its casts are done.
    if (sBotSettings.IsOn(player, Setting::ProfessionsCraftAtHub) && rt.handInAt && getMSTimeDiff(rt.handInAt, now) <= HubWindowMs && now >= rt.nextCraftCheck)
    {
        rt.nextCraftCheck = now + CraftCheckGapMs;
        std::string text;
        // Only where nothing hostile is around (IsQuietToCraft): not every place a quest is handed in at is a town.
        uint32 hostile = 0;
        bool const quiet = IsQuietToCraft(player, &hostile);
        if (!quiet && now >= rt.nextUnquietNote)
        {
            rt.nextUnquietNote = now + UnquietNoteMs;
            LOG_INFO(ActivityLog, "activity: {} does not craft at ({:.0f}, {:.0f}) after its hand-in: {} hostile creatures within {:.0f} yd",
                bot.name, player->GetPositionX(), player->GetPositionY(), hostile, CraftQuietYards);
        }
        // What the guild ordered comes before what only gives skill.
        if (quiet && sCraftOrders.Work(guild, player, text))
        {
            BotDriver::Rest(player);
            rt.why = "Crafting for the guild's order: " + text;
            return;
        }
        // Its professions take turns, each with its own look (WorkOnProfessions): at most MaxHubBatches batches per
        // hand-in, the next one when the batch before is made.
        constexpr uint8 MaxHubBatches = 3;
        // (The planner leaves a crafting bot alone until its casts are done; the gap is only the time to the next look.
        // With 20 s a bot had left the hub for its next task between two batches.)
        constexpr uint32 HubBatchGapMs = 3 * IN_MILLISECONDS;
        // A piece of gear somebody in the guild would wear comes first, once in five minutes (and what it takes: a
        // material made, a purchase near the hub; the next look goes on from there). It used to come after everything
        // that gives skill, and so hardly ever: something always gives skill.
        if (quiet && CraftGearForGuild(guild, bot, player, rt))
        {
            rt.nextCraftCheck = now + HubBatchGapMs;
            return;
        }
        if (quiet && rt.hubBatches < MaxHubBatches)
            switch (WorkOnProfessions(guild, bot, player, rt, uint32(sBotSettings.Get(player, Setting::ProfessionsCraftBatch)), false))
            {
                case ProfessionWork::Crafting:
                    ++rt.hubBatches;
                    rt.nextCraftCheck = now + HubBatchGapMs;
                    return;
                case ProfessionWork::Shopping:
                    // Back here once it is bought: the hub's time is still running.
                    rt.nextCraftCheck = now + HubBatchGapMs;
                    return;
                case ProfessionWork::Walking:
                    // On its way to the anvil or forge next to the hub: the look goes on there.
                    rt.nextCraftCheck = now + HubBatchGapMs;
                    return;
                default:
                    break;
            }
    }
    // And a few casts where water lies next to the hub, for a bot with Fishing in its plan: the gathering professions
    // rise while it levels (the owner, 2026-11-12), and Fishing only rises by fishing. At most every twenty minutes.
    constexpr uint32 HubFishCasts = 8;
    constexpr uint32 HubFishGapMs = 20 * MINUTE * IN_MILLISECONDS;
    if (bot.order.type != OrderType::Craft && rt.handInAt && getMSTimeDiff(rt.handInAt, now) <= HubWindowMs && now >= rt.nextHubFishing &&
        !player->IsInCombat() && PlanFishing(bot, player, rt, HubFishCasts))
    {
        rt.nextHubFishing = now + HubFishGapMs;
        return;
    }
    // What stands next to the bot comes before a trip to a vendor: the ender of a finished quest, a quest giver (a
    // level-4 character walked 535 yd to an innkeeper for food with its hand-in 100 yd away).
    if (player->GetZoneId() == zone->zoneId && PlanQuests(bot, player, rt, zone->zoneId, NearQuestNpcDistance))
        return;
    if (PlanUpkeep(bot, player, rt))
        return;
    if (PlanDiscovery(player, rt))
        return;

    // Outside the zone with work of its quests near: on with that work. The zone's quests send a bot across its border
    // (the goretusks of "Redridge Goulash" live in Westfall for a bot of Duskwood), and a bot that walked back into its
    // zone after every spawn it had emptied, and out again for the next one, spent nine of twelve minutes on the way.
    constexpr float BorderWorkDistance = 600.0f;
    if (player->GetZoneId() != zone->zoneId && PlanQuests(bot, player, rt, zone->zoneId, BorderWorkDistance, true))
        return;

    if (player->GetZoneId() != zone->zoneId)
    {
        if (hearthToZone())
            return;
        ZoneHub const* hub = sWorldIndex.GetHub(zone->zoneId, player->GetTeamId());
        if (!hub || hub->mapId != player->GetMapId())
        {
            BotTask hold;
            hold.kind = TaskKind::Hold;
            StartTask(player, rt, hold);
            rt.why = Acore::StringFormat("No known way into {}", zone->name);
            rt.waitUntil = now + HoldReplanMs;
            return;
        }

        BotTask task;
        task.kind = TaskKind::Travel;
        task.hasSpot = true;
        task.spot.mapId = hub->mapId;
        task.spot.x = hub->x;
        task.spot.y = hub->y;
        task.spot.z = hub->z;
        task.targetName = zone->name;
        // Where this bot has quests to take there, if it has any: the hub is the same for every level (Ashenvale's lies
        // among level-30 creatures, and a level-21 bot's way to it was "too dangerous" although Maestra's Post is not).
        SpawnPoint entry;
        if (ZoneEntry(bot, player, rt, *zone, entry))
            task.spot = entry;
        // Begun again and again without getting nearer (a pack or a fight on the way ends it each time): the zone
        // waits like one the bot could not find a way into, and the autopilot picks another.
        float const left = Dist2d(player, task.spot);
        if (rt.zoneTravelZone != zone->zoneId || !rt.zoneTravelStarts || getMSTimeDiff(rt.zoneTravelAt, now) > TravelRestartWindowMs ||
            left < rt.zoneTravelLeft - TravelRestartProgress)
        {
            rt.zoneTravelZone = zone->zoneId;
            rt.zoneTravelStarts = 0;
            rt.zoneTravelAt = now;
            rt.zoneTravelLeft = left;
            rt.zoneTravelFromX = player->GetPositionX();
            rt.zoneTravelFromY = player->GetPositionY();
        }
        if (++rt.zoneTravelStarts > TravelRestartLimit)
        {
            rt.zoneTravelStarts = 0;
            // Still where the first of these travels began: it is the place that holds the bot, not the zone it wants
            // to go to. A level-20 warlock stood in Moonbrook with Defias packs on every side: Redridge, Duskwood, Loch
            // Modan and the Wetlands were written off within two minutes, each after four starts of a few seconds, and
            // it set out for Darkshore across the sea. The zone stays; the bot leaves by hearthstone, or waits a
            // minute for the packs to move (a bot that stands for five minutes is put back on a road, CheckStranded).
            float const moved = std::sqrt((player->GetPositionX() - rt.zoneTravelFromX) * (player->GetPositionX() - rt.zoneTravelFromX) +
                (player->GetPositionY() - rt.zoneTravelFromY) * (player->GetPositionY() - rt.zoneTravelFromY));
            if (moved < BoxedInYards)
            {
                LOG_INFO(ActivityLog, "activity: {} began its travel to {} {} times within {} min and is still where it began ({:.0f} yd "
                    "from there): the place holds it, not the zone, which stays its zone", bot.name, zone->name,
                    uint32(TravelRestartLimit) + 1, getMSTimeDiff(rt.zoneTravelAt, now) / (MINUTE * IN_MILLISECONDS) + 1, moved);
                // It waits first: what holds it is often a creature on its round (a Shore Strider at the Feathermoon
                // ferry held a rogue for one minute, and it used its hearthstone). The second time within a quarter of an
                // hour the hearthstone (the owner: only in the utmost emergency).
                constexpr uint32 PlaceHeldWindowMs = 15 * MINUTE * IN_MILLISECONDS;
                if (!rt.placeHeldAt || getMSTimeDiff(rt.placeHeldAt, now) > PlaceHeldWindowMs)
                    rt.placeHeldTimes = 0;
                rt.placeHeldAt = now ? now : 1;
                // The second time back the way it came (along its trail: two bots stood beside a camp of 12 murlocs and
                // of 19 Mirefin, every route's first node beyond the camp, and used their hearthstone); the third time,
                // or with no trail to walk, the hearthstone.
                // Among hostile creatures it does not wait there: the first time already back the way it came (a
                // level-58 warrior waited a minute beside the Mossflayers of the Undercroft, began the same travel again
                // and died with five on it, 2026-11-27).
                ++rt.placeHeldTimes;
                if (rt.placeHeldTimes == 1 && Escape::HostilesAround(player, 40.0f) >= 2 &&
                    StartTrailOut(player, rt, "what stands around it lets it go nowhere, and it does not wait among it"))
                {
                    rt.placeHeldTimes = 2; // (the next time the hearthstone)
                    return;
                }
                if (rt.placeHeldTimes >= 2)
                {
                    if (rt.placeHeldTimes == 2 && StartTrailOut(player, rt, "what stands around it lets it go nowhere"))
                        return;
                    if (HearthHome(player, rt, "what stands around it lets it go nowhere", StrandedHearthYards))
                        return;
                }
                BotTask hold;
                hold.kind = TaskKind::Hold;
                StartTask(player, rt, hold);
                rt.why = "Waiting for a way out of where it stands";
                rt.waitUntil = now + BoxedInWaitMs;
                return;
            }
            rt.unreachableZones[zone->zoneId] = now + UnreachableZoneMs;
            rt.zoneChosenAt = 0;
            LOG_INFO(ActivityLog, "activity: {} began its travel to {} {} times within {} min without getting nearer ({:.0f} yd "
                "left): the zone waits for {} min", bot.name, zone->name, uint32(TravelRestartLimit) + 1,
                getMSTimeDiff(rt.zoneTravelAt, now) / (MINUTE * IN_MILLISECONDS) + 1, left, UnreachableZoneMs / (MINUTE * IN_MILLISECONDS));
            return;
        }
        StartTask(player, rt, task);
        StepTask(guild, bot, player, rt);
        return;
    }

    CareForQuestLog(bot, player, rt, zone->zoneId);
    if (PlanQuests(bot, player, rt, zone->zoneId))
        return;
    // No quest work in the zone right now: the hand-ins that wait outside it come before grinding.
    if (PlanErrand(guild, bot, player, rt, zone->zoneId, true, true))
        return;
    // At the level cap grinding earns no experience: a bot that has no quest left waits at the inn (until professions,
    // dungeons and raids give a capped bot something to do) and looks at the zones again every few minutes.
    if (IsAtLevelCap(player))
    {
        // The guild's ammunition first when an engineer could make what runs low (the owner, 2026-11-22: [quote left out]): at the cap its professions are mostly done
        // and it comes to a hub seldom, so the profession mode makes it (WorkOnProfessions) and hands back to the
        // autopilot when there is nothing left.
        if (bot.order.type == OrderType::Auto && player->HasSkill(SKILL_ENGINEERING) && now >= rt.nextAmmoLook &&
            !AmmoClaimedByOther(guild, bot.guid, std::max<uint32>(now, 1)))
        {
            SlowLook const timed("the guild's ammunition", bot.name);
            uint32 const stock = uint32(sBotSettings.Get(player, Setting::ProfessionsAmmoStock));
            // (Below three quarters of the stock, as the consumables below: the profession mode makes it up to the stock.)
            uint32 const below = stock ? std::max<uint32>(1, stock * 3 / 4) : 0;
            std::vector<Player*> const shooters = GunHunters(guild);
            std::string unused;
            if (below && !shooters.empty() && sProfessions.AmmoForGuild(player, shooters, below, AmmoCapBatch, unused, nullptr, false))
            {
                LOG_INFO(ActivityLog, "activity: {} is level {} (the level cap): the ammunition of the guild's hunters runs low "
                    "in the guild storage (below {} of a kind) and it can make more: its order is the profession mode", bot.name,
                    uint32(level), below);
                RecordEvent(guild, "ZONE_DONE", bot.guid, Acore::StringFormat("{} makes ammunition for the guild's hunters now.",
                    bot.name), level, player->GetZoneId());
                capCraftRetry.erase(bot.guid);
                ammoMaker[guild.ownerAccount] = { bot.guid, std::max<uint32>(now, 1) + AmmoClaimMs };
                ClearTask(rt, 0, "to the guild's ammunition");
                bot.order.type = OrderType::Craft;
                bot.order.zoneId = 0;
                sGuildAdventureMgr.SaveOrder(bot);
                OnOrderChanged(guild, bot);
                return;
            }
            rt.nextAmmoLook = now + AmmoLookMs;
        }
        // The guild's endgame consumables when it can make what runs low (the owner, 2026-11-25: [quote left out]): at the cap it comes to a hub
        // seldom, so the profession mode makes them (WorkOnProfessions) and hands back to the autopilot when there is
        // nothing left. One crafter of a guild at a time.
        if (bot.order.type == OrderType::Auto && now >= rt.nextConsumableLook && sStorageService.GetOwnerOf(player) &&
            !ConsumablesClaimedByOther(guild, bot.guid, std::max<uint32>(now, 1)))
        {
            SlowLook const timed("the guild's consumables", bot.name);
            uint32 const stock = uint32(sBotSettings.Get(player, Setting::ProfessionsConsumableStock));
            // Only below three quarters of the stock (the profession mode then makes it up to the stock): a stock of 19 of
            // 20 sent a warrior on a walk of 726 yd past camps to a quiet place for one weightstone (2026-11-27).
            uint32 const below = stock ? std::max<uint32>(1, stock * 3 / 4) : 0;
            uint8 const top = GuildTopLevel(guild);
            std::unordered_set<uint32> const skip = ConsumableSkips(player, rt, sStorageService.GetOwnerOf(player), std::max<uint32>(now, 1));
            GearStep step;
            std::string what;
            if (below && sProfessions.ConsumablesForGuild(player, top, below, ConsumableCapBatch, what, step, false, &skip))
            {
                LOG_INFO(ActivityLog, "activity: {} is level {} (the level cap): the guild storage holds fewer than {} of {} for the "
                    "guild's dungeon runs and its recipes make it: its order is the profession mode", bot.name, uint32(level), below, what);
                RecordEvent(guild, "ZONE_DONE", bot.guid, Acore::StringFormat("{} makes consumables for the guild's dungeon runs now ({}).",
                    bot.name, what), level, player->GetZoneId());
                capCraftRetry.erase(bot.guid);
                rt.capConsumable = step.gain;
                rt.capConsumableHad = step.gain ? sStorageService.GetAmount(sStorageService.GetOwnerOf(player), step.gain, 0) : 0;
                consumableMaker[guild.ownerAccount] = { bot.guid, std::max<uint32>(now, 1) + ConsumableClaimMs };
                ClearTask(rt, 0, "to the guild's consumables");
                bot.order.type = OrderType::Craft;
                bot.order.zoneId = 0;
                sGuildAdventureMgr.SaveOrder(bot);
                OnOrderChanged(guild, bot);
                return;
            }
            rt.nextConsumableLook = now + ConsumableLookMs;
        }
        // Raw materials of the guild storage processed likewise (the owner, 2026-11-25: ore into bars above 1000, processed
        // materials on stock): the profession mode walks to the forge.
        if (bot.order.type == OrderType::Auto && now >= rt.nextProcessLook && sStorageService.GetOwnerOf(player) &&
            !ProcessClaimedByOther(guild, bot.guid, now))
        {
            SlowLook const timed("the guild's raw materials and lockboxes", bot.name);
            uint32 const above = uint32(sBotSettings.Get(player, Setting::ProfessionsProcessAbove));
            uint32 const stock = uint32(sBotSettings.Get(player, Setting::ProfessionsProcessStock));
            GearStep step;
            std::string what;
            // (And a blacksmith's skeleton keys for the storage's lockboxes: KeysForGuild.)
            bool const process = above && sProfessions.ProcessForGuild(player, above, stock, ConsumableCapBatch, what, step, false);
            if (process || sProfessions.KeysForGuild(player, ConsumableCapBatch, what, step, false))
            {
                if (process)
                    LOG_INFO(ActivityLog, "activity: {} is level {} (the level cap): the guild storage holds more than {} of what its recipes "
                        "make {} of: its order is the profession mode", bot.name, uint32(level), above, what);
                else
                    LOG_INFO(ActivityLog, "activity: {} is level {} (the level cap): the guild storage holds lockboxes that its {} would "
                        "open: its order is the profession mode", bot.name, uint32(level), what);
                RecordEvent(guild, "ZONE_DONE", bot.guid, Acore::StringFormat("{} processes the guild's raw materials now ({}).",
                    bot.name, what), level, player->GetZoneId());
                processMaker[guild.ownerAccount] = { bot.guid, std::max<uint32>(now, 1) + ProcessClaimMs };
                capCraftRetry.erase(bot.guid);
                ClearTask(rt, 0, "to the guild's materials");
                bot.order.type = OrderType::Craft;
                bot.order.zoneId = 0;
                sGuildAdventureMgr.SaveOrder(bot);
                OnOrderChanged(guild, bot);
                return;
            }
            rt.nextProcessLook = now + ProcessLookMs;
        }
        // The enchantments for the guild's gear likewise (the owner, 2026-11-25): an enchanter at the cap writes them on
        // vellums in the profession mode (WorkOnProfessions), which hands back to the autopilot when there is none left.
        if (bot.order.type == OrderType::Auto && now >= rt.nextScrollLook && player->HasSkill(SKILL_ENCHANTING) &&
            sStorageService.GetOwnerOf(player) && sBotSettings.IsOn(player, Setting::ProfessionsEnchantScrolls))
        {
            SlowLook const timed("enchantments for the guild's gear", bot.name);
            std::string what;
            uint32 lack = 0;
            if (sProfessions.EnchantScrollForGuild(player, GuildEnchantWearers(guild), what, lack, false))
            {
                LOG_INFO(ActivityLog, "activity: {} is level {} (the level cap): it can write {} on a vellum for the guild: its order "
                    "is the profession mode", bot.name, uint32(level), what);
                RecordEvent(guild, "ZONE_DONE", bot.guid, Acore::StringFormat("{} enchants scrolls for the guild's gear now ({}).",
                    bot.name, what), level, player->GetZoneId());
                capCraftRetry.erase(bot.guid);
                ClearTask(rt, 0, "to the guild's enchantments");
                bot.order.type = OrderType::Craft;
                bot.order.zoneId = 0;
                sGuildAdventureMgr.SaveOrder(bot);
                OnOrderChanged(guild, bot);
                return;
            }
            rt.nextScrollLook = now + ScrollLookMs;
        }
        // First its professions (the profession mode, a real order the owner sees), then grinding creatures of level 50
        // to 60 (CapProfessionWorkLeft above); waiting at the inn only when neither is to be had.
        if (bot.order.type == OrderType::Auto && CapProfessionWorkLeft(bot, player))
        {
            auto retry = capCraftRetry.find(bot.guid);
            if (retry == capCraftRetry.end() || static_cast<int32>(now - retry->second) >= 0)
            {
                std::string skills;
                for (std::size_t i = 0; i < 2; ++i)
                    if (uint32 const skill = bot.professions.primary[i])
                        if (SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill))
                            skills += Acore::StringFormat("{}{} {}", skills.empty() ? "" : ", ", line->name[0],
                                player->HasSkill(skill) ? Acore::StringFormat("{}", player->GetPureSkillValue(skill)) : std::string("not learned"));
                LOG_INFO(ActivityLog, "activity: {} is level {} (the level cap) and has no quest left in {}: it works on its professions "
                    "until they are done ({}): its order is the profession mode", bot.name, uint32(level), zone->name, skills);
                RecordEvent(guild, "ZONE_DONE", bot.guid, Acore::StringFormat("{} has no quest left at level {} and works on its "
                    "professions now ({}).", bot.name, uint32(level), skills), level, player->GetZoneId());
                capCraftRetry.erase(bot.guid);
                ClearTask(rt, 0, "to its professions");
                bot.order.type = OrderType::Craft;
                bot.order.zoneId = 0;
                sGuildAdventureMgr.SaveOrder(bot);
                OnOrderChanged(guild, bot);
                return;
            }
        }
        if (PlanGrind(player, rt, zone->zoneId, std::max<int32>(1, int32(level) - CapGrindLevelsBelow), CapGrindYards))
        {
            rt.why = Acore::StringFormat("Level {} and no quests left: grinding in {} ({}){}", uint32(level), zone->name, rt.task.targetName,
                CapProfessionWorkLeft(bot, player) ? ", its professions again later" : "");
            return;
        }
        rt.capIdleUntil = now + CapIdleRecheckMs;
        LOG_INFO(ActivityLog, "activity: {} is level {} (the level cap) and has no quest left in {}: waits at the inn", bot.name,
            uint32(level), zone->name);
        RunIdle(bot, player, rt);
        rt.why = Acore::StringFormat("Level {} and no quests left for it: {}", uint32(level), rt.why);
        return;
    }
    // Nothing it can do from here, and it stands where the risk model would not let it work (a travel ended inside a
    // camp and the last stretch to its target was refused): out to a safe place first. A level-27 warrior stood 192 yd
    // from the Dragonmaw catapult, in the camp, with no task until it was attacked.
    if (PlanRetreat(bot, player, rt))
        return;
    // What the zone had left was behind creatures too strong for the bot (its walks were refused): the autopilot
    // looks at the zones again before it grinds here; the givers it left alone do not count as quests of this zone now.
    if (bot.order.type == OrderType::Auto && rt.gateRecheck)
    {
        rt.gateRecheck = false;
        rt.zoneChosenAt = 0;
        // Once it has left, the zone is not chosen again for a while (its givers count again when their own half
        // hours are over, and the same walks would be refused again).
        rt.unreachableZones[zone->zoneId] = now + ZoneRestMs;
        LOG_INFO(ActivityLog, "activity: {} has nothing in {} it can safely get to at level {}: it looks at the zones again", bot.name,
            zone->name, uint32(level));
        // The second time within half an hour, its inn elsewhere: home by hearthstone, the place it has ways from. The
        // look at the zones keeps a bot in the zone when no other one has more, and the grind it turns to there then
        // leads into the same creatures. Thunderhoof, a level-13 druid of the test guild, went south from the Crossroads
        // on its quests in the Barrens and stood 45 minutes among creatures of level 16 to 19: every way north was
        // refused, it left five fights and kept away from a creature every few seconds, "nothing in The Barrens it can
        // safely get to" four times, and its hearthstone to the Crossroads, 1300 yards off, stayed unused (its ways
        // ended in packs and fights, not in the refusals the boxed-in rule counts).
        // (In the same zone: Thrakgar, a level-30 bot of the test guild, found nothing in Thousand Needles, travelled to
        // Ashenvale, found nothing there either 13 minutes later, and the hearthstone took it back to Freewind Post in
        // Thousand Needles, the zone it had just left, round 244.)
        // (Only the second check at least five minutes after the first and within 300 yd of where it was made: a bot that
        // moved on is not boxed in, and two checks a minute apart say the same thing. The rule gave 42 of the live
        // server's 70 hearthstones of 2026-11-26, the two checks one to fifteen minutes apart, most of them to Freewind
        // Post or Shadowprey Village inside or beside the zone; the hearthstone is for the utmost emergency, the owner, L32.)
        constexpr uint32 NothingSafeAgainMs = 30 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 NothingSafeMinGapMs = 5 * MINUTE * IN_MILLISECONDS;
        constexpr float NothingSafeSameYards = 300.0f;
        bool const sameZone = rt.nothingSafeAt && rt.nothingSafeZone == zone->zoneId;
        uint32 const since = sameZone ? getMSTimeDiff(rt.nothingSafeAt, now) : 0;
        bool const nearFirst = sameZone && player->GetMapId() == rt.nothingSafeMap &&
            Dist2d(player, rt.nothingSafeX, rt.nothingSafeY) <= NothingSafeSameYards;
        bool const again = sameZone && nearFirst && since >= NothingSafeMinGapMs && since <= NothingSafeAgainMs;
        // (A check too soon after the first, from the same place, leaves the first one standing.)
        if (!(sameZone && nearFirst && since < NothingSafeMinGapMs))
        {
            rt.nothingSafeAt = now;
            rt.nothingSafeZone = zone->zoneId;
            rt.nothingSafeMap = player->GetMapId();
            rt.nothingSafeX = player->GetPositionX();
            rt.nothingSafeY = player->GetPositionY();
        }
        // (Not at once: the look at the zones that follows may send it elsewhere by a way it can walk. Sunwhisper, a
        // level-42 priest of the test guild, found nothing in Tanaris from Wavestrider Beach twice, used its hearthstone to
        // Gadgetzan, 1300 yd off in the same zone, and picked Feralas a second later, a flight from Gadgetzan it could
        // have walked to, round 257. The stone waits three minutes, at the top of RunAdventure.)
        if (again && !rt.nothingSafeHearthAt)
        {
            rt.nothingSafeHearthAt = now ? now : 1;
            LOG_INFO(ActivityLog, "activity: {} found nothing in {} it can safely get to from here twice, {} min apart: it looks for "
                "a way out, the hearthstone if it is still here in 3 min", bot.name, zone->name, since / (MINUTE * IN_MILLISECONDS));
        }
        return;
    }
    // Not in a zone that has just killed the bot twice (OnDied), while it rests: the hearthstone takes the bot out when
    // its inn is not next door, else it waits where it is.
    bool const struck = rt.emptyZoneDeaths && rt.emptyZone == zone->zoneId && now < rt.emptyZoneUntil && player->GetZoneId() == zone->zoneId;
    if (struck)
    {
        if (HearthHome(player, rt, Acore::StringFormat("{} killed it twice and it has nothing else it can get to", zone->name),
            StrandedHearthYards))
            return;
        if (now >= rt.nextStruckNote)
        {
            rt.nextStruckNote = now + 5 * MINUTE * IN_MILLISECONDS;
            LOG_INFO(ActivityLog, "activity: {} does not grind in {}: it died there twice within a quarter of an hour, and no other "
                "zone has work it can get to; it waits ({} min left)", bot.name, zone->name,
                (rt.emptyZoneUntil - now) / (MINUTE * IN_MILLISECONDS) + 1);
        }
    }
    else if (PlanGrind(player, rt, zone->zoneId))
        return;

    // Nothing at all, and the autopilot's pick still says this zone: after a minute of it the zone counts as empty
    // for a quarter of an hour and the zones are looked at again (the pick stays only when no other zone has
    // anything either).
    if (bot.order.type == OrderType::Auto && ++rt.idleHolds >= IdleHoldLimit && !(rt.emptyZone == zone->zoneId && now < rt.emptyZoneUntil))
    {
        rt.emptyZone = zone->zoneId;
        rt.emptyZoneUntil = now + EmptyZoneMs;
        rt.emptyZoneDeaths = false;
        rt.zoneChosenAt = 0;
        LOG_INFO(ActivityLog, "activity: {} has had nothing to do in {} for {} s at level {}: the zone counts as empty for {} min, "
            "it looks at the zones again", bot.name, zone->name, uint32(rt.idleHolds) * HoldReplanMs / IN_MILLISECONDS, uint32(level),
            EmptyZoneMs / (MINUTE * IN_MILLISECONDS));
    }

    // Still nothing after two minutes of this, and every two minutes after: the bot looks for a way out by itself.
    // Home by hearthstone from anywhere but its inn; without it, what it waits with is dropped (the zones it found no
    // way into, the pier it was refused at), so the next look at the zones may take a flight from the master next to
    // it (FindFlight's way out). A level-45 warrior stood at Firewatch Ridge for nine minutes, 210 yd from Thorium
    // Point's flight master: its three quests there were too much for it, its grind spots too, the walk out of the
    // gorge was refused, and its hearthstone had four minutes left (the owner: [quote left out]).
    constexpr uint8 IdleWayOutHolds = 4; // holds of HoldReplanMs each
    if (bot.order.type == OrderType::Auto && rt.idleHolds && rt.idleHolds % IdleWayOutHolds == 0)
    {
        uint32 const idleMin = uint32(rt.idleHolds) * HoldReplanMs / (MINUTE * IN_MILLISECONDS);
        // (Not for an inn it can walk to: a warrior of 50 used its stone in Feralas for its inn 770 yd away and picked
        // Tanaris a second later, 2026-11-27.)
        if (HearthHome(player, rt, Acore::StringFormat("nothing it can do in {} for {} min", zone->name, idleMin), 1000.0f))
            return;
        LOG_INFO(ActivityLog, "activity: {} has had nothing it can do in {} for {} min and no hearthstone to use: what it waits with "
            "is dropped ({} zones), it looks at the zones again and takes a flight out when one is near", bot.name, zone->name,
            idleMin, rt.unreachableZones.size());
        rt.unreachableZones.clear();
        rt.seaClosedUntil = 0;
        rt.zoneChosenAt = 0;
        rt.nextCatchUpLook = 0;
        if (IsCutOff(player) && CutOffWayOut(player, rt, "has nothing to do and is cut off from every road"))
            return;
    }

    BotTask hold;
    hold.kind = TaskKind::Hold;
    hold.targetName = zone->name;
    StartTask(player, rt, hold);
    rt.why = Acore::StringFormat("Nothing left to do in {} at level {}{}", zone->name, uint32(level),
        struck ? ": it died there twice and waits before it fights there again" : "");
    rt.waitUntil = now + HoldReplanMs;
}

// ---------------------------------------------------------------------------------------------------------------
// Zones
// ---------------------------------------------------------------------------------------------------------------

uint32 OrderService::CountEligibleQuests(Player* player, uint32 zoneId, bool afar) const
{
    AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(player->GetGUID());
    GuildBot const* bot = guild ? guild->FindPlanned(player->GetGUID()) : nullptr;
    if (!bot)
        return 0;

    static BotRuntime const noRuntime{};
    auto runtime = _runtime.find(player->GetGUID());
    BotRuntime const& rt = runtime != _runtime.end() ? runtime->second : noRuntime;

    // Quests the bot can take, from a giver it has not set aside (one it could not get to, or whose way led past
    // creatures too strong for it): a zone whose quests all wait behind such ways has nothing for the bot right now.
    // The giver has to stand in the zone: a quest that leads into it from a city far away is not found by a bot that
    // went there for it (the Alliance's quests "of" the Hillsbrad Foothills below level 25 are such; a level-21
    // character moved there for them and found nothing to do but grind).
    // And only where PlanQuests would send the bot: not a giver or an ender at a place that is set aside, or among
    // creatures the bot cannot take. (A level-23 mage held in Duskwood for twenty minutes: the zone still counted
    // quests whose givers the planner would not offer, so the pick stayed and nothing was planned.)
    auto offered = [&](SpawnPoint const& spot)
    {
        return !rt.IsInAvoidedArea(spot.mapId, spot.x, spot.y, player->GetLevel()) &&
            IsPlaceUsable(player, rt, spot.mapId, spot.x, spot.y, spot.z);
    };
    // A zone across the sea is counted from afar (`afar`): a giver that stands in it, at a place the bot could use.
    // (It used to be taken for "enough quests" unseen: bots sailed over, found nothing they could do, and sailed back
    // for the zone they had come from, which was unseen again by then.)
    // And only quests whose work the walk rules let the bot get to (the owner, 2026-11-14: a way is refused from four levels
    // above the bot): the zone's hub to the nearest of its targets. (The guild leader went to the Blasted Lands at level
    // 51; the ways to every quest's creatures there lead past the Dreadmaul ogres, levels 58 to 60, and all were refused
    // one after the other, the zone counted 0, and its way out was refused too: the hearthstone.)
    // (With the groups on the way, as the walk gate counts them: Desolace counted six to eight quests for bots of level
    // 32 and 33 whose work lay behind pairs and camps of level-33 and -34 creatures on every way from Ghost Walker Post;
    // they went there and had every task refused, 25 such refusals for one bot in half an hour, round 256.)
    auto workReachable = [&](Quest const* quest) { return QuestWorkReachable(player, rt, zoneId, quest); };
    // From afar, and for a zone the bot does not stand in, the giver and the ender are judged by their way from the
    // zone's hub too: a bot at the level cap sailed to Silithus for "3 quests", a hand-in at the Bronzebeard Encampment
    // behind Hive'Regal's elites (level 65) among them, and found nothing it could do there; another flew there from
    // Dustwallow Marsh for "5", three of them behind the same elites, round 167. (In the zone it stands in the walk gate
    // judges the real way from where it is.)
    bool const judgeWays = afar || zoneId != player->GetZoneId();
    auto wayReachable = [&](SpawnPoint const& spot)
    {
        if (!judgeWays || !spot.spawnId)
            return true;
        return PlaceWayReachable(player, rt, zoneId, spot);
    };
    uint32 count = 0;
    for (uint32 questId : sWorldIndex.GetZoneQuests(zoneId))
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
            if (!rt.IsDangerous(questId, player->GetLevel()) && IsQuestPickable(*bot, player, quest) && workReachable(quest))
                if (std::optional<SpawnPoint> giver = afar ? SpawnInZone(sWorldIndex.GetQuestStarters(questId), player, zoneId, rt) :
                        NearestSpawn(sWorldIndex.GetQuestStarters(questId), player, zoneId, true, ZoneEntryReach, rt))
                    if (offered(*giver) && wayReachable(*giver))
                        ++count;
    if (afar)
    {
        // And the hand-ins the quest log holds for it.
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = player->GetQuestSlotQuestId(slot);
            if (!questId || player->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
                continue;
            if (std::optional<SpawnPoint> ender = SpawnInZone(sWorldIndex.GetQuestEnders(questId), player, zoneId, rt))
                if (offered(*ender) && wayReachable(*ender))
                    ++count;
        }
        return count;
    }

    // The work the quest log holds in the zone (turn-ins and open objectives the planner has a spot for): the zone is
    // not done while it is open. Only what PlanQuests would turn into a task counts: a quest in the log it cannot act on
    // would otherwise keep the bot grinding here until it out-levels the zone.
    std::vector<QuestCandidate> work;
    CollectLogTasks(*bot, player, rt, zoneId, work);
    std::unordered_set<uint32> quests;
    for (QuestCandidate const& candidate : work)
    {
        bool const atNpc = candidate.task.kind == TaskKind::Pickup || candidate.task.kind == TaskKind::TurnIn;
        if (atNpc && candidate.task.hasSpot && (!offered(candidate.task.spot) || !wayReachable(candidate.task.spot)))
            continue;
        if (!atNpc && judgeWays)
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(candidate.questId); quest && !workReachable(quest))
                continue;
        quests.insert(candidate.questId);
    }
    return count + uint32(quests.size());
}

uint8 OrderService::QuestWorkDanger(uint32 zoneId, Quest const* quest, TeamId team, uint8 level) const
{
    uint64 const key = (uint64(quest->GetQuestId()) << 16) | (uint64(team == TEAM_ALLIANCE ? 1 : 2) << 8) | uint64(level);
    if (auto known = _questWorkDanger.find(key); known != _questWorkDanger.end())
        return known->second;
    uint8 danger = 0;
    ZoneHub const* hub = sWorldIndex.GetHub(zoneId, team);
    if (hub)
    {
        // The places of its work: the creatures it asks to be killed, the objects it asks to be used, and the creatures
        // and objects the items it asks for come from. (Objects too: a quest whose item lies in a chest among the elites
        // of Hive'Regal counted as work a bot could reach, round 167.)
        std::vector<SpawnPoint> places;
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            if (quest->RequiredNpcOrGo[i] > 0)
                for (SpawnPoint const& spawn : sWorldIndex.GetCreatureSpawns(uint32(quest->RequiredNpcOrGo[i])))
                    places.push_back(spawn);
            else if (quest->RequiredNpcOrGo[i] < 0)
                for (SpawnPoint const& spawn : sWorldIndex.GetGameObjectSpawns(uint32(-quest->RequiredNpcOrGo[i])))
                    places.push_back(spawn);
        }
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredItemId[i])
                for (SpawnPoint const& spawn : sWorldIndex.GetItemSources(quest->RequiredItemId[i]))
                    places.push_back(spawn);
        std::vector<std::pair<float, SpawnPoint>> spots;
        for (SpawnPoint const& spawn : places)
            if (spawn.mapId == hub->mapId && sWorldIndex.GetSpawnZone(spawn) == zoneId)
                spots.emplace_back(Dist2d(spawn.x, spawn.y, hub->x, hub->y), spawn);
        // The nearest few: the bot works the spawns it can get to.
        constexpr std::size_t SpotsAsked = 4;
        std::sort(spots.begin(), spots.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        uint8 best = 255;
        for (std::size_t i = 0; i < spots.size() && i < SpotsAsked; ++i)
        {
            SpawnPoint const& spot = spots[i].second;
            RoadRoute const route = sRoadMap.FindRoute(hub->mapId, hub->x, hub->y, hub->z, spot.x, spot.y, spot.z, team, level);
            uint8 const way = route.found ? route.danger : sRoadMap.DangerAlong(hub->mapId, hub->x, hub->y, spot.x, spot.y, team);
            best = std::min(best, way);
        }
        if (!spots.empty())
            danger = best ? best : 1; // (0 would read as "not judged")
    }
    _questWorkDanger[key] = danger;
    return danger;
}

uint8 OrderService::PlaceWayDanger(uint32 zoneId, SpawnPoint const& spot, TeamId team, uint8 level) const
{
    auto const key = std::make_tuple(uint32(spot.spawnId), spot.gameObject, uint8(team == TEAM_ALLIANCE ? 1 : 2), level);
    if (auto known = _placeWayDanger.find(key); known != _placeWayDanger.end())
        return known->second;
    uint8 danger = 0;
    if (ZoneHub const* hub = sWorldIndex.GetHub(zoneId, team); hub && hub->mapId == spot.mapId)
    {
        RoadRoute const route = sRoadMap.FindRoute(hub->mapId, hub->x, hub->y, hub->z, spot.x, spot.y, spot.z, team, level);
        uint8 const way = route.found ? route.danger : sRoadMap.DangerAlong(hub->mapId, hub->x, hub->y, spot.x, spot.y, team);
        danger = way ? way : 1; // (0 would read as "not judged")
    }
    _placeWayDanger[key] = danger;
    return danger;
}

OrderService::HubWay OrderService::JudgeHubWay(uint32 zoneId, SpawnPoint const& spot, TeamId team, uint8 level) const
{
    HubWay way;
    ZoneHub const* hub = sWorldIndex.GetHub(zoneId, team);
    if (!hub || hub->mapId != spot.mapId)
        return way;
    RoadRoute const route = sRoadMap.FindRoute(hub->mapId, hub->x, hub->y, hub->z, spot.x, spot.y, spot.z, team, level);
    uint8 const danger = route.found ? route.danger : sRoadMap.DangerAlong(hub->mapId, hub->x, hub->y, spot.x, spot.y, team);
    way.danger = danger ? danger : 1; // (0 would read as "not judged")
    if (route.found)
    {
        way.crowd = route.crowd;
        way.crowdLevel = route.crowdLevel;
    }
    // Around every group: what is left on it is what no way avoids.
    RoadRoute const around = sRoadMap.FindRoute(hub->mapId, hub->x, hub->y, hub->z, spot.x, spot.y, spot.z, team, level, {}, {},
        [](uint8, uint8) { return false; });
    if (around.found)
    {
        way.aroundFound = true;
        way.aroundDanger = around.danger ? around.danger : 1;
        way.aroundCrowd = around.crowd;
        way.aroundCrowdLevel = around.crowdLevel;
        way.aroundCamp = around.camp;
    }
    return way;
}

bool OrderService::HubWayPasses(Player const* player, BotRuntime const& rt, HubWay const& way) const
{
    if (!way.danger)
        return true;
    int32 const level = int32(player->GetLevel());
    int32 const margin = int32(sBotSettings.Get(player, Setting::WalksMaxLevelsAbove));
    auto dangerOk = [&](uint8 danger) { return danger < RoadMap::TownDanger && int32(danger) - level <= margin; };
    // As the walk gate refuses a way (LeaveDangerousWalk): a group it cannot take, a camp it cannot take three of.
    constexpr uint32 CampFoesAtOnce = 3;
    auto groupOk = [&](uint8 crowd, uint8 crowdLevel, bool camp)
    {
        if (camp)
            return Risk::CanFight(player, rt, crowdLevel, CampFoesAtOnce);
        return !RoadMap::GroupCounts(crowd, crowdLevel, level) || Risk::CanFight(player, rt, crowdLevel, crowd);
    };
    if (dangerOk(way.danger) && groupOk(way.crowd, way.crowdLevel, false))
        return true;
    return way.aroundFound && dangerOk(way.aroundDanger) && groupOk(way.aroundCrowd, way.aroundCrowdLevel, way.aroundCamp);
}

bool OrderService::QuestWorkReachable(Player const* player, BotRuntime const& rt, uint32 zoneId, Quest const* quest) const
{
    TeamId const team = player->GetTeamId();
    uint8 const level = player->GetLevel();
    uint64 const key = (uint64(quest->GetQuestId()) << 16) | (uint64(team == TEAM_ALLIANCE ? 1 : 2) << 8) | uint64(level);
    auto known = _questWorkWays.find(key);
    if (known == _questWorkWays.end())
    {
        std::vector<HubWay> ways;
        if (ZoneHub const* hub = sWorldIndex.GetHub(zoneId, team))
        {
            // The places of its work, as QuestWorkDanger finds them: the nearest few to the zone's hub.
            std::vector<SpawnPoint> places;
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                if (quest->RequiredNpcOrGo[i] > 0)
                    for (SpawnPoint const& spawn : sWorldIndex.GetCreatureSpawns(uint32(quest->RequiredNpcOrGo[i])))
                        places.push_back(spawn);
                else if (quest->RequiredNpcOrGo[i] < 0)
                    for (SpawnPoint const& spawn : sWorldIndex.GetGameObjectSpawns(uint32(-quest->RequiredNpcOrGo[i])))
                        places.push_back(spawn);
            }
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i])
                    for (SpawnPoint const& spawn : sWorldIndex.GetItemSources(quest->RequiredItemId[i]))
                        places.push_back(spawn);
            std::vector<std::pair<float, SpawnPoint>> spots;
            for (SpawnPoint const& spawn : places)
                if (spawn.mapId == hub->mapId && sWorldIndex.GetSpawnZone(spawn) == zoneId)
                    spots.emplace_back(Dist2d(spawn.x, spawn.y, hub->x, hub->y), spawn);
            constexpr std::size_t SpotsAsked = 4;
            std::sort(spots.begin(), spots.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
            for (std::size_t i = 0; i < spots.size() && i < SpotsAsked; ++i)
                ways.push_back(JudgeHubWay(zoneId, spots[i].second, team, level));
        }
        known = _questWorkWays.emplace(key, std::move(ways)).first;
    }
    if (known->second.empty())
        return true; // nothing of its work lies in the zone: not judged
    for (HubWay const& way : known->second)
        if (HubWayPasses(player, rt, way))
            return true;
    return false;
}

bool OrderService::PlaceWayReachable(Player const* player, BotRuntime const& rt, uint32 zoneId, SpawnPoint const& spot) const
{
    TeamId const team = player->GetTeamId();
    uint8 const level = player->GetLevel();
    auto const key = std::make_tuple(uint32(spot.spawnId), spot.gameObject, uint8(team == TEAM_ALLIANCE ? 1 : 2), level);
    auto known = _placeWays.find(key);
    if (known == _placeWays.end())
        known = _placeWays.emplace(key, JudgeHubWay(zoneId, spot, team, level)).first;
    return HubWayPasses(player, rt, known->second);
}

uint32 OrderService::ChooseZone(AdventureGuild const& guild, GuildBot const& bot, Player* player, uint32 exclude) const
{
    Perf::Get().zonePicks.fetch_add(1, std::memory_order_relaxed);
    uint8 const level = player->GetLevel();
    Region const region = GetRegion(player);
    uint32 const current = player->GetZoneId();
    auto runtime = _runtime.find(bot.guid);
    BotRuntime const* rt = runtime != _runtime.end() ? &runtime->second : nullptr;

    // Every zone that fits the level, the team, the order's limits and can be reached; with its eligible quests (across
    // the sea counted from afar) and the way there (straight line to its hub plus a penalty per ship).
    struct Candidate
    {
        uint32 zoneId = 0;
        uint32 quests = 0;
        float cost = 0.0f;
        bool early = false;     // the bot is in the first levels of the zone's range: most of what lives there is above it
        bool sea = false;       // a ship or zeppelin lies between the bot and the zone
        uint32 deaths = 0;      // guild characters of the bot's level - 1 and above that died there in the last hours
        bool hot = false;       // ... HotZoneDeaths of them or more: the zone comes after the others
    };
    // A bot that has gone ashore stays on that side for an hour, whatever the other side seems to offer.
    bool const crossedLately = rt && rt->CrossedLately();
    // Nor does it look across the sea while the way to the ship is refused from where it stands (every zone over there
    // leads over the same pier: three zones were picked and their voyages refused within ten seconds).
    bool const seaClosed = rt && rt->SeaClosed(getMSTime(), player->GetMapId(), player->GetPositionX(), player->GetPositionY());
    std::string noWayOn; // zones across the sea left out: no way on from the landing
    std::vector<Candidate> candidates;
    for (ZoneTemplate const& zone : sGuildAdventureMgr.GetZones())
    {
        if (zone.zoneId == exclude || !zone.AllowsTeam(guild.team) || !zone.AllowsOrder(OrderType::LevelZone))
            continue;
        if (rt && zone.zoneId != current && rt->IsZoneUnreachable(zone.zoneId))
            continue;
        if (level < zone.minLevel || level > zone.maxLevel)
            continue;
        int32 const legs = sWorldIndex.CountTravelLegs(region, GetZoneRegion(zone.continent, zone.zoneId, guild.team), guild.team);
        if (legs < 0)
            continue;
        if (bot.order.type == OrderType::Auto && bot.order.continent >= 0 && zone.continent != uint16(bot.order.continent))
            continue;
        ZoneHub const* hub = sWorldIndex.GetHub(zone.zoneId, guild.team);
        if (!hub)
            continue;

        Candidate candidate;
        candidate.zoneId = zone.zoneId;
        candidate.sea = legs != 0;
        if (candidate.sea && seaClosed)
            continue;
        // (Not across the sea to a landing from which the zone is out of the bot's reach.)
        if (candidate.sea && legs == 1 && rt)
        {
            TravelLink const* link = sWorldIndex.NextTravelLink(region, GetZoneRegion(zone.continent, zone.zoneId, guild.team), guild.team,
                player->GetMapId(), player->GetPositionX(), player->GetPositionY(), hub->mapId, hub->x, hub->y);
            if (link && !ReachableFromLanding(player, *rt, *link, zone))
            {
                noWayOn += Acore::StringFormat("{}{}", noWayOn.empty() ? "" : ", ", zone.name);
                continue;
            }
        }
        candidate.quests = candidate.sea && crossedLately ? 0 : CountEligibleQuests(player, zone.zoneId, candidate.sea);
        float const distance = (legs == 0 && hub->mapId == player->GetMapId()) ? Dist2d(player, hub->x, hub->y) : 0.0f;
        candidate.cost = distance + float(legs) * VoyageLegCost;
        candidate.early = level < zone.minLevel + ZoneComfortLevels;
        candidate.deaths = CountGuildDeaths(guild.ownerAccount, zone.zoneId, level > 1 ? uint8(level - 1) : level);
        candidate.hot = candidate.deaths >= HotZoneDeaths;
        // The zone the bot stands in and has just set aside itself (nothing in it it could safely get to, two deaths)
        // counts no quests: the count above judges the ways by their danger alone, not by the groups and spots its tasks
        // were refused for. (A level-14 druid at Camp Taurajo "looked at the zones again" three times in 45 minutes and
        // kept the Barrens each time, fighting level-18 creatures it left, until the count fell to 0, round 190.)
        if (rt && zone.zoneId == current && rt->IsZoneUnreachable(current))
            candidate.quests = 0;
        // The zone the bot stands in, with nothing it can do: remembered, so that its count from afar does not bring the
        // bot back. A zone across the sea counted from afar that it found so counts none for six hours at its level.
        // (Bots at the level cap swapped Silithus and Eastern Plaguelands every two or three hours: each counted 0 where
        // it stood and 3 to 5 across the sea, from the hand-ins and givers it could not get to once there, round 159.)
        if (rt && zone.zoneId == current && !candidate.sea && candidate.quests == 0)
        {
            uint32 const nowMs = getMSTime();
            auto& mark = rt->afarEmpty[zone.zoneId];
            if (mark.first <= nowMs || mark.second != level)
                LOG_INFO(ActivityLog, "activity: {} (level {}) has nothing it can do in {}, where it stands: from afar that zone "
                    "counts no quests for it for {} h", bot.name, uint32(level), zone.name, AfarEmptyMs / (HOUR * IN_MILLISECONDS));
            mark = { std::max<uint32>(nowMs + AfarEmptyMs, 1), level };
        }
        // (Any zone it does not stand in, not only one across the sea: Zulkash (43) found nothing in Feralas, went to
        // Tanaris, found nothing there either and picked Feralas again 19 minutes later with 4 quests, round 257.)
        if (rt && zone.zoneId != current && candidate.quests)
        {
            auto const mark = rt->afarEmpty.find(zone.zoneId);
            if (mark != rt->afarEmpty.end() && mark->second.first > getMSTime() && mark->second.second == level)
                candidate.quests = 0;
        }
        // (A zone the bot has just held in with nothing to do, or died in twice, counts no quests for a while.)
        if (rt && rt->emptyZone == zone.zoneId && getMSTime() < rt->emptyZoneUntil)
            candidate.quests = 0;
        candidates.push_back(candidate);
    }
    if (candidates.empty())
        return 0;

    // Stay where the bot is while that zone fits its level and still has work: bots level zone by zone like players,
    // instead of running across the continent for a few more quests.
    auto here = std::find_if(candidates.begin(), candidates.end(), [current](Candidate const& c) { return c.zoneId == current; });
    // Not in a zone that keeps killing the guild's characters of its level while another zone on its side, one that
    // does not, has enough quests for it: the bot levels there first and comes back stronger.
    bool leavesHot = false;
    if (here != candidates.end() && here->hot)
        for (Candidate const& candidate : candidates)
            leavesHot = leavesHot || (!candidate.hot && !candidate.sea && candidate.zoneId != current && candidate.quests >= MinQuestsToMove);
    if (here != candidates.end() && here->quests > 0 && !leavesHot)
        return current;

    // Otherwise the nearest zone that has enough quests for the bot, however far (the owner, 2026-11-12: grinding only when
    // no quests for its level are to be had, which with double experience should never be so; a level-21 character
    // had moved from the Wetlands to the nearer Hillsbrad Foothills, where the Alliance's quests begin at 25, and ground
    // bears there until it died three times). A zone the bot is well into the level range of comes before one it has
    // just grown into (a little below the bot is where it survives: its gear is what quests gave it).
    // More quests weigh against a longer way (a zone with a dozen quests twice as far away as one with three).
    auto score = [](Candidate const& candidate)
    {
        return candidate.cost / (1.0f + float(std::min<uint32>(candidate.quests, 12)) / 4.0f);
    };
    // The bot's own side of the sea first, whatever a zone across it counts: a crossing takes minutes, and what the
    // zone over there is like for the bot only shows when it stands in it. Across the sea only for a zone with more
    // quests (MinQuestsToCross), when no zone on this side has enough.
    Candidate const* best = nullptr;
    for (bool sea : { false, true })
    {
        for (bool early : { false, true })
        {
            for (Candidate const& candidate : candidates)
            {
                // (A zone that keeps killing the guild comes after the others, like one the bot has just grown into.)
                if (candidate.sea != sea || (candidate.early || candidate.hot) != early ||
                    candidate.quests < (sea ? MinQuestsToCross : MinQuestsToMove))
                    continue;
                if (!best || score(candidate) < score(*best))
                    best = &candidate;
            }
            if (best)
                break;
        }
        if (best)
            break;
    }
    // None has enough: the bot stays where it is and grinds near it until a level opens quests again (the owner,
    // 2026-11-12: grinding when no quests for its level are to be had in its zone, about its level and near). Not a
    // journey for one or two quests: live round 51 had a level-27 bot travel 6700 yd for two, after three zone
    // changes in a quarter of an hour, and travels were a fifth of the old bots' time. Only a bot that found nothing
    // at all to do where it is (no quest, nothing to grind: its zone counts as empty) goes for the zone on its side
    // with the most quests, however few. Out of every zone's range, the nearest zone, on this side if there is one.
    // (And with no zone on its side left that counts a quest, across the sea for one worth a move: Morwenna, a level-30
    // Horde character in Hillsbrad, counted 0 in every zone of the Eastern Kingdoms but Stranglethorn Vale's 1, whose way
    // was refused, went home by hearthstone and ground on with Thousand Needles at 5 across the sea, 2026-11-26.)
    if (!best && rt && rt->emptyZone == current && getMSTime() < rt->emptyZoneUntil)
        for (bool sea : { false, true })
        {
            for (Candidate const& candidate : candidates)
                if (candidate.sea == sea && candidate.quests >= (sea ? MinQuestsToMove : 1u) && candidate.zoneId != current &&
                    (!best || candidate.quests > best->quests || (candidate.quests == best->quests && candidate.cost < best->cost)))
                    best = &candidate;
            if (best)
                break;
        }
    // A bot at the level cap does not grind: in a zone with no quest for it, staying is waiting at the inn for good (a
    // level-60 death knight in Silithus, 0 quests, kept it over Eastern Plaguelands with 7 across the sea and Winterspring
    // with 1, round 138). It goes for a zone with quests: on its side one that is worth a move, else across the sea one
    // that is worth a move (from afar), else on its side any with a quest.
    if (!best && IsAtLevelCap(player) && (here == candidates.end() || here->quests == 0))
    {
        auto most = [&](bool sea, uint32 atLeast)
        {
            Candidate const* found = nullptr;
            for (Candidate const& candidate : candidates)
                if (candidate.sea == sea && candidate.zoneId != current && candidate.quests >= atLeast &&
                    (!found || candidate.quests > found->quests || (candidate.quests == found->quests && candidate.cost < found->cost)))
                    found = &candidate;
            return found;
        };
        best = most(false, MinQuestsToMove);
        if (!best)
            best = most(true, MinQuestsToMove);
        if (!best)
            best = most(false, 1);
    }
    if (!best && here != candidates.end())
        best = &*here;
    // (A bot that stands in no zone of its level, where a flight, a ship or a rescue left it: a zone with quests before
    // one with none. On this side the one with the most, then one across the sea that is worth a move; a level-25
    // warrior in Dustwallow Marsh picked Thousand Needles, the nearest, with no quest for it, before Redridge with
    // seven.)
    if (!best)
        for (Candidate const& candidate : candidates)
            if (!candidate.sea && candidate.quests > 0 &&
                (!best || candidate.quests > best->quests || (candidate.quests == best->quests && candidate.cost < best->cost)))
                best = &candidate;
    if (!best)
        for (Candidate const& candidate : candidates)
            if (candidate.sea && candidate.quests >= MinQuestsToMove && (!best || candidate.quests > best->quests))
                best = &candidate;
    // (Never across the sea for a zone with no quest for it: a level-33 bot at Theramore, every zone of its side
    // written off, "picked" Alterac Mountains and Arathi Highlands with 0 quests and went for the ship.)
    for (bool sea : { false, true })
        if (!best)
            for (Candidate const& candidate : candidates)
                if (candidate.sea == sea && (!sea || candidate.quests > 0) && (!best || candidate.cost < best->cost))
                    best = &candidate;
    if (best && rt && best->zoneId != rt->targetZone)
    {
        std::string others;
        for (Candidate const& candidate : candidates)
            if (&candidate != best)
                others += Acore::StringFormat("{}{} {}", others.empty() ? "" : ", ", ZoneName(candidate.zoneId), candidate.quests);
        LOG_INFO(ActivityLog, "activity: {} (level {}) picks {} as its zone: {} quest{} to take or work on there, {:.0f} yd away{}{}{}{}",
            bot.name, uint32(level), ZoneName(best->zoneId), best->quests, best->quests == 1 ? "" : "s", best->cost,
            best->sea ? ", across the sea (no zone on its side has enough for it)" : "",
            others.empty() ? "" : " (the others: " + others + ")",
            crossedLately ? "; it went ashore less than an hour ago and stays on this side" :
                seaClosed ? "; the way to the ship was refused from here a moment ago, so nothing across the sea" :
                !noWayOn.empty() ? "; not across the sea for " + noWayOn + ": no way on from the landing" : "",
            leavesHot && best->zoneId != current ? Acore::StringFormat("; it leaves {}, where {} guild characters of level {} and above "
                "died in the last {} h", ZoneName(current), here->deaths, uint32(level > 1 ? level - 1 : level), HotZoneSeconds / HOUR) :
                std::string());
    }
    return best ? best->zoneId : 0;
}

void OrderService::CheckOutleveled(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(bot.order.zoneId);
    if (!zone || player->GetLevel() <= zone->maxLevel || rt.reportedOutleveled == zone->zoneId)
        return;

    rt.reportedOutleveled = zone->zoneId;
    RecordEvent(guild, "ZONE_DONE", bot.guid, Acore::StringFormat("{} out-levelled {}.", bot.name, zone->name),
        player->GetLevel(), zone->zoneId);

    if (!bot.autoAdvance)
        return;

    uint32 const next = ChooseZone(guild, bot, player, zone->zoneId);
    if (!next)
        return;

    bot.order.zoneId = next;
    bot.order.setAt = Now();
    sGuildAdventureMgr.SaveOrder(bot);
    PushBot(guild, bot.guid, false);
    RecordEvent(guild, "ORDER", bot.guid, Acore::StringFormat("{} moves on to {} (auto-advance).", bot.name, ZoneName(next)),
        player->GetLevel(), next);
    ClearTask(rt, 0, "auto-advance to the next zone");
}

// ---------------------------------------------------------------------------------------------------------------
// Planning
// ---------------------------------------------------------------------------------------------------------------

bool OrderService::ZoneEntry(GuildBot const& bot, Player* player, BotRuntime const& rt, ZoneTemplate const& zone, SpawnPoint& entry) const
{
    // The nearest giver, standing in the zone, of a quest of the zone the bot can take; first one of those the zone's
    // count counted (CountEligibleQuests): a giver at a place it may use, whose way from the zone's hub and whose work
    // the walk rules let it get to. (A level-32 priest in Stonetalon picked Thousand Needles for 9 quests and set off for
    // the nearest giver, Pao'ka Swiftmountain on Highperch behind level-39 creatures, which the count had left out: the
    // way and the flight were refused, the zone written off, and it went home by hearthstone, 2026-11-26.)
    bool found = false, foundGood = false;
    float best = 0.0f, bestGood = 0.0f;
    SpawnPoint good;
    for (uint32 questId : sWorldIndex.GetZoneQuests(zone.zoneId))
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest || rt.IsDangerous(questId, player->GetLevel()) || !IsQuestPickable(bot, player, quest))
            continue;
        std::optional<SpawnPoint> giver = NearestSpawn(sWorldIndex.GetQuestStarters(questId), player, zone.zoneId, true, ZoneEntryReach, rt);
        if (!giver)
            continue;
        float const distance = Dist2d(player, *giver);
        if (!found || distance < best)
        {
            found = true;
            best = distance;
            entry = *giver;
        }
        if (foundGood && distance >= bestGood)
            continue;
        if (rt.IsInAvoidedArea(giver->mapId, giver->x, giver->y, player->GetLevel()) ||
            !IsPlaceUsable(player, rt, giver->mapId, giver->x, giver->y, giver->z) ||
            !QuestWorkReachable(player, rt, zone.zoneId, quest) ||
            (giver->spawnId && !PlaceWayReachable(player, rt, zone.zoneId, *giver)))
            continue;
        foundGood = true;
        bestGood = distance;
        good = *giver;
    }
    if (foundGood)
        entry = good;
    return found;
}

bool OrderService::IsQuestPickable(GuildBot const& bot, Player* player, Quest const* quest) const
{
    uint32 const questId = quest->GetQuestId();
    auto block = bot.questBlocks.find(questId);
    if (block != bot.questBlocks.end() && block->second.blocked)
        return false;

    if (player->GetQuestStatus(questId) != QUEST_STATUS_NONE || player->GetQuestRewardStatus(questId))
        return false;
    if (sQuestSupport.WasDropped(player->GetGUID(), questId))
        return false;

    // The same rules NewRpg applies when it talks to the giver, so the bot never walks to a giver that will not hand
    // the quest out to it.
    if (!NewRpgHelpers::IsQuestWorthDoing(player, quest) || !NewRpgHelpers::IsQuestCapableDoing(player, quest))
        return false;

    return player->CanTakeQuest(quest, false) && player->CanAddQuest(quest, false);
}

// A visit to a poison vendor for a rogue's poisons (RoguePoisons.h): the trip is remembered, so that it is taken up again
// when something cuts it short; the visit sells and repairs too when that is due.
void OrderService::StartPoisonTrip(Player* player, BotRuntime& rt, ServicePoint const& vendor, bool sell, bool repair, char const* why)
{
    if (!rt.poisonTripSince)
        rt.poisonTripSince = getMSTime();
    rt.poisonVendor = vendor.spawn;
    LOG_INFO(ActivityLog, "activity: {} goes to a vendor ({}, {:.0f} yd): {}poisons below half their stock ({}){}",
        player->GetName(), vendor.name, Dist2d(player, vendor.spawn), why, Poisons::Describe(player),
        sell ? "; it sells there too" : "");
    BotTask task;
    task.kind = TaskKind::Vendor;
    task.sell = sell;
    task.repair = repair && vendor.npcFlags & UNIT_NPC_FLAG_REPAIR;
    task.hasSpot = true;
    task.spot = vendor.spawn;
    task.targetName = vendor.name;
    StartTask(player, rt, task);
    rt.why = Acore::StringFormat("Buying poisons at {}", vendor.name);
}

// A rogue that carries no poison at all and has a bare weapon (RoguePoisons.h): any stop in a town with a poison vendor,
// between two tasks, not only a hand-in at a hub (round 74: a rogue handed in, crafted and shopped for its profession
// in Lakeshire and Darkshire and went on with bare weapons for hours). The owner's rule holds: no trip from the field, only
// a vendor that stands in a town (an innkeeper or a flight master near it) within PoisonTownReach of the bot.
bool OrderService::PlanPoisonStop(Player* player, BotRuntime& rt)
{
    uint32 const now = getMSTime();
    // (A look again at once when the bot has moved since the last one: a rogue teleported into Darkshire handed in and
    // was paired with a quest partner before the 15 s since its look in Redridge were over.)
    bool const moved = rt.poisonLookAt.mapId != player->GetMapId() || Dist2d(player, rt.poisonLookAt) > PoisonLookMoveYards;
    if (now < rt.nextPoisonTopUp || (now < rt.nextPoisonLook && !moved) || player->IsInCombat() || player->IsInFlight() ||
        player->GetTransport() || !Poisons::Bare(player))
        return false;
    rt.nextPoisonLook = now + PoisonLookMs;
    rt.poisonLookAt.mapId = player->GetMapId();
    rt.poisonLookAt.x = player->GetPositionX();
    rt.poisonLookAt.y = player->GetPositionY();
    auto inTown = [](ServicePoint const& vendor)
    {
        for (ServiceType type : { ServiceType::Innkeeper, ServiceType::FlightMaster })
            for (ServicePoint const& service : sWorldIndex.GetServices(type))
                if (service.spawn.mapId == vendor.spawn.mapId &&
                    (service.spawn.x - vendor.spawn.x) * (service.spawn.x - vendor.spawn.x) +
                    (service.spawn.y - vendor.spawn.y) * (service.spawn.y - vendor.spawn.y) <= PoisonTownYards * PoisonTownYards)
                    return true;
        return false;
    };
    ServicePoint const* vendor = NearestService(ServiceType::Vendor, player, rt, 0, [&](ServicePoint const& service)
        { return Dist2d(player, service.spawn) <= PoisonTownReach && Poisons::VendorSells(service.spawn.entry, player) && inTown(service); });
    if (!vendor)
        return false;
    StartPoisonTrip(player, rt, *vendor, false, false, "it carries no poison and a weapon is bare, a poison vendor stands in this town; ");
    return true;
}

bool OrderService::PlanUpkeep(GuildBot const& /*bot*/, Player* player, BotRuntime& rt)
{
    uint32 const now = getMSTime();
    if (now < rt.nextUpkeepCheck)
        return false;
    rt.nextUpkeepCheck = now + UpkeepCheckMs;

    // When a bot goes to a vendor (the owner, 2026-11-12: [quote left out]):
    // at a hub right after a hand-in, or at once when it is urgent (GuildAdventure.Vendor.Trips).
    Settings const settings = sBotSettings.For(player);
    uint32 const trips = settings.vendorTrips;
    // A hunter's ammunition (the owner, 2026-11-13: [quote left out]; Ammunition.h): below GuildAdventure.Ammo.RestockBelow rounds it goes to
    // a vendor that sells its ammunition from wherever it is (noAmmo); a bundle or more below its stock it visits such
    // a vendor in reach of a hub it has come to or of a vendor trip (ammoWanted). Neither while the guild storage holds
    // ammunition for it: the supply step brings that within a minute.
    bool const shoots = player->getClass() == CLASS_HUNTER && Ammo::KindOf(player);
    uint32 const rounds = shoots ? Ammo::Count(player) : 0;
    bool const ammoShort = shoots && rounds + AmmoBundle <= Ammo::Stock(player) && !Ammo::InStorage(player);
    bool const noAmmo = ammoShort && rounds < settings.ammoRestockBelow && now >= rt.nextAmmoTrip;
    bool const ammoWanted = ammoShort && now >= rt.nextAmmoTopUp;
    // A squad member plans no upkeep of its own (it follows): its leader's vendor trip is the squad's. So a member with
    // full bags sends its leader, and at the visit the member's bags get the same care (two members of the guild carried
    // full bags for hours: no vendor trip since 08:23 and since 15:47).
    constexpr float SquadUpkeepYards = 100.0f;
    uint32 memberFree = std::numeric_limits<uint32>::max();
    for (ObjectGuid memberGuid : sSquadService.GetMembers(player->GetGUID()))
        if (Player* member = ObjectAccessor::FindConnectedPlayer(memberGuid); member && member->IsInWorld() &&
            member->GetMapId() == player->GetMapId() && Dist2d(player, member->GetPositionX(), member->GetPositionY()) <= SquadUpkeepYards)
            memberFree = std::min(memberFree, member->GetFreeInventorySpace());
    bool const memberFull = memberFree <= UrgentFreeSlots && now >= rt.nextMemberTrip;
    bool const bagsFull = player->GetFreeInventorySpace() <= UrgentFreeSlots || memberFull;
    bool const gearBreaking = MinDurabilityPercent(player) < UrgentRepairPercent;
    bool const urgent = bagsFull || gearBreaking || noAmmo;
    bool const atHub = trips == 0 && rt.handInAt && getMSTimeDiff(rt.handInAt, now) <= HubWindowMs;
    bool const mayTrip = trips == 2 || urgent || atHub;
    // At a hub: only a vendor that stands there.
    // (Urgent for gear or ammunition: any distance. For full bags alone: a vendor that is not far; a hunter walked
    // 944 yd to a clothier with full bags that the bag sweep emptied on the way.)
    auto inReach = [&](ServicePoint const* service)
    {
        if (!service || trips == 2 || gearBreaking || noAmmo)
            return service;
        // (With no free slot at all there is nothing to wait for: any distance, as for gear and ammunition. A bot at the
        // mouth of the Cliffspring river, 700 yd from a vendor, only set its spots aside.)
        if (bagsFull && (player->GetFreeInventorySpace() == 0 || memberFree == 0))
            return service;
        return Dist2d(player, service->spawn) <= (bagsFull ? FullBagsVendorDistance : HubDistance) ? service : nullptr;
    };

    bool const repair = mayTrip && now >= rt.nextRepair && MinDurabilityPercent(player) < RepairBelowPercent;
    bool const sell = mayTrip && now >= rt.nextSell && (player->GetFreeInventorySpace() <= SellAtFreeSlots ||
        (memberFree <= SellAtFreeSlots && now >= rt.nextMemberTrip) || noAmmo);
    if (memberFull)
        rt.memberTripFree = memberFree;
    // A top-up alone is worth a visit at a hub (or with "whenever due"), never a walk from the field.
    bool const topUp = ammoWanted && (atHub || trips == 2);
    // A rogue's poisons (RoguePoisons.h): at a hub after a hand-in (or with "whenever due"), a poison vendor that stands
    // there when it carries less than half its stock and the guild storage holds none for it (the owner's rule for
    // shopping: at a hub, no trips from the field). Before the ordinary vendor trip, which the visit takes along (it
    // sells there too): in round 73 a rogue with bare weapons handed in twice 66 yd from Malissa in Darkshire and went
    // to a vendor 171 yd away to sell, and the poisons never came. The purchase happens at the visit (Poisons::BuyAt),
    // and only the visit (or a hub without such a vendor) starts the pause before the next look. A trip that something
    // else cut short (a quest partner, a fight, a new task: a rogue went for Malissa 152 yd away, teamed up for a quest
    // 15 s later and never came back) is taken up again at the next plan while the bot is still near that vendor.
    bool const poisonTripOpen = rt.poisonTripSince && getMSTimeDiff(rt.poisonTripSince, now) < PoisonTripMs &&
        rt.poisonVendor.mapId == player->GetMapId() && Dist2d(player, rt.poisonVendor) <= PoisonTripReach;
    if (rt.poisonTripSince && !poisonTripOpen)
        rt.poisonTripSince = 0;
    if (!gearBreaking && (atHub || trips == 2 || poisonTripOpen) && now >= rt.nextPoisonTopUp && Poisons::WantsTopUp(player))
    {
        float const reach = poisonTripOpen ? PoisonTripReach : HubDistance;
        ServicePoint const* vendor = NearestService(ServiceType::Vendor, player, rt, 0, [&](ServicePoint const& service)
            { return Dist2d(player, service.spawn) <= reach && Poisons::VendorSells(service.spawn.entry, player); });
        if (!vendor)
        {
            rt.nextPoisonTopUp = now + PoisonTopUpGapMs;
            rt.poisonTripSince = 0;
        }
        else
        {
            StartPoisonTrip(player, rt, *vendor, sell, repair, poisonTripOpen ? "again (its trip was cut short), " :
                atHub ? "at a hub after a hand-in, " : "");
            return true;
        }
    }

    // (Nearly out of ammunition: the trip does not wait for the next sale to be due.)
    if (repair || sell || topUp || noAmmo)
    {
        auto sellsAmmo = [player](ServicePoint const& service) { return Ammo::VendorSells(service.spawn.entry, player) != 0; };
        ServicePoint const* vendor = repair ? inReach(NearestService(ServiceType::Repair, player, rt)) : nullptr;
        // Nearly out of ammunition: a vendor that sells some, wherever it stands.
        if (!vendor && noAmmo)
            vendor = NearestService(ServiceType::Vendor, player, rt, 0, sellsAmmo);
        // Below its stock: a vendor in reach that sells some comes before any other vendor.
        if (!vendor && ammoWanted)
            vendor = NearestService(ServiceType::Vendor, player, rt, 0, [&](ServicePoint const& service)
                { return Dist2d(player, service.spawn) <= settings.ammoTopUpYards && sellsAmmo(service); });
        if (!vendor && (repair || sell))
            vendor = inReach(NearestService(ServiceType::Vendor, player, rt));
        if (vendor)
        {
            // (A repairer without ammunition does not count as the trip for it: that follows.)
            bool const forAmmo = (noAmmo || ammoWanted) && sellsAmmo(*vendor);
            if (forAmmo && noAmmo)
                rt.nextAmmoTrip = now + AmmoTripGapMs;
            if (forAmmo)
                rt.nextAmmoTopUp = now + AmmoTopUpGapMs;
            LOG_INFO(ActivityLog, "activity: {} goes to a vendor ({}, {:.0f} yd): {}{}{}{}; {} free bag slots, gear at {}%{}",
                player->GetName(), vendor->name, Dist2d(player, vendor->spawn), atHub ? "at a hub after a hand-in" : "",
                memberFull ? " a squad member's bags full" : bagsFull ? " bags full" : "", gearBreaking ? " gear breaking" : "",
                !forAmmo ? "" : noAmmo ? " nearly out of ammunition" : " ammunition below its stock",
                player->GetFreeInventorySpace(), uint32(MinDurabilityPercent(player)),
                shoots ? Acore::StringFormat(", ammunition {} of {}", rounds, Ammo::Stock(player)) : std::string());
            BotTask task;
            task.kind = TaskKind::Vendor;
            task.repair = repair && vendor->npcFlags & UNIT_NPC_FLAG_REPAIR;
            task.sell = sell;
            task.hasSpot = true;
            task.spot = vendor->spawn;
            task.targetName = vendor->name;
            StartTask(player, rt, task);
            rt.why = Acore::StringFormat("{} at {}", repair ? "Repairing and selling" : forAmmo && (noAmmo || !sell) ?
                "Buying ammunition" : "Selling loot", vendor->name);
            return true;
        }
    }

    // Food and drink: a trip when the bot runs low, and a food vendor close by is taken along below half a stack.
    // Not before GuildAdventure.Provisions.MinLevel (the owner, 2026-11-12: [quote left out]; a level-3 leader had walked from Northshire to Goldshire
    // for it).
    // Shopping belongs to a hub (the owner, 2026-11-12: [quote left out]): a bot below half a stack buys when a vendor that sells what it lacks stands within
    // GuildAdventure.Provisions.HubYards of it. A walk to a vendor from the field when it is nearly out is an option
    // (GuildAdventure.Provisions.TripWhenLow, off: without provisions a rest just takes longer).
    uint32 const half = settings.provisionsStack / 2;
    uint32 const drinkHalf = settings.provisionsDrinkStack / 2;
    bool const low = NeedsFood(player, settings.provisionsRestockBelow) || NeedsDrink(player, settings.provisionsRestockBelow);
    bool const hubDue = now >= rt.nextNearbyRestock && (NeedsFood(player, half) || CarriesFewDrinks(player, drinkHalf));
    // A mana user without any drink is as badly off as a hunter without ammunition: every rest takes a minute and a
    // half instead of twenty seconds. It goes to a vendor that sells drink from wherever it is (not a mage: it
    // conjures its own in the rest).
    // No trip while the guild storage has drink for it: it takes a piece from there when it drinks (StorageSupply).
    // With none in its bags and none in the storage it goes at once and as far as it takes (the owner, 2026-11-14: [quote left out]; the
    // rest calls for it, StepRest). GuildAdventure.Provisions.TripWhenOutYards can bound the trip (0: no bound; it
    // was 600 after a priest had walked 1230 yards for drink, and a paladin in the middle of Dustwallow then had no
    // vendor in reach at all).
    bool outOfDrink = settings.provisionsTripWhenOut && player->getClass() != CLASS_MAGE && NeedsDrink(player, 1) &&
        now >= rt.nextRestock;
    if (outOfDrink && StorageSupply::HasDrink(player))
    {
        outOfDrink = false;
        rt.nextRestock = now + MINUTE * IN_MILLISECONDS;
    }
    // Three rests in a row without the food or drink they were for: the bot has none and passes no hub (a recruit, a
    // bot back from a long dungeon run). It goes to a vendor that sells what it lacks, wherever one stands, unless
    // the guild storage has it.
    constexpr uint8 UnfedRestsForTrip = 3;
    bool starving = rt.unfedRests >= UnfedRestsForTrip && now >= rt.nextRestock &&
        (NeedsFood(player, 1) || (player->getClass() != CLASS_MAGE && NeedsDrink(player, 1)));
    if (rt.unfedRests && !NeedsFood(player, 1) && !NeedsDrink(player, 1))
        rt.unfedRests = 0;
    if (starving && !NeedsFood(player, 1) && StorageSupply::HasDrink(player))
        starving = false;
    bool const lowTrip = (settings.provisionsTripWhenLow && low && now >= rt.nextRestock) || starving;
    bool const tripDue = lowTrip || outOfDrink;
    if (player->GetLevel() >= settings.provisionsMinLevel && player->GetMoney() >= settings.provisionsMinMoney && (hubDue || tripDue))
    {
        if (tripDue)
            rt.nextRestock = now + RestockGapMs;
        rt.nextNearbyRestock = now + NearbyRestockGapMs;
        // A vendor that sells what is missing (a butcher has no drink, an innkeeper usually both).
        bool const needFood = NeedsFood(player, half);
        bool const needDrink = CarriesFewDrinks(player, drinkHalf);
        float const reach = lowTrip ? MaxServiceDistance :
            outOfDrink ? (settings.provisionsTripWhenOutYards > 0.0f ?
                std::max(settings.provisionsTripWhenOutYards, settings.provisionsHubYards) : MaxServiceDistance) : settings.provisionsHubYards;
        auto sellsWhatIsMissing = [player, needFood, needDrink, reach](ServicePoint const& service)
        {
            return Dist2d(player, service.spawn) <= reach &&
                (!needFood || VendorSells(service.spawn.entry, FoodSpellCategory, player)) &&
                (!needDrink || VendorSells(service.spawn.entry, DrinkSpellCategory, player));
        };
        // Among all vendors, by what they sell: the "food vendor" flag is missing on many who sell bread and water
        // (Brother Danil in Northshire), and the nearest with the flag stood a zone further (Goldshire).
        ServicePoint const* vendor = NearestService(ServiceType::Vendor, player, rt, 0, sellsWhatIsMissing);
        // Nobody near sells both: one that sells the drink, else one that sells the food (a hub's baker and its
        // innkeeper are two people).
        if (!vendor && needFood && needDrink)
        {
            vendor = NearestService(ServiceType::Vendor, player, rt, 0, [player, reach](ServicePoint const& service)
            {
                return Dist2d(player, service.spawn) <= reach && VendorSells(service.spawn.entry, DrinkSpellCategory, player);
            });
            if (!vendor)
                vendor = NearestService(ServiceType::Vendor, player, rt, 0, [player, reach](ServicePoint const& service)
                {
                    return Dist2d(player, service.spawn) <= reach && VendorSells(service.spawn.entry, FoodSpellCategory, player);
                });
        }
        // (No vendor for a bot that is out of drink: it rests for its mana as before, StepRest.)
        if (!vendor && outOfDrink)
            rt.drinkTripUntil = 0;
        if (vendor && (outOfDrink || starving))
            LOG_INFO(ActivityLog, "activity: {} goes to a vendor ({}, {:.0f} yd): {}", player->GetName(), vendor->name,
                Dist2d(player, vendor->spawn), starving ? Acore::StringFormat("{} rests in a row without food or drink", uint32(rt.unfedRests)) :
                    std::string("out of drink"));
        if (vendor && starving)
            rt.unfedRests = 0;
        // A trip from the field (the option): low but not out only to a vendor that is not far, the quest work near
        // the bot comes first; out of food (or of drink, for a mana user) the trip is worth the way.
        constexpr float LowRestockDistance = 400.0f;
        bool const out = NeedsFood(player, 1) || NeedsDrink(player, 1);
        if (vendor && Dist2d(player, vendor->spawn) > std::max(settings.provisionsHubYards, out ? MaxServiceDistance : LowRestockDistance))
            vendor = nullptr;
        if (vendor)
        {
            BotTask task;
            task.kind = TaskKind::Vendor;
            task.restock = true;
            task.hasSpot = true;
            task.spot = vendor->spawn;
            task.targetName = vendor->name;
            StartTask(player, rt, task);
            rt.why = Acore::StringFormat("Buying food and drink at {}", vendor->name);
            // (Out of drink: no rest for mana on the way, the drink is at the vendor. StepRest.)
            rt.drinkTripUntil = outOfDrink ? std::max<uint32>(now + RestockGapMs, 1) : 0;
            return true;
        }
    }

    // Gear (GearUpkeep.h): a vendor near the bot sells something to wear that is an upgrade for it and within the
    // budget: the bot walks there and buys it. The vendor is chosen by the same risk rules as any other; nothing is
    // bought at a distance.
    // Looked for once a minute (a bot that hands a quest in at a town sees the town's vendors); after a trip not again
    // for five minutes, whatever came of it.
    constexpr uint32 GearLookGapMs = MINUTE * IN_MILLISECONDS;
    constexpr uint32 GearTripGapMs = 5 * MINUTE * IN_MILLISECONDS;
    if (settings.gearBuyEnable && settings.gearBuyNearYards > 0.0f && now >= rt.nextGearTrip)
    {
        rt.nextGearTrip = now + GearLookGapMs;
        float const nearYards = settings.gearBuyNearYards;
        auto sellsUpgrade = [player, nearYards](ServicePoint const& service)
        {
            return Dist2d(player, service.spawn) <= nearYards && Gear::SellsUpgrade(player, service.spawn.entry);
        };
        if (ServicePoint const* vendor = NearestService(ServiceType::Vendor, player, rt, 0, sellsUpgrade))
        {
            BotTask task;
            task.kind = TaskKind::Vendor;
            task.hasSpot = true;
            task.spot = vendor->spawn;
            task.targetName = vendor->name;
            StartTask(player, rt, task);
            rt.why = Acore::StringFormat("Buying better gear at {}", vendor->name);
            rt.nextGearTrip = now + GearTripGapMs;
            return true;
        }
    }
    // Recipes (the owner, 2026-11-13: [quote left out]): a vendor near the bot sells a recipe of one of its
    // professions that it can learn now and pay: the bot walks there, buys it and learns it. As with gear, nothing at
    // a distance; and at every vendor it visits anyway it buys what that one has (StepTask).
    if (float const recipeYards = float(sBotSettings.Get(player, Setting::ProfessionsRecipeYards)); recipeYards > 0.0f && now >= rt.nextRecipeTrip)
    {
        rt.nextRecipeTrip = now + GearLookGapMs;
        auto sellsRecipe = [player, recipeYards](ServicePoint const& service)
        {
            return Dist2d(player, service.spawn) <= recipeYards && Professions::SellsRecipe(player, service.spawn.entry);
        };
        if (ServicePoint const* vendor = NearestService(ServiceType::Vendor, player, rt, 0, sellsRecipe))
        {
            BotTask task;
            task.kind = TaskKind::Vendor;
            task.hasSpot = true;
            task.spot = vendor->spawn;
            task.targetName = vendor->name;
            StartTask(player, rt, task);
            rt.why = Acore::StringFormat("Buying a recipe for its profession at {}", vendor->name);
            rt.nextRecipeTrip = now + GearTripGapMs;
            return true;
        }
    }

    uint8 const level = player->GetLevel();
    if (level > rt.trainCheckedLevel)
    {
        rt.trainCheckedLevel = level;
        ServicePoint const* trainer = NearestService(ServiceType::ClassTrainer, player, rt, player->getClass());
        if (sBotSettings.IsOn(player, Setting::AutoLearnClassSpells))
        {
            // No trip: the spells come where the bot is (the nearest trainer of its class only lends its list).
            ServicePoint const* teacher = ClassTrainerForLearning(player);
            if (uint32 const learned = teacher ? LearnTrainerSpellsInPlace(TrainerOf(teacher), player) : 0)
                LOG_INFO("module", "mod-guild-adventure: {} learned {} class spell{} (level {}, no trainer trip)",
                    player->GetName(), learned, learned == 1 ? "" : "s", level);
            return false;
        }
        std::vector<Trainer::Spell const*> const spells = AffordableSpells(TrainerOf(trainer), player);
        if (trainer && !spells.empty())
        {
            BotTask task;
            task.kind = TaskKind::Train;
            task.hasSpot = true;
            task.spot = trainer->spawn;
            task.targetName = trainer->name;
            StartTask(player, rt, task);
            rt.why = Acore::StringFormat("On the way to {}: {} new spell{}", trainer->name, spells.size(), spells.size() == 1 ? "" : "s");
            return true;
        }
    }
    return false;
}

bool OrderService::PlanQuests(GuildBot const& bot, Player* player, BotRuntime& rt, uint32 zoneId, float within, bool anyKind)
{
    std::vector<QuestCandidate> candidates;
    uint32 const freeSlots = FreeQuestSlots(player);
    bool const logHasRoom = freeSlots >= MinFreeQuestSlots;

    // The quest of a task that ended for what stood in its way (a pack on the way, a pull the bot cannot afford, a
    // fight it left): with no progress since that task began it counts against the quest, and the second such end in
    // a row leaves the quest alone for a quarter of an hour. A level-23 rogue began the same two objectives seven
    // times in eight minutes, each ended by a pack or a pull, and died at the third quest of the same kind.
    if (uint32 const missed = std::exchange(rt.missQuest, 0))
    {
        uint32 const nowMs = getMSTime();
        if (QuestMark(player, missed) != rt.missMark)
            rt.questMisses.erase(missed);
        else if (!rt.IsDangerous(missed, player->GetLevel()))
        {
            BotRuntime::QuestMiss& miss = rt.questMisses[missed];
            if (miss.count && getMSTimeDiff(miss.at, nowMs) > QuestMissWindowMs)
                miss.count = 0;
            if (!miss.count)
                miss.at = nowMs;
            if (++miss.count >= QuestMissLimit)
            {
                BotRuntime::Danger& danger = rt.dangerousQuests[missed];
                danger.level = player->GetLevel();
                danger.until = nowMs + SpotRetryMs;
                rt.gateRecheck = true;
                LOG_INFO(ActivityLog, "activity: {} leaves quest {} \"{}\" for a quarter of an hour: {} of its tasks in a row ended "
                    "without progress for what stood in their way (a pack, a pull it cannot afford, a fight it left, a refused way, a rescue)", bot.name,
                    missed, QuestTitle(missed), uint32(miss.count));
                rt.questMisses.erase(missed);
            }
        }
    }

    // (a) Quests in the log: a turn-in when complete, else one task per open objective.
    std::vector<WaitingObjective> waiting;
    CollectLogTasks(bot, player, rt, zoneId, candidates, &waiting);
    // A quest none of whose objectives has a spot the bot can work at (Risk::JudgeSpot for every spawn near it): it is
    // left until the level from which one would do, instead of being looked at again with every plan (and its
    // targets walked to and let go of again). Another quest, or another zone, comes first.
    for (WaitingObjective const& entry : waiting)
    {
        uint32 const questId = entry.questId;
        if (rt.IsDangerous(questId, player->GetLevel()) ||
            std::any_of(candidates.begin(), candidates.end(), [questId](QuestCandidate const& c) { return c.questId == questId; }))
            continue;
        // (Several objectives of one quest: the lowest level any of them names; none when one of them names none. A
        // partner helps only when it helps with every one of them.)
        uint8 retry = entry.retry;
        bool partnerHelps = entry.partnerHelps;
        std::string why = entry.why;
        for (WaitingObjective const& other : waiting)
            if (other.questId == questId)
            {
                retry = (!other.retry || !retry) ? 0 : std::min(retry, other.retry);
                partnerHelps = partnerHelps && other.partnerHelps;
                if (why.empty())
                    why = other.why;
            }
        BotRuntime::Danger& danger = rt.dangerousQuests[questId];
        danger.level = retry ? uint8(retry - 1) : player->GetLevel();
        danger.until = getMSTime() + (retry ? SpotRetryLevelMs : SpotRetryMs);
        LOG_INFO(ActivityLog, "activity: {} leaves quest {} \"{}\" {}: {}", bot.name, questId, QuestTitle(questId),
            retry ? Acore::StringFormat("until level {}", uint32(retry)) : std::string("for a quarter of an hour"), why.empty() ?
                Acore::StringFormat("every spot of it near is too much for it at level {}", uint32(player->GetLevel())) : why);
        sQuestPartners.OnLeftUntil(player, questId, retry);
        // For guild bots up to that level the quest waits for a partner from now on (QuestPartners.h), when a second
        // character beside it would make the quest workable.
        if (partnerHelps)
            sQuestPartners.OnTooHard(player, questId, danger.level);
        rt.gateRecheck = true;
    }

    // (b) Eligible quests of the zone that the bot has not taken yet: pickups. With nothing at all in reach (a bot that
    // entered a wide zone at its far end: Ashenvale from Zoram'gar, its quests at Splintertree Post), the givers anywhere
    // in the zone count.
    for (float reach : { MaxQuestNpcDistance, ZoneWideDistance })
    {
        if (!logHasRoom || (reach > MaxQuestNpcDistance && !candidates.empty()))
            break;
        for (uint32 questId : sWorldIndex.GetZoneQuests(zoneId))
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest || rt.IsDangerous(questId, player->GetLevel()) || !IsQuestPickable(bot, player, quest))
                continue;
            // Not a quest whose work lies behind creatures the walk rules refuse (the zone pick counts it out as well): a
            // level-51 test character took five quests in the Blasted Lands whose creatures it would not have got to.
            if (zoneId && !QuestWorkReachable(player, rt, zoneId, quest))
                continue;

            std::optional<SpawnPoint> giver = NearestSpawn(sWorldIndex.GetQuestStarters(questId), player, 0, true, reach, rt);
            if (!giver)
                continue;
            // A hard quest (QuestPartners.h) is not walked to alone: only in a squad, or near its giver while a guild
            // character that could be its partner keeps it.
            if (!sQuestPartners.AllowsPickup(bot, player, questId, Dist2d(player, *giver)))
                continue;

            QuestCandidate candidate;
            candidate.distance = Dist2d(player, *giver);
            candidate.bucket = candidate.distance <= NearbyPickupRadius ? -1 : 2;
            candidate.questId = questId;
            candidate.task.kind = TaskKind::Pickup;
            candidate.task.questId = questId;
            candidate.task.hasSpot = true;
            candidate.task.spot = *giver;
            candidate.task.targetName = SpawnName(*giver);
            candidates.push_back(std::move(candidate));
        }
    }

    // Not back to a place the bot left a fight at or died at lately (FightEscape, RiskModel): a quest giver or ender
    // there waits until the place is free again (its time grows with every escape there), like the objectives do.
    std::erase_if(candidates, [&](QuestCandidate const& candidate)
    {
        return (candidate.task.kind == TaskKind::Pickup || candidate.task.kind == TaskKind::TurnIn) && candidate.task.hasSpot &&
            rt.IsInAvoidedArea(candidate.task.spot.mapId, candidate.task.spot.x, candidate.task.spot.y, player->GetLevel());
    });
    // And not to a quest giver or an ender that stands among creatures the bot cannot take (Risk::JudgeSpot for the
    // place, as for an objective's spawn): the catapult of "Nek'rosh's Gambit" stands in the Dragonmaw camp, and a
    // level-26 warrior died twice at the hand-in.
    std::erase_if(candidates, [&](QuestCandidate const& candidate)
    {
        return (candidate.task.kind == TaskKind::Pickup || candidate.task.kind == TaskKind::TurnIn) && candidate.task.hasSpot &&
            !IsPlaceUsable(player, rt, candidate.task.spot.mapId, candidate.task.spot.x, candidate.task.spot.y, candidate.task.spot.z);
    });
    if (within > 0.0f)
        std::erase_if(candidates, [within, anyKind](QuestCandidate const& candidate)
        {
            return (!anyKind && candidate.task.kind != TaskKind::Pickup && candidate.task.kind != TaskKind::TurnIn) ||
                candidate.distance > within;
        });
    // A quest partner squad (QuestPartners.h) works the quest it was formed for first.
    if (uint32 const partnerQuest = rt.squad.partnerQuest; partnerQuest &&
        std::any_of(candidates.begin(), candidates.end(), [partnerQuest](QuestCandidate const& c) { return c.questId == partnerQuest; }))
        std::erase_if(candidates, [partnerQuest](QuestCandidate const& c) { return c.questId != partnerQuest; });
    if (candidates.empty())
        return false;

    // WAQ ranking (wowbot335 QuestTaskRank): bucket, then nearest, then more progress, then lowest quest id.
    auto best = std::min_element(candidates.begin(), candidates.end(), [](QuestCandidate const& a, QuestCandidate const& b)
    {
        if (a.bucket != b.bucket)
            return a.bucket < b.bucket;
        if (a.distance != b.distance)
            return a.distance < b.distance;
        if (a.progress != b.progress)
            return a.progress > b.progress;
        return a.questId < b.questId;
    });

    BotTask task = best->task;
    StartTask(player, rt, task);

    std::string const title = QuestTitle(task.questId);
    switch (task.kind)
    {
        case TaskKind::Pickup:
            rt.why = Acore::StringFormat("Picking up \"{}\" from {}", title, task.targetName);
            break;
        case TaskKind::TurnIn:
            rt.why = Acore::StringFormat("Turning in \"{}\" at {}", title, task.targetName);
            break;
        case TaskKind::Escort:
            rt.why = Acore::StringFormat("Escorting {} for \"{}\"", task.targetName, title);
            break;
        case TaskKind::Event:
            rt.why = Acore::StringFormat("Going to {} for \"{}\"", task.targetName, title);
            break;
        case TaskKind::Fish:
            rt.why = Acore::StringFormat("Going fishing for \"{}\"", title);
            break;
        case TaskKind::Vendor:
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(task.buyItem);
            rt.why = Acore::StringFormat("Buying {} x {} for \"{}\" from {}", task.buyCount, proto ? proto->Name1 : "an item", title,
                task.targetName);
            break;
        }
        default:
            rt.why = Acore::StringFormat("Questing in {}: \"{}\" ({})", ZoneName(zoneId), title, task.targetName);
            break;
    }

    if (task.kind == TaskKind::Objective)
    {
        // A long way to the spot is walked by the planner (WalkTo: a route that avoids what is far above the bot);
        // NewRpg takes over near the spot. Its own walk there is one straight navmesh walk through whatever lives on
        // the way.
        // (A quest area without a spawn has its centre as the spot.)
        if (task.spot.mapId == player->GetMapId() && (task.spot.x != 0.0f || task.spot.y != 0.0f) && sBotSettings.IsOn(player, Setting::RoadsEnable) &&
            Dist2d(player, task.spot) > sBotSettings.GetFloat(player, Setting::RoadsMinDistance))
            rt.task.approach = true;
        else
            BotDriver::DoQuest(player, sObjectMgr->GetQuestTemplate(task.questId), task.objective, task.hasSpot,
                task.spot.x, task.spot.y, task.spot.z);
    }
    return true;
}

bool OrderService::PlanGrind(Player* player, BotRuntime& rt, uint32 zoneId, int32 minFoeLevel, float maxDistance)
{
    if (!sBotSettings.IsOn(player, Setting::GrindEnable))
        return false;
    int32 const level = player->GetLevel();
    // About its own level down to a little weaker, nearby (the owner, 2026-11-12; GuildAdventure.Grind.*), and nothing
    // the risk rules do not let it fight (Risk::CanFight: the level cap and what its own fights have cost it).
    int32 maxFoe = level + sBotSettings.Get(player, Setting::GrindLevelsAbove);
    while (maxFoe > 1 && !Risk::CanFight(player, rt, uint8(std::min(maxFoe, 255))))
        --maxFoe;
    std::vector<std::pair<float, MobSpawn const*>> candidates;
    int32 const minFoe = minFoeLevel >= 0 ? minFoeLevel : level - int32(sBotSettings.Get(player, Setting::GrindLevelsBelow));
    float const reach = maxDistance > 0.0f ? maxDistance : sBotSettings.GetFloat(player, Setting::GrindMaxDistance);
    for (MobSpawn const& mob : sWorldIndex.GetMobSpawns(player->GetMapId()))
    {
        if (int32(mob.maxLevel) < minFoe || int32(mob.maxLevel) > maxFoe)
            continue;
        float const distance = Dist2d(player, mob.spawn);
        if (distance > reach)
            continue;
        if (!WorldIndex::IsAttackableFaction(mob.factionTemplate, player))
            continue;
        if (rt.IsAvoided(mob.spawn) || rt.IsInAvoidedArea(mob.spawn.mapId, mob.spawn.x, mob.spawn.y, uint8(level)))
            continue;
        // Nor one whose straight way leads through a place the bot has just set aside (after a pull it would not make,
        // the planner offered four condor spawns in a row whose way led through the same three murlocs).
        if (rt.CrossesAvoidedArea(mob.spawn.mapId, player->GetPositionX(), player->GetPositionY(), mob.spawn.x, mob.spawn.y, uint8(level)))
            continue;
        candidates.emplace_back(distance, &mob);
    }
    if (candidates.empty())
        return false;

    // Densest cluster, nearer is better; the zone is checked for the best few only.
    constexpr std::size_t MaxCandidates = 400;
    std::sort(candidates.begin(), candidates.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    if (candidates.size() > MaxCandidates)
        candidates.resize(MaxCandidates);

    std::vector<std::pair<float, MobSpawn const*>> scored;
    for (auto const& [distance, mob] : candidates)
    {
        uint32 neighbours = 0;
        for (auto const& [otherDistance, other] : candidates)
        {
            float const dx = mob->spawn.x - other->spawn.x;
            float const dy = mob->spawn.y - other->spawn.y;
            if (dx * dx + dy * dy <= GrindClusterRadius * GrindClusterRadius)
                ++neighbours;
        }
        scored.emplace_back(Risk::GrindScore(player, *mob, neighbours, distance), mob);
    }
    std::sort(scored.begin(), scored.end(), [](auto const& a, auto const& b) { return a.first > b.first; });

    // Beasts and the like first. Humanoids live in camps, call for help and have patrols around them: of those only
    // ones below the bot's level, and only when nothing else is near (a level-22 rogue ground a level-22 Blackrock
    // Sentry at the orcs' camp in Redridge; the camp's scout and outrunner came along, and it died).
    constexpr std::size_t MaxZoneChecks = 10;
    // Not among elites: a grind is fight after fight in one place, and an elite down to five levels below the bot that
    // wanders or calls there comes into one of them (Buffadin died twice in a day grinding the broodlings and whelps of
    // Mazthoril in Winterspring among the elite Cobalt Mageweavers and Scalebanes, rounds 158 and 162). As on the walks
    // since round 143.
    // The elites that count, once for this look: near enough to a candidate, down to five levels below the bot, hostile
    // to it; their spawns and the points of their rounds (the Mageweavers walk the floor of the cave, and a spot on such
    // a round is no better than one beside a spawn).
    constexpr float EliteRoomYards = 60.0f;
    constexpr float EliteRoomHeight = 30.0f;
    constexpr int32 EliteLevelsBelow = 5;
    FactionTemplateEntry const* ownFaction = player->GetFactionTemplateEntry();
    std::vector<SpawnPoint> elites;
    for (std::vector<MobSpawn> const* strongs : { &sWorldIndex.GetStrongSpawns(player->GetMapId()), &sWorldIndex.GetPatrolPoints(player->GetMapId()) })
        for (MobSpawn const& strong : *strongs)
        {
            if (!strong.elite || int32(strong.maxLevel) + EliteLevelsBelow < level || Dist2d(player, strong.spawn) > reach + EliteRoomYards)
                continue;
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(strong.factionTemplate);
            if (faction && ownFaction && faction->IsHostileTo(*ownFaction))
                elites.push_back(strong.spawn);
        }
    auto amongElites = [&](SpawnPoint const& at)
    {
        for (SpawnPoint const& elite : elites)
            if (std::fabs(elite.z - at.z) <= EliteRoomHeight && Dist2d(elite.x, elite.y, at.x, at.y) <= EliteRoomYards)
                return true;
        return false;
    };
    for (int pass = 0; pass < 2; ++pass)
    for (std::size_t i = 0, checked = 0; i < scored.size() && checked < MaxZoneChecks; ++i)
    {
        MobSpawn const* mob = scored[i].second;
        CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(mob->spawn.entry);
        bool const humanoid = proto && proto->type == CREATURE_TYPE_HUMANOID;
        if (humanoid != (pass == 1) || (humanoid && int32(mob->maxLevel) >= level))
            continue;
        // Nor one that is dead and not back before the bot is there (its own kill a moment ago: a mage at the cap
        // walked from one dead Death Singer to the next, twenty seconds of "nothing to fight at the spot" each, round
        // 145; the objectives have the same rule).
        if (IsSpawnDown(player, mob->spawn))
            continue;
        // (Before the count of spots looked at: the look is cheap, and in the Eastern Plaguelands and Silithus the ten
        // best-placed spots stood among elites often enough that bots at the level cap waited at the inn, round 163.)
        if (amongElites(mob->spawn))
            continue;
        ++checked;
        if (sWorldIndex.GetSpawnZone(mob->spawn) != zoneId)
            continue;
        // Nor one without walkable ground at it (the ground guard refuses every walk there). A Noxious Plaguebat and a
        // Carrion Grub are spawned on the rock beside Light's Hope whose top the navmesh does not cover; grinds at the cap
        // chose them, every walk was refused ("20 yd through solid ground, 24 through the air") and each bot set the
        // spot aside for 20 min, then the next one chose it, round 228.
        if (!GroundGuard::HasGroundAt(player, mob->spawn.x, mob->spawn.y, mob->spawn.z))
            continue;
        // Nor one under water: a grind is the bot's own choice, and a fight under water is the worst place for one (no
        // paths for its runs, its breath; PickGrindTarget, round 230).
        if (player->GetMap()->IsUnderWater(player->GetPhaseMask(), mob->spawn.x, mob->spawn.y, mob->spawn.z, player->GetCollisionHeight()))
            continue;
        // The risk model's verdict on the spot, as for a quest's spawn (who would join, what stands next to it).
        if (!Risk::JudgeSpot(player, rt, mob->spawn).usable)
            continue;
        // Nor one whose straight way leads through the middle of a camp it cannot take (the walk to a grind spot has no
        // gate of its own, and the camp look on the way sees a camp only 45 yards ahead): Priestyo, a level-60 priest,
        // kept away from Zaeldarr's Mossflayer camp in the Eastern Plaguelands, was given a Putrid Gargoyle 319 yd off on
        // the camp's far side, walked into the camp and died with three on it and no way out, round 244. (What stands at
        // the spot itself is the spot verdict's.)
        constexpr float GrindCampSpotYards = 40.0f;
        if (!StandsAtCamp(player, rt) && Risk::EntersCamp(player, rt, player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
                mob->spawn.x, mob->spawn.y, nullptr, nullptr, 0.0f, GrindCampSpotYards))
            continue;

        BotTask task;
        task.kind = TaskKind::Grind;
        task.hasSpot = true;
        task.spot = mob->spawn;
        task.targetName = SpawnName(mob->spawn);
        // A band of the caller's (the level cap: 50 to 60) is the band the step fights at the spot too; without it the
        // step took the grind settings' (57 to 60 at the cap) and found nothing at a spot of level-54 Plaguehounds:
        // "nothing to fight at the spot" every twenty seconds, one fight in four minutes, round 145.
        if (minFoeLevel >= 0)
        {
            task.grindMin = uint8(std::clamp<int32>(minFoe, 1, 255));
            task.grindMax = uint8(std::clamp<int32>(maxFoe, 1, 255));
        }
        StartTask(player, rt, task);
        rt.why = Risk::IsCautious(rt, uint8(level)) ?
            Acore::StringFormat("Playing it safe after several deaths: grinding in {} ({})", ZoneName(zoneId), task.targetName) :
            Acore::StringFormat("Grinding in {}: {} (no fitting quests left)", ZoneName(zoneId), task.targetName);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------------------------------------------

void OrderService::StartTask(Player* player, BotRuntime& rt, BotTask task)
{
    uint32 const now = getMSTime();
    task.startedAt = now;
    task.nearSince = 0;
    task.lastAssert = 0;
    rt.task = std::move(task);
    rt.stuckSince = 0; // UpdateStuck starts both of its clocks on its first call for this task
    rt.taskQuestMark = rt.task.questId ? QuestMark(player, rt.task.questId) : 0;
    if (rt.task.kind != TaskKind::Hold)
    {
        rt.idleHolds = 0;
        ++rt.window.tasksBegun;
        ++rt.total.tasksBegun;
    }
    // Every task looks for a flight at its first step (the clock was the bot's: a travel refused on foot was followed
    // by travels to three other zones within the same minute, none of which looked for a flight, and each zone was
    // written off as unreachable).
    rt.nextFlightCheck = 0;
    if (rt.task.kind == TaskKind::Hold)
        BotDriver::Rest(player);

    // Holds are re-planned every 30 s; only real tasks go into the activity log.
    if (rt.task.kind != TaskKind::Hold)
        LOG_INFO(ActivityLog, "activity: {} starts {}{}{}{} (target {}{}) in {}", player->GetName(), ToString(rt.task.kind),
            rt.task.questId ? Acore::StringFormat(" quest {} \"{}\"", rt.task.questId, QuestTitle(rt.task.questId)) : "",
            rt.task.objective >= 0 ? Acore::StringFormat(" objective {}", rt.task.objective) : "",
            rt.task.kind == TaskKind::Voyage ? Acore::StringFormat(" on {}", rt.task.link.name) : "",
            rt.task.targetName.empty() ? "-" : rt.task.targetName,
            rt.task.hasSpot ? Acore::StringFormat(", {:.0f} yd", Dist2d(player, rt.task.spot)) : "",
            ZoneName(player->GetZoneId()));
}

void OrderService::ClearTask(BotRuntime& rt, uint32 waitMs, char const* outcome)
{
    // A task that ended within 10 s counts for the status line (begun again and again: TrackActivity).
    if (rt.task.kind != TaskKind::None && rt.task.kind != TaskKind::Hold && getMSTimeDiff(rt.task.startedAt, getMSTime()) < 10 * IN_MILLISECONDS)
    {
        ++rt.window.tasksQuick;
        ++rt.total.tasksQuick;
    }
    if (rt.task.kind != TaskKind::None && rt.task.kind != TaskKind::Hold)
        LOG_INFO(ActivityLog, "activity: {} ends {}{} after {} s{}{}: {}", rt.name, ToString(rt.task.kind),
            rt.task.questId ? Acore::StringFormat(" quest {}", rt.task.questId) : "",
            getMSTimeDiff(rt.task.startedAt, getMSTime()) / IN_MILLISECONDS,
            rt.task.deaths ? Acore::StringFormat(", deaths {}", rt.task.deaths) : "",
            rt.task.unreachable ? ", target not reached" : "", outcome ? outcome : "over");
    // A journey that ended before its flight (stuck on the way to the master, its walk refused): the next task may fly. Its
    // gap held back every other journey for five minutes, "no flight: it has just flown for another task", and Ashmire,
    // which had flown nowhere, had seven tasks in Stranglethorn Vale refused one after the other in 16 s for the same pair
    // of level-41 creatures outside Booty Bay and wrote the zone off for six hours, 2026-11-26.
    if (rt.task.kind == TaskKind::Travel && rt.task.journey && !rt.task.flew)
        rt.nextJourney = 0;
    // A way refused, or a target no way led to: counted at the place the bot stands (LeaveBoxedPlace).
    if (outcome && rt.task.kind != TaskKind::None && (IsRefusedWay(outcome) || IsNoWayEnd(outcome)) && rt.refusalsPending < 255)
        ++rt.refusalsPending;
    // Work on a quest that ended for what stood in its way, or whose way was refused: the next plan looks whether the
    // quest got anywhere, and the second such end in a row without progress leaves the quest alone for a quarter of an
    // hour (PlanQuests). Every kind of quest work counts, an escort, a quest event, a pickup and a journey to a far spot
    // included: the escort of "Suntara Stones" was begun again 31 times in 21 minutes, each time ended by a pack on the
    // way, because setting the escort's spawn aside does not stop it while its NPC stands in sight. (A hand-in has a
    // rule of its own: it waits for its way to clear and then sets its ender aside; an errand's travel is dropped.)
    // (A quest item's vendor too: a priest set out for six vendors of a Flagon of Mead one after the other, each walk
    // refused for a place it had set aside on the way, thirteen refusals in a few minutes.)
    bool const questWork = rt.task.kind == TaskKind::Objective || rt.task.kind == TaskKind::Interact ||
        rt.task.kind == TaskKind::Escort || rt.task.kind == TaskKind::Event || rt.task.kind == TaskKind::Pickup ||
        (rt.task.kind == TaskKind::Vendor && rt.task.buyItem) ||
        (rt.task.kind == TaskKind::Travel && rt.task.journey && !rt.task.errand);
    if (outcome && rt.task.questId && questWork && (IsBlockedEnd(outcome) || IsRefusedWay(outcome)))
    {
        rt.missQuest = rt.task.questId;
        rt.missMark = rt.taskQuestMark;
        // And that spawn is not the next plan's choice again: another spot of the quest, or another quest.
        if (rt.task.hasSpot)
            rt.Avoid(rt.task.spot, SpotRetryMs);
    }
    rt.task = BotTask();
    rt.stuckSince = 0;
    if (waitMs)
        rt.waitUntil = getMSTime() + waitMs;
}

namespace
{
    // ", stood most while: hold 40 s, turn-in 12 s": the tasks the bot stood still with in the window, the longest first
    // (up to three, each 5 s or more; nothing when none was).
    std::string StoodMostWhile(BotRuntime::Tally const& tally)
    {
        constexpr uint32 MinMs = 5 * IN_MILLISECONDS;
        constexpr std::size_t Shown = 3;
        std::vector<std::pair<uint32, std::size_t>> buckets;
        for (std::size_t i = 0; i < tally.stoodByKindMs.size(); ++i)
            if (tally.stoodByKindMs[i] >= MinMs)
                buckets.emplace_back(tally.stoodByKindMs[i], i);
        if (buckets.empty())
            return {};
        std::sort(buckets.begin(), buckets.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
        std::string text = ", stood most while: ";
        for (std::size_t i = 0; i < buckets.size() && i < Shown; ++i)
        {
            char const* name = buckets[i].second == BotRuntime::Tally::StoodWaiting ? "waiting on purpose" :
                ToString(TaskKind(buckets[i].second));
            text += Acore::StringFormat("{}{} {} s", i ? ", " : "", name, buckets[i].first / IN_MILLISECONDS);
        }
        return text;
    }
}

void OrderService::LogStatus(GuildBot const& bot, Player* player, BotRuntime& rt)
{
    uint32 const now = getMSTime();
    rt.nextStatus = now + StatusLogMs;
    std::string moved = "-";
    if (rt.hasStatusPosition)
        moved = rt.statusMap != player->GetMapId() ? std::string("other map") :
            Acore::StringFormat("{:.0f} yd", Dist2d(player, rt.statusX, rt.statusY));
    rt.hasStatusPosition = true;
    rt.statusMap = player->GetMapId();
    rt.statusX = player->GetPositionX();
    rt.statusY = player->GetPositionY();

    uint32 const nextLevelXp = player->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
    uint32 const xp = player->GetUInt32Value(PLAYER_XP);
    LOG_INFO(ActivityLog, "activity: {} status: level {} ({}% xp), {} at ({:.0f}, {:.0f}) map {}, moved {} in {} min, hp {}%, "
        "durability {}%, {} free bag slots, {}c, xp/h {}, {} fights ({} against 2+ foes, {} begun below half health, {} left), "
        "{} rests ({} s), {} deaths, {} food, {} drink carried ({} and {} used), stood still {} s ({} s with a walk to do), walked "
        "{:.0f} yd, {} turns back, {} tasks begun ({} ended within 10 s), {} target switches{}, task {} ({} s), {}: {}", bot.name,
        uint32(player->GetLevel()),
        nextLevelXp ? uint64(xp) * 100 / nextLevelXp : 0, ZoneName(player->GetZoneId()), player->GetPositionX(),
        player->GetPositionY(), player->GetMapId(), moved, StatusLogMs / (MINUTE * IN_MILLISECONDS),
        uint32(player->GetHealthPct()), MinDurabilityPercent(player), player->GetFreeInventorySpace(), player->GetMoney(),
        GetXpPerHour(rt.guid), rt.window.fights, rt.window.crowded, rt.window.weakStarts, rt.window.escapes, rt.window.rests,
        rt.window.restSeconds, rt.window.deaths, CountConsumables(player, FoodSpellCategory),
        CountConsumables(player, DrinkSpellCategory), rt.window.foodUsed, rt.window.drinkUsed, rt.window.stoodMs / IN_MILLISECONDS,
        rt.window.stoodWalkMs / IN_MILLISECONDS, rt.window.walkedYards, rt.window.turnBacks, rt.window.tasksBegun, rt.window.tasksQuick,
        rt.window.targetSwitches, StoodMostWhile(rt.window), ToString(rt.task.kind),
        rt.task.kind != TaskKind::None ? getMSTimeDiff(rt.task.startedAt, now) / IN_MILLISECONDS : 0,
        player->IsInCombat() ? "in combat" : (player->IsAlive() ? "alive" : "dead"), rt.why.empty() ? "-" : rt.why);
    rt.window = BotRuntime::Tally();
}

std::string OrderService::DescribeNpcFailure(Player* player, BotRuntime const& rt, Quest const* quest) const
{
    BotTask const& task = rt.task;
    WorldObject* target = FindInSight(player, task.spot);
    bool const npcHasQuest = target && quest && (task.kind == TaskKind::TurnIn ?
        target->hasInvolvedQuest(quest->GetQuestId()) : target->hasQuest(quest->GetQuestId()));
    std::string canDo = "-";
    if (quest)
        canDo = task.kind == TaskKind::TurnIn ?
            Acore::StringFormat("can reward {}", player->CanRewardQuest(quest, false) ? "yes" : "no") :
            Acore::StringFormat("can take {}, can add {}, worth doing {}", player->CanTakeQuest(quest, false) ? "yes" : "no",
                player->CanAddQuest(quest, false) ? "yes" : "no", NewRpgHelpers::IsQuestWorthDoing(player, quest) ? "yes" : "no");
    return Acore::StringFormat("{} in sight {}, distance {:.1f}, reached {}, offers/takes it {}, {}, rpg {}, in combat {}, "
        "quest status {}, {} s in the world", task.targetName, target ? "yes" : "no",
        target ? player->GetDistance(target) : Dist2d(player, task.spot),
        target && BotDriver::HasReachedVisitTarget(player, target->GetGUID()) ? "yes" : "no", npcHasQuest ? "yes" : "no", canDo,
        ToString(BotDriver::GetRpgState(player)), player->IsInCombat() ? "yes" : "no",
        quest ? uint32(player->GetQuestStatus(quest->GetQuestId())) : 0, getMSTimeDiff(rt.since, getMSTime()) / IN_MILLISECONDS);
}

void OrderService::OnQuestNpcFailed(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt, char const* reason)
{
    BotTask const& task = rt.task;
    uint32 const questId = task.questId;
    LOG_INFO(ActivityLog, "activity: {} quest {} \"{}\" {}: {}", bot.name, questId, QuestTitle(questId), reason,
        DescribeNpcFailure(player, rt, sObjectMgr->GetQuestTemplate(questId)));

    // The task began in the bot's first minute in the world: try again at once, it is no failure.
    if (getMSTimeDiff(rt.since, task.startedAt) < LoginGraceMs)
    {
        ClearTask(rt, 5 * IN_MILLISECONDS, "failed in the first minute after login; trying again");
        return;
    }
    rt.Avoid(task.spot, QuestNpcRetryMs);
    FailQuest(guild, bot, player, questId, reason);
    ClearTask(rt, 5 * IN_MILLISECONDS, reason);
}

void OrderService::StepSpiritRes(GuildBot const& bot, Player* player, BotRuntime& rt)
{
    if (player->IsBeingTeleported())
        return;

    ServicePoint const* nearest = nullptr;
    float nearestDistance = SpiritHealerSearchRadius;
    for (ServicePoint const& service : sWorldIndex.GetServices(ServiceType::SpiritHealer))
    {
        if (service.spawn.mapId != player->GetMapId())
            continue;
        float const distance = Dist2d(player, service.spawn);
        if (distance < nearestDistance)
        {
            nearest = &service;
            nearestDistance = distance;
        }
    }
    // The ghost already left the graveyard (or there is no healer): playerbots takes it back to the corpse.
    if (!nearest)
    {
        LOG_INFO("module", "mod-guild-adventure: {} should take the spirit healer, but none is within {:.0f} yd of its ghost at "
            "({:.0f}, {:.0f})", bot.name, SpiritHealerSearchRadius, player->GetPositionX(), player->GetPositionY());
        rt.spiritRes = false;
        return;
    }

    Creature* healer = player->FindNearestCreature(nearest->spawn.entry, SpiritHealerSearchRadius, true);
    if (!healer)
    {
        LOG_INFO("module", "mod-guild-adventure: {} should take the spirit healer, but {} is not there", bot.name, nearest->name);
        rt.spiritRes = false;
        return;
    }
    if (!player->IsWithinDistInMap(healer, INTERACTION_DISTANCE))
    {
        // A ghost's step across the graveyard, like the hops onto a ship's deck.
        float x, y, z;
        healer->GetNearPoint(player, x, y, z, player->GetObjectSize(), 2.0f, healer->GetAngle(player));
        BotDriver::Hop(player, x, y, z);
        return;
    }
    if (BotDriver::ResurrectAtSpiritHealer(player, healer))
        LOG_INFO("module", "mod-guild-adventure: {} resurrected at the spirit healer (its corpse lay where it would have died again)", bot.name);
    else
        LOG_INFO("module", "mod-guild-adventure: {} could not use the spirit healer; back to its corpse", bot.name);
    rt.spiritRes = false;
    rt.hasLastPosition = false; // the core may move it to the graveyard nearest to its corpse
}

void OrderService::OnDied(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    uint32 const now = getMSTime();
    uint8 const level = player->GetLevel();
    ++rt.task.deaths;
    ++rt.window.deaths;
    ++rt.total.deaths;
    uint8 const foesAtOnce = rt.fight.active ? rt.fight.foes : 0;
    EndFight(player, rt, true);

    CreatureTemplate const* foe = rt.lastFoeEntry ? sObjectMgr->GetCreatureTemplate(rt.lastFoeEntry) : nullptr;
    // A fight that ended a while ago did not kill it (a druid died of fatigue on a sea route, and its line named a
    // creature it had fought in another zone eight minutes before): the line says how long ago, and what the place
    // says instead.
    constexpr uint32 FoeFreshMs = 20 * IN_MILLISECONDS;
    uint32 const foeAgo = foe ? getMSTimeDiff(rt.lastFoeAt, now) : 0;
    bool const foeFresh = foe && foeAgo <= FoeFreshMs;
    // Where it died says what killed it when no fight did: a bot that drowned leaves no fight line.
    uint8 const water = Escape::WaterAt(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
    uint32 const underWaterSeconds = rt.underWaterSince ? getMSTimeDiff(rt.underWaterSince, now) / IN_MILLISECONDS : 0;
    std::string const waterNote = water == 2 ? Acore::StringFormat(", under water{}", rt.underWaterSince ?
        Acore::StringFormat(" for {} s", underWaterSeconds) : std::string()) :
        (water == 1 ? std::string(", in water") : std::string());
    // Deep sea (the core's "dark water") wears a swimmer out: fatigue. A minute and more under water: its breath.
    std::string cause;
    if (!foeFresh)
    {
        LiquidData const liquid = player->GetMap()->GetLiquidData(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ(), player->GetCollisionHeight(), {});
        if (water && (liquid.Flags & MAP_LIQUID_TYPE_DARK_WATER))
            cause = ", of fatigue in deep water (no fight)";
        else if (water == 2 && underWaterSeconds >= 60)
            cause = ", drowned (no fight)";
        else
            cause = ", not in a fight";
    }
    rt.underWaterSince = 0;
    // A squad member has no task of its own: it follows its leader (a dungeon run's group too). The line names the leader,
    // so a death in a dungeon does not read as a bot standing about there (Magefrost in Dire Maul East, round 215).
    std::string doing = rt.task.kind == TaskKind::None ? std::string("no task") : std::string(ToString(rt.task.kind));
    if (rt.task.kind == TaskKind::None)
        if (ObjectGuid const leaderGuid = sSquadService.GetLeader(bot.guid); !leaderGuid.IsEmpty())
            if (GuildBot const* leader = guild.FindBot(leaderGuid))
                doing = Acore::StringFormat("no task of its own (it follows its squad leader {}{})", leader->name,
                    leader->order.type == OrderType::Dungeon ? " on a dungeon run" : "");
    LOG_INFO(ActivityLog, "activity: {} died at level {} in {} at ({:.0f}, {:.0f}, {:.0f}) map {}{}{}{}, last fought {}, during {}{}, "
        "durability {}%", bot.name, uint32(level), ZoneName(player->GetZoneId()), player->GetPositionX(), player->GetPositionY(),
        player->GetPositionZ(), player->GetMapId(), waterNote, cause, DescribeLastHit(player),
        !foe ? std::string("nothing") : (foeFresh ? Acore::StringFormat("{} (level {})", foe->Name, uint32(rt.lastFoeLevel)) :
            Acore::StringFormat("{} (level {}) {} before", foe->Name, uint32(rt.lastFoeLevel),
                foeAgo >= 2 * MINUTE * IN_MILLISECONDS ? Acore::StringFormat("{} min", foeAgo / (MINUTE * IN_MILLISECONDS)) :
                    Acore::StringFormat("{} s", foeAgo / IN_MILLISECONDS))),
        doing,
        rt.task.questId ? Acore::StringFormat(" quest {} \"{}\"", rt.task.questId, QuestTitle(rt.task.questId)) : "",
        MinDurabilityPercent(player));
    rt.lastFoeEntry = 0;
    // Not back to life among what killed it: playerbots uses a soulstone or Reincarnation at once, where the bot lies
    // (a warlock died again 21 seconds after its death, at the same spot). With hostile creatures at the corpse the
    // bot releases its spirit and walks back like everyone else.
    // (What killed it counts: those creatures turn for home the moment it dies, and "on its way home" was left out
    // of the count. Of 39 deaths of one night with three or more creatures on the bot, 31 found "nothing hostile"
    // at the corpse; a warlock rose by its soulstone in the middle of a gnoll camp 15 seconds after six of them had
    // killed it, and a ghost walked back into a field of ten.)
    constexpr float SelfResClearYards = 30.0f;
    if (player->GetUInt32Value(PLAYER_SELF_RES_SPELL))
        if (uint32 const hostiles = Escape::HostilesAround(player, SelfResClearYards, true))
        {
            player->SetUInt32Value(PLAYER_SELF_RES_SPELL, 0);
            LOG_INFO(ActivityLog, "activity: {} does not rise where it fell (soulstone or reincarnation): {} hostile creature{} within {:.0f} yd",
                bot.name, hostiles, hostiles == 1 ? "" : "s", SelfResClearYards);
        }
    // The place and the foe are remembered (RiskModel): dangerous places are set aside whatever the quest.
    if (std::string const note = Risk::OnDeath(player, rt, foe ? rt.lastFoeLevel : 0, foesAtOnce); !note.empty())
        LOG_INFO("module", "mod-guild-adventure: {} died at ({:.0f}, {:.0f}): {}", bot.name, player->GetPositionX(),
            player->GetPositionY(), note);
    // A squad member's death counts for its leader's plans too.
    OnSquadMemberDied(player, rt);

    // Died again where it died a moment ago: the ghost takes the spirit healer instead of reviving among the same mobs.
    float const dx = player->GetPositionX() - rt.lastDeathX;
    float const dy = player->GetPositionY() - rt.lastDeathY;
    rt.spiritRes = rt.lastDeathAt && getMSTimeDiff(rt.lastDeathAt, now) <= CorpseCampWindowMs &&
        rt.lastDeathMap == player->GetMapId() && dx * dx + dy * dy <= CorpseCampRadius * CorpseCampRadius &&
        MinDurabilityPercent(player) >= SpiritResMinDurability;
    // The corpse lies in a camp or under water: coming back to life there is the next death (a mage died twice 50 s
    // apart at one gnoll camp, a warlock drowned at its corpse). The spirit healer then, at its price.
    constexpr uint32 CorpseCampCreatures = 2;
    constexpr float CorpseCampYards = 30.0f;
    // (Whatever its gear: the spirit healer's quarter of the durability costs less than rising beside what killed it.
    // Stormstrikey, a level-60 shaman at 33 % durability, rose 8 yards from the Nerubian Overseer that had killed it,
    // a rare elite, and was dead again fifteen seconds later, round 194; the urgent vendor trip repairs worn gear.)
    if (!rt.spiritRes)
    {
        uint32 const around = Escape::HostilesAround(player, CorpseCampYards, true);
        // ... or beside one creature far above it (a level-17 paladin stood up next to the level-24 boar that had
        // killed it and was dead again seven seconds later).
        constexpr uint32 CorpseStrongLevels = 3;
        uint32 worth = 0;
        Creature* const strong = Escape::StrongestAround(player, CorpseCampYards, &worth, true);
        bool const beside = strong && worth >= uint32(player->GetLevel()) + CorpseStrongLevels;
        if (water == 2 || around >= CorpseCampCreatures || beside)
        {
            rt.spiritRes = true;
            LOG_INFO(ActivityLog, "activity: {} will take the spirit healer: its corpse lies {}", bot.name, water == 2 ?
                std::string("under water") : (around >= CorpseCampCreatures ? Acore::StringFormat("among {} hostile creatures", around) :
                    Acore::StringFormat("beside {} (level {})", strong->GetName(), uint32(strong->GetLevel()))));
        }
    }
    // The spirit healer of this corpse stands where the bot cannot stand either: home instead (Risk::Rescue: the
    // spirit healer's price, then its bind point). A level-19 mage died in an orc camp, took the spirit healer of
    // the graveyard 98 yards away inside the same camp, had every way out refused ("passes level-23 creatures"),
    // walked out through the camp and was dead again 93 seconds after its first death.
    if (rt.spiritRes && !rt.rescue && sBotSettings.For(player).riskRescueDeaths)
        if (GraveyardStruct const* grave = sGraveyard->GetClosestGraveyard(player, player->GetTeamId()))
            if (grave->Map == player->GetMapId() &&
                (rt.IsInAvoidedArea(grave->Map, grave->x, grave->y, level) ||
                    !Risk::IsSafePlace(player, rt, grave->Map, grave->x, grave->y, grave->z)))
            {
                rt.rescue = true;
                LOG_INFO(ActivityLog, "activity: {} will go home instead: the spirit healer of its corpse ({:.0f} yd from it) stands in a "
                    "place it keeps away from or among creatures it cannot take", bot.name,
                    player->GetDistance2d(grave->x, grave->y));
            }
    rt.lastDeathAt = now;
    rt.lastDeathMap = player->GetMapId();
    rt.lastDeathX = player->GetPositionX();
    rt.lastDeathY = player->GetPositionY();

    // Two deaths in one zone within a quarter of an hour: on the autopilot the zone counts as empty for a while and
    // the zones are looked at again (a level-24 rogue died four times in Redridge in twenty minutes, each time back at
    // the same camps).
    constexpr uint32 ZoneDeathWindowMs = 15 * MINUTE * IN_MILLISECONDS;
    uint32 const deathZone = player->GetZoneId();
    if (rt.zoneDeathZone != deathZone || getMSTimeDiff(rt.zoneDeathAt, now) > ZoneDeathWindowMs)
    {
        rt.zoneDeathZone = deathZone;
        rt.zoneDeathAt = now;
        rt.zoneDeaths = 0;
    }
    if (++rt.zoneDeaths >= 2 && bot.order.type == OrderType::Auto)
    {
        rt.zoneDeaths = 0;
        // Each time at one level the zone rests twice as long (15, 30, 60, 120 min), from the second time it is not
        // picked at all meanwhile, and the bot does not grind in it while it rests (RunAdventure). A level-19 rogue
        // died in Redridge six times in three quarters of an hour: the quarter of an hour was over each time, and in
        // between it ground beside the patrols of Stonewatch, where the spirit healer had put it.
        auto& strike = rt.zoneStrikes[deathZone];
        if (strike.second != player->GetLevel())
            strike = { uint8(0), player->GetLevel() };
        strike.first = std::min<uint8>(strike.first + 1, 4);
        uint32 const rest = EmptyZoneMs << (strike.first - 1);
        rt.emptyZone = deathZone;
        rt.emptyZoneUntil = now + rest;
        rt.emptyZoneDeaths = true;
        if (strike.first >= 2)
            rt.unreachableZones[deathZone] = now + rest;
        rt.zoneChosenAt = 0;
        LOG_INFO(ActivityLog, "activity: {} died twice in {} within {} min (time {} at level {}): the zone counts as empty for {} min, "
            "it does not grind there meanwhile and looks at the zones again", bot.name, ZoneName(deathZone),
            getMSTimeDiff(rt.zoneDeathAt, now) / (MINUTE * IN_MILLISECONDS) + 1, uint32(strike.first), uint32(player->GetLevel()),
            rest / (MINUTE * IN_MILLISECONDS));
    }

    // The guild's memory of it: the zone, and the quest it was working on (ChooseZone, CollectLogTasks).
    AddGuildDeath(guild.ownerAccount, deathZone, (rt.task.kind == TaskKind::Objective || rt.task.kind == TaskKind::Interact ||
        rt.task.kind == TaskKind::Event) ? rt.task.questId : 0, player->GetLevel());

    // Quest objectives are counted per quest, so a task the planner started again (after a jump, the stuck tracker)
    // still adds up.
    if ((rt.task.kind == TaskKind::Objective || rt.task.kind == TaskKind::Interact ||
        rt.task.kind == TaskKind::Event) && rt.task.questId)
    {
        uint32 const questId = rt.task.questId;
        // A death of the guild on this quest (QuestPartners.h: several make it a quest for partners).
        sQuestPartners.OnQuestDeath(player, questId);
        BotRuntime::DeathTally& tally = rt.questDeaths[questId];
        if (!tally.count || getMSTimeDiff(tally.firstAt, now) > DeathWindowMs)
            tally = { 0, now };
        // One death rests the quest (it was three: the bot went back to what had killed it twice more). The third
        // rest of a quest counts as a failure.
        constexpr uint32 QuestDeathsToRest = 1;
        constexpr uint8 RestsToFail = 3;
        if (++tally.count < QuestDeathsToRest)
            return;
        rt.questDeaths.erase(questId);

        // To a creature far above it (four levels or more): four times as long. A level-37 paladin was back in the
        // water of a level-46 shark as soon as the half hour was over.
        constexpr uint8 FarAboveLevels = 4;
        bool const farAbove = foe && foeFresh && rt.lastFoeLevel >= level + FarAboveLevels;
        uint32 const restMs = SetAsideMs * (farAbove ? 4 : 1);
        BotRuntime::Danger& danger = rt.dangerousQuests[questId];
        danger.level = level;
        danger.until = now + restMs;
        danger.strikes = uint8(std::min<uint32>(danger.strikes + 1, 255));
        rt.gateRecheck = true;
        std::string const title = QuestTitle(questId);
        LOG_INFO(ActivityLog, "activity: {} leaves quest {} \"{}\" until level {} or for {} min: it died on it{}{}",
            bot.name, questId, title, uint32(level) + 1, restMs / (MINUTE * IN_MILLISECONDS),
            farAbove ? Acore::StringFormat(" to a creature far above it ({}, level {})", foe->Name, uint32(rt.lastFoeLevel)) : std::string(),
            danger.strikes >= RestsToFail ? " (the third time: it counts as a failure)" : "");
        if (danger.strikes >= RestsToFail)
        {
            // Set aside twice: a failure in the sense of wowbot335 A21 (abandoned with one retry, blocked the second time).
            FailQuest(guild, bot, player, questId, "deaths");
            danger.strikes = 0;
        }
        rt.whyBeforeDeath = Acore::StringFormat("Too dangerous for now: \"{}\" (it died there); doing something else first", title);
        ClearTask(rt, 0, "set aside after deaths");
        return;
    }

    // Grinding: one death is enough (no grind spot is worth a second one; a quest's place takes three). The spot and
    // where the bot died (it roams around its spot) are set aside.
    if (rt.task.kind == TaskKind::Grind && rt.task.deaths >= 1)
    {
        rt.AddAvoidArea(rt.task.spot.mapId, rt.task.spot.x, rt.task.spot.y, level, SetAsideMs, true);
        rt.AddAvoidArea(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), level, SetAsideMs, true);
        LOG_INFO("module", "mod-guild-adventure: {} died {} times grinding at {}; the spot is set aside until level {} or {} min",
            bot.name, rt.task.deaths, rt.task.targetName, uint32(level) + 1, SetAsideMs / (MINUTE * IN_MILLISECONDS));
        rt.whyBeforeDeath = Acore::StringFormat("Too dangerous for now: {} ({} deaths); grinding elsewhere", rt.task.targetName,
            rt.task.deaths);
        ClearTask(rt, 0, "grind spot set aside after deaths");
        return;
    }

    if (rt.task.deaths < DeathsToSetAside)
        return;

    // Dying again and again on the way to an NPC: the way there is too dangerous for now, another NPC (or another
    // quest) comes first.
    switch (rt.task.kind)
    {
        case TaskKind::Pickup:
        case TaskKind::TurnIn:
        case TaskKind::Vendor:
        case TaskKind::Train:
        case TaskKind::Discover:
            LOG_INFO("module", "mod-guild-adventure: {} died {} times on the way to {} ({}); skipped for {} min", bot.name,
                rt.task.deaths, rt.task.targetName, ToString(rt.task.kind), SetAsideMs / (MINUTE * IN_MILLISECONDS));
            rt.Avoid(rt.task.spot, SetAsideMs);
            rt.whyBeforeDeath = Acore::StringFormat("Too dangerous for now: the way to {} ({} deaths)", rt.task.targetName,
                rt.task.deaths);
            if (rt.task.kind == TaskKind::Vendor || rt.task.kind == TaskKind::Train)
                rt.nextUpkeepCheck = 0;
            ClearTask(rt, 0, "target skipped after deaths on the way");
            break;
        // A travel that keeps dying counts like one that keeps getting stuck.
        case TaskKind::Travel:
            OnTravelStuck(bot, rt, Dist2d(player, rt.task.spot));
            rt.whyBeforeDeath = rt.why;
            break;
        case TaskKind::Voyage:
            if (rt.task.voyage == VoyagePhase::ToDock)
            {
                OnTravelStuck(bot, rt, Dist2d(player, rt.task.link.from.x, rt.task.link.from.y));
                rt.whyBeforeDeath = rt.why;
            }
            break;
        default:
            break;
    }
}

void OrderService::PauseStuck(BotRuntime& rt)
{
    // wowbot335 A20: time spent fighting or dead does not count, so both clocks move on with the tick.
    // (Never past now: UpdateStuck takes now - since without a wrap, and a clock a step's gap ahead of now read as two
    // minutes and more of standing; with the rest's pause every bot whose task had begun within its last step was
    // "stuck" the second its rest ended, 19 in a quarter of an hour, 2026-11-26.)
    if (!rt.stuckSince)
        return;
    uint32 const now = getMSTime();
    auto catchUp = [&](uint32& since)
    {
        int32 const behind = int32(now - since);
        since = behind > 0 ? since + std::min<uint32>(rt.stepGapMs, uint32(behind)) : now;
    };
    catchUp(rt.stuckSince);
    catchUp(rt.approachSince);
}

bool OrderService::StartShoreLeave(GuildBot const& bot, Player* player, BotRuntime& rt, MotionTransport* ship)
{
    TravelLink const* known = nullptr;
    for (TravelLink const& link : sWorldIndex.GetTravelLinks())
        if (link.transportEntry == ship->GetEntry())
        {
            known = &link;
            break;
        }
    if (!known)
    {
        if (rt.why != "On board an unknown transport")
            LOG_INFO("module", "mod-guild-adventure: {} is on {} ({}), which is no known route: it stays aboard", bot.name,
                ship->GetName(), ship->GetEntry());
        rt.why = "On board an unknown transport";
        return false;
    }

    BotTask task;
    task.kind = TaskKind::Voyage;
    task.link = *known;
    task.voyage = VoyagePhase::Ride;
    task.anyDock = true;
    task.shoreLeave = true;
    task.ship = ship->GetGUID();
    task.targetName = known->name;
    StartTask(player, rt, task);
    rt.task.phaseSince = getMSTime();
    LOG_INFO("module", "mod-guild-adventure: {} is on board {} without a voyage: going ashore at the next stop", bot.name,
        known->name);
    rt.why = Acore::StringFormat("On board {}: going ashore at the next stop", known->name);
    return true;
}

namespace
{
    constexpr uint32 HearthstoneSpell = 8690;
    constexpr uint32 HearthstoneItem = 6948;
    constexpr uint32 HearthstoneCooldownMs = HOUR * IN_MILLISECONDS;

    // An AI-played character makes the inn of a hub it hands a quest in at its home, as a player does on the way up:
    // its hearthstone (the planner's way out, HearthHome, and the rescue) then leads into the country it levels in
    // and not back to where it began. (A level-23 character on Kalimdor's mainland hearthed to Azuremyst Isle.)
    // Only in the zone it levels in (`ownZone`, 0: anywhere): the hearthstone is its way back from an errand (HearthHome
    // from the zone travel), and an inn next to an errand's ender (Thelsamar for a hunter of the Badlands) is not home.
    void BindAtHubInn(Player* player, uint32 ownZone)
    {
        if (ownZone && player->GetZoneId() != ownZone)
            return;
        constexpr float InnYards = 120.0f;       // an innkeeper this near the hand-in
        constexpr float HomeFarYards = 1500.0f;  // and its home is farther from that inn than this
        ServicePoint const* inn = nullptr;
        float best = InnYards;
        for (ServicePoint const& service : sWorldIndex.GetServices(ServiceType::Innkeeper))
        {
            if (service.spawn.mapId != player->GetMapId() || std::fabs(service.spawn.z - player->GetPositionZ()) > 30.0f)
                continue;
            float const distance = Dist2d(player, service.spawn);
            if (distance < best && WorldIndex::IsFriendlyFaction(service.factionTemplate, player))
            {
                inn = &service;
                best = distance;
            }
        }
        if (!inn)
            return;
        if (player->m_homebindMapId == inn->spawn.mapId)
        {
            float const dx = player->m_homebindX - inn->spawn.x, dy = player->m_homebindY - inn->spawn.y;
            if (dx * dx + dy * dy < HomeFarYards * HomeFarYards)
                return;
        }
        WorldLocation const home(inn->spawn.mapId, inn->spawn.x, inn->spawn.y, inn->spawn.z, player->GetOrientation());
        player->SetHomebind(home, player->GetAreaId());
        LOG_INFO(ActivityLog, "activity: {} makes the inn of {} in {} its home: its hearthstone leads there now", player->GetName(),
            inn->name, ZoneName(player->GetZoneId()));
    }
}

bool OrderService::StartTrailOut(Player* player, BotRuntime& rt, std::string const& why)
{
    constexpr float MinBackYards = 50.0f;  // a trail shorter than this leads nowhere new
    constexpr float SkipYards = 5.0f;      // points this near the bot are passed
    constexpr uint32 AgainMs = 15 * MINUTE * IN_MILLISECONDS;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if ((rt.trailOutUsedAt && getMSTimeDiff(rt.trailOutUsedAt, now) < AgainMs) || !player->IsAlive() || player->IsInCombat() ||
        player->IsInFlight() || player->GetTransport() || rt.squad.following)
        return false;
    std::vector<BotRuntime::TrailPoint> way;
    float farthest = 0.0f;
    for (auto it = rt.trail.rbegin(); it != rt.trail.rend(); ++it)
    {
        if (it->mapId != player->GetMapId())
            break;
        float const apart = Dist2d(player, it->x, it->y);
        if (way.empty() && apart <= SkipYards)
            continue;
        way.push_back(*it);
        farthest = std::max(farthest, apart);
    }
    if (way.empty() || farthest < MinBackYards)
        return false;
    LOG_INFO(ActivityLog, "activity: {} walks back the way it came ({} points, {:.0f} yd from here at most) before anything else: {}",
        player->GetName(), uint32(way.size()), farthest, why);
    if (rt.task.kind != TaskKind::None)
        ClearTask(rt, 0, "walks back the way it came");
    BotDriver::Rest(player);
    rt.trailOut = std::move(way);
    rt.trailOutIndex = 0;
    rt.trailOutSince = now;
    rt.trailOutMovedAt = now;
    rt.trailOutLastX = player->GetPositionX();
    rt.trailOutLastY = player->GetPositionY();
    rt.trailOutUsedAt = now;
    rt.why = "Walking back the way it came";
    return true;
}

bool OrderService::CutOffWayOut(Player* player, BotRuntime& rt, char const* what)
{
    // The way it came first: it walked in, so it can walk out (the owner: rescues are a workaround). A mage that had mined
    // an ooze-covered deposit inside Hive'Ashi in Silithus followed an incomplete path towards its next vein to where
    // that path ended, a pocket 18 yd under the surface from which the path finder leads nowhere, not even back, and was
    // "rescued" there, round 146. What led it in is set aside, so the walk out is not followed by the same walk in.
    constexpr uint32 CutOffAvoidMs = 30 * MINUTE * IN_MILLISECONDS;
    std::string const target = rt.task.targetName;
    // The whole navmesh may have the way off a mesa the path finder does not see (Thousand Needles, 2026-11-25: two
    // characters that had walked up the Grimtotem mesas by the navmesh's way were "cut off" there on their next task and
    // rescued down to the canyon): it walks that way to its task's spot, once per task.
    constexpr float OpenWayYards = 4000.0f;
    BotTask& task = rt.task;
    // On a floor under water that leads nowhere (the Undercity's canals): out along the surface, as a player swims.
    if (task.kind != TaskKind::None && WadeOut(player, task, task.hasSpot ? task.spot.x : player->GetPositionX(),
            task.hasSpot ? task.spot.y : player->GetPositionY(), task.hasSpot ? task.spot.z : player->GetPositionZ(), what))
        return true;
    std::string navWhy;
    if (task.hasSpot && !player->GetMap()->Instanceable() && task.OpenTryDue(player->GetPositionX(), player->GetPositionY()))
    {
        float length = 0.0f;
        std::vector<NavPoint> corners;
        std::string unsafe;
        if (!sDungeonNav.FindWay(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), task.spot.x,
                task.spot.y, task.spot.z, &corners, &length, player->GetMap(), &navWhy))
            LOG_INFO(ActivityLog, "activity: {} {}{}{}: the navmesh has no way there either from ({:.0f}, {:.0f}, {:.0f}): {}",
                player->GetName(), what, target.empty() ? "" : " on its way to ", target, player->GetPositionX(), player->GetPositionY(),
                player->GetPositionZ(), navWhy);
        else if (length <= OpenWayYards && !NavWayIsSafe(player, rt, corners, unsafe))
            LOG_INFO(ActivityLog, "activity: {} {}{}{}: the navmesh's way from ({:.0f}, {:.0f}, {:.0f}) ({:.0f} yd) does not do: {}",
                player->GetName(), what, target.empty() ? "" : " on its way to ", target, player->GetPositionX(), player->GetPositionY(),
                player->GetPositionZ(), length, unsafe);
        else if (length <= OpenWayYards)
        {
            task.openWay = true;
            task.navActive = false;
            task.ownSteps = false;
            task.stallSince = 0;
            task.routePlanned = false;
            task.route.found = false;
            task.walkX = task.walkY = 0.0f;
            task.nearSince = 0;
            rt.stuckSince = 0;
            LOG_INFO(ActivityLog, "activity: {} {}{}{}: the navmesh has a way of {:.0f} yd there ({:.0f} yd straight), and it walks that way",
                player->GetName(), what, target.empty() ? "" : " on its way to ", target, length,
                player->GetExactDist(task.spot.x, task.spot.y, task.spot.z));
            return true;
        }
    }
    if (rt.task.hasSpot)
        rt.Avoid(rt.task.spot, CutOffAvoidMs);
    std::string const why = Acore::StringFormat("it {}{}{}", what, target.empty() ? "" : " on its way to ", target);
    // A step down off a ledge first: on a rock top the way it came may be a fight's (the trail is kept out of fights) or
    // none at all after a login, and a walk back along the path finder's path ends at the rock's edge (round 174).
    if (StepOffLedge(player, rt, why))
        return true;
    if (StartTrailOut(player, rt, why))
    {
        rt.trailOutForCutOff = true;
        return true;
    }
    return Rescue(player, rt, what, 30.0f);
}

bool OrderService::LiftOutOfGround(Player* player, BotRuntime& rt, std::string const& why)
{
    // Two characters fought Blighted Surges at the shore of Lake Mereldar, one of which spawns 3 yd under the lake's
    // sloping floor, ended 3 to 4 yd inside the ground and were rescued to a road 200 yd away, rounds 183 and 184. A bot
    // that a few yards of terrain cover, with no floor under it, is put on the terrain right above it: where it was meant
    // to stand. Once in two minutes; a bot deeper down, or one whose terrain above has no floor of its own, goes on to
    // the rest of the ways out.
    constexpr uint32 AgainMs = 2 * MINUTE * IN_MILLISECONDS;
    constexpr float MaxDepth = 8.0f;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if ((rt.liftedAt && getMSTimeDiff(rt.liftedAt, now) < AgainMs) || !player->IsAlive() || player->GetTransport() ||
        player->GetVehicle() || player->IsInFlight() || player->GetMap()->Instanceable())
        return false;
    Map* map = player->GetMap();
    uint32 const phase = player->GetPhaseMask();
    float const x = player->GetPositionX();
    float const y = player->GetPositionY();
    float const z = player->GetPositionZ();
    float const terrain = map->GetGridHeight(x, y);
    if (terrain <= INVALID_HEIGHT || terrain - z < 1.0f || terrain - z > MaxDepth ||
        map->GetHeight(phase, x, y, z + 2.0f, true, 100.0f) > INVALID_HEIGHT)
        return false;
    // The terrain's own surface there, and not a roof over it (a rock or a building on the terrain).
    float const ground = map->GetHeight(phase, x, y, terrain + 1.0f, true, 3.0f);
    if (ground <= INVALID_HEIGHT || std::fabs(ground - terrain) > 1.5f)
        return false;
    rt.liftedAt = now;
    // (With the last walk the ground check let through: how it got there.)
    std::string const passed = sGroundGuard.DescribeLastPassed(player->GetGUID());
    LOG_INFO(ActivityLog, "activity: {} stands {:.1f} yd inside the ground at ({:.0f}, {:.0f}, {:.0f}) map {}: it is put on the ground "
        "right above it ({:.0f}); {}{}{}", player->GetName(), terrain - z, x, y, z, player->GetMapId(), ground, why,
        passed.empty() ? "" : "; last walk the ground check let through ", passed);
    player->NearTeleportTo(x, y, ground + 0.5f, player->GetOrientation());
    rt.strandSince = now;
    return true;
}

namespace
{
    // The way down a slope along its ground (StepOffLedge): a point every 1.5 yd at the floor just below the one before,
    // a rise of at most 1 yd and a drop of at most 4 yd a point, no water, nothing in the way, ending at the height of
    // its end. A death knight that a Stitched Horror's Mighty Blow knocked off the plateau above the Infectis Scar stood
    // on the slope below it, off the navmesh, and the straight line down to the scar's floor ran through the air above
    // the slope's bend: it was rescued, round 272.
    bool SlopeWalk(Map* map, uint32 phase, float collisionHeight, uint32 mapId, float px, float py, float pz, float x, float y,
        float z, std::vector<BotRuntime::TrailPoint>& out)
    {
        constexpr float Step = 1.5f;
        constexpr float MaxRise = 1.0f;
        constexpr float MaxDrop = 4.0f;
        out.clear();
        float const length = std::sqrt((x - px) * (x - px) + (y - py) * (y - py));
        uint32 const steps = std::max<uint32>(1, uint32(std::ceil(length / Step)));
        float lx = px, ly = py, lz = pz;
        for (uint32 i = 1; i <= steps; ++i)
        {
            float const t = float(i) / float(steps);
            float const sx = px + (x - px) * t;
            float const sy = py + (y - py) * t;
            float const sz = map->GetHeight(phase, sx, sy, lz + MaxRise + 0.5f, true, MaxRise + MaxDrop + 1.0f);
            if (sz <= INVALID_HEIGHT || sz - lz > MaxRise || lz - sz > MaxDrop ||
                map->IsInWater(phase, sx, sy, sz, collisionHeight) ||
                !map->isInLineOfSight(lx, ly, lz + 1.5f, sx, sy, sz + 1.5f, phase, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::M2))
                return false;
            out.push_back({ mapId, sx, sy, sz });
            lx = sx;
            ly = sy;
            lz = sz;
        }
        return std::fabs(lz - z) <= 1.5f;
    }
}

bool OrderService::StepOffLedge(Player* player, BotRuntime& rt, std::string const& why)
{
    // A paladin grinding in the Eastern Plaguelands ended a fight on top of a rock beside Terrordale, 7 to 15 yd above the
    // ground around it: the navmesh covers the top, but no polygon joins it to the slope below, so every path from there
    // is "incomplete" and ends at the top's edge. Its south-west edge is 3 yd above the ground beyond it, a step down any
    // walker takes. It entered the world there after a restart, its trail 12 yd long, and was rescued, round 174.
    constexpr uint32 AgainMs = 15 * MINUTE * IN_MILLISECONDS;
    constexpr float MaxDrop = 5.0f;      // yards through the air the step may run (the ground guard's drop rule begins at 6)
    constexpr float SlopeDrop = 30.0f;   // how much lower the floor may lie when the step runs down a slope on the ground
    constexpr float MaxRise = 1.5f;
    constexpr int Angles = 16;
    constexpr uint32 MaxSearches = 120;  // path searches at most (the answers are cached by the walk cache)
    static constexpr float Radii[] = { 6.0f, 9.0f, 12.0f, 15.0f, 18.0f, 22.0f, 26.0f };
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if ((rt.ledgeStepAt && getMSTimeDiff(rt.ledgeStepAt, now) < AgainMs) || !player->IsAlive() || player->IsInCombat() ||
        player->IsInFlight() || player->GetTransport() || player->GetVehicle() || IsSwimming(player) || rt.squad.following)
        return false;
    Map* map = player->GetMap();
    uint32 const phase = player->GetPhaseMask();
    float const px = player->GetPositionX();
    float const py = player->GetPositionY();
    float const pz = player->GetPositionZ();
    // The roads it should get to: the nearest nodes of the road map that stand on the navmesh, beyond a few steps.
    std::vector<RoutePoint> roads;
    float minDistance = 30.0f;
    for (int asked = 0, skipped = 0; asked < 3 && skipped <= 8;)
    {
        RoutePoint node;
        std::string name;
        if (!sRoadMap.FindNearestNode(player->GetMapId(), px, py, minDistance, RescueRange, node, name))
            break;
        minDistance = std::sqrt((node.x - px) * (node.x - px) + (node.y - py) * (node.y - py)) + 10.0f;
        if (!NodeOnNavmesh(player, node))
        {
            ++skipped;
            continue;
        }
        roads.push_back(node);
        ++asked;
    }
    if (roads.empty())
        return false;
    uint32 searches = 0;
    for (float const radius : Radii)
        for (int i = 0; i < Angles && searches < MaxSearches; ++i)
        {
            float const angle = float(i) * 2.0f * float(M_PI) / float(Angles);
            float const x = px + std::cos(angle) * radius;
            float const y = py + std::sin(angle) * radius;
            // The floor there, looked for from just above the bot's height down a slope (whether the step runs on the
            // ground or off a ledge through the air is the probe's below).
            float const z = map->GetHeight(phase, x, y, pz + MaxRise + 0.5f, true, MaxRise + SlopeDrop + 0.5f);
            if (z <= INVALID_HEIGHT || pz - z > SlopeDrop || z - pz > MaxRise ||
                map->IsInWater(phase, x, y, z, player->GetCollisionHeight()))
                continue;
            bool reaches = false;
            for (RoutePoint const& road : roads)
            {
                ++searches;
                if (WalkReaches(player, x, y, z, road.x, road.y, road.z))
                {
                    reaches = true;
                    break;
                }
                if (searches >= MaxSearches)
                    break;
            }
            if (!reaches)
                continue;
            // The step itself, a straight line as it is walked: on the ground, through no rock, off no high ledge. Or,
            // down a slope, along its ground (SlopeWalk).
            PathGenerator path(player);
            path.CalculatePath(x, y, z, false);
            PathReport const step = ProbeMovement(player, path, false, x, y, z);
            std::vector<BotRuntime::TrailPoint> slope;
            bool const straight = step.valid && !step.LeavesTheGround() && step.throughSolid <= 1.0f &&
                step.offGround - step.throughSolid <= MaxDrop + 1.0f;
            if (!straight && (pz - z < 2.0f ||
                    !SlopeWalk(map, phase, player->GetCollisionHeight(), player->GetMapId(), px, py, pz, x, y, z, slope)))
                continue;
            LOG_INFO(ActivityLog, "activity: {} steps down off the ledge it stands on at ({:.0f}, {:.0f}, {:.0f}): {:.0f} yd to ({:.0f}, {:.0f}, "
                "{:.0f}), {:.1f} yd {}{}, from where the path finder walks to a road; {}", player->GetName(), px, py, pz, radius, x, y, z,
                std::fabs(pz - z), pz >= z ? "lower" : "higher", straight ? "" : ", down the slope along its ground", why);
            if (rt.task.kind != TaskKind::None)
                ClearTask(rt, 0, "steps down off a ledge");
            BotDriver::Rest(player);
            rt.trailOut.clear();
            if (straight)
                rt.trailOut.push_back({ player->GetMapId(), x, y, z });
            else
                rt.trailOut = std::move(slope);
            rt.trailOutIndex = 0;
            rt.trailOutSince = now;
            rt.trailOutMovedAt = now;
            rt.trailOutLastX = px;
            rt.trailOutLastY = py;
            rt.trailOutStraight = true;
            rt.trailOutForCutOff = true;
            rt.ledgeStepAt = now;
            rt.why = "Stepping down off a ledge";
            return true;
        }
    return false;
}

bool OrderService::StepTrailOut(Player* player, BotRuntime& rt)
{
    constexpr uint32 MaxMs = 90 * IN_MILLISECONDS;
    constexpr uint32 NoHeadwayMs = 8 * IN_MILLISECONDS;
    constexpr float AtPoint = 4.0f;
    if (rt.trailOut.empty())
        return false;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    auto stop = [&](char const* how, bool back = false)
    {
        LOG_INFO(ActivityLog, "activity: {} ends its {} after {} s: {}", player->GetName(),
            rt.trailOutStraight ? "step down off the ledge" : "walk back the way it came", getMSTimeDiff(rt.trailOutSince, now) / IN_MILLISECONDS,
            back && rt.trailOutStraight ? "down" : how);
        rt.trailOut.clear();
        rt.trailOutStraight = false;
        rt.trailOutIndex = 0;
        // A new place: its ways and its zone travels are counted anew from here.
        rt.zoneTravelStarts = 0;
        rt.boxedRefusals = 0;
        rt.boxedSince = 0;
        // The way out of a place cut off from every road that did not get back where it came from: the rescue now, as
        // before the walk (a mage that walked 30 yd back inside Hive'Ashi and stood there, its travels refused and its
        // walks to a safe spot ending at once, for eight minutes, round 146).
        bool const rescue = rt.trailOutForCutOff && !back && player->IsAlive() && !player->IsInCombat() && !player->IsInFlight() &&
            !player->GetTransport() && !player->IsBeingTeleported();
        rt.trailOutForCutOff = false;
        if (rescue && IsCutOff(player))
            Rescue(player, rt, "is cut off from every road, and its way back did not lead out", 30.0f);
    };
    if (!player->IsAlive() || player->IsInCombat() || player->IsBeingTeleported() || player->IsInFlight() || player->GetTransport())
    {
        stop(player->IsInCombat() ? "a fight" : "it moved on otherwise");
        return false;
    }
    if (getMSTimeDiff(rt.trailOutSince, now) > MaxMs)
    {
        stop("time over");
        return false;
    }
    while (rt.trailOutIndex < rt.trailOut.size() &&
        Dist2d(player, rt.trailOut[rt.trailOutIndex].x, rt.trailOut[rt.trailOutIndex].y) <= AtPoint)
        ++rt.trailOutIndex;
    if (rt.trailOutIndex >= rt.trailOut.size())
    {
        stop("back where it had come from", true);
        return false;
    }
    if (Dist2d(player, rt.trailOutLastX, rt.trailOutLastY) > 2.0f)
    {
        rt.trailOutLastX = player->GetPositionX();
        rt.trailOutLastY = player->GetPositionY();
        rt.trailOutMovedAt = now;
    }
    else if (getMSTimeDiff(rt.trailOutMovedAt, now) > NoHeadwayMs)
    {
        stop("it does not get on");
        return false;
    }
    BotRuntime::TrailPoint const& point = rt.trailOut[rt.trailOutIndex];
    BotDriver::PauseAI(player, 2 * IN_MILLISECONDS);
    // (A step down off a ledge is the probed straight line: the path finder's path ends at the ledge's edge.)
    if (!player->isMoving())
        player->GetMotionMaster()->MovePoint(0, point.x, point.y, point.z, FORCED_MOVEMENT_NONE, 0.0f, 0.0f, !rt.trailOutStraight);
    rt.why = rt.trailOutStraight ? "Stepping down off a ledge" : "Walking back the way it came";
    return true;
}

bool OrderService::StepMemberVendorVisit(GuildBot const& bot, Player* player, BotRuntime& rt)
{
    constexpr uint32 VisitMaxMs = 45 * IN_MILLISECONDS;
    constexpr float AtVendor = INTERACTION_DISTANCE - 1.0f;
    if (rt.vendorVisit.IsEmpty())
        return false;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    Creature* vendor = ObjectAccessor::GetCreature(*player, rt.vendorVisit);
    auto finish = [&](char const* how)
    {
        // A trip made for this member that freed nothing: its leader does not make it again for half an hour (a druid
        // went to a vendor for its member every two minutes, eleven times: the member's bags held worn soulbound gear
        // that the leader's visit did not sell for it).
        uint32 const freeNow = player->GetFreeInventorySpace();
        if (freeNow <= std::max<uint32>(rt.vendorVisitFree, UrgentFreeSlots))
            if (auto leaderRt = _runtime.find(rt.vendorVisitLeader); leaderRt != _runtime.end())
            {
                leaderRt->second.nextMemberTrip = now + 30 * MINUTE * IN_MILLISECONDS;
                LOG_INFO(ActivityLog, "activity: {}'s visit at the vendor freed nothing in its bags ({} free slots): its squad leader's "
                    "next trip for it waits 30 min ({})", bot.name, freeNow, how);
            }
        rt.vendorVisit.Clear();
        rt.vendorVisitLeader.Clear();
        rt.vendorVisitSince = 0;
    };
    if (!vendor || !vendor->IsAlive() || !player->IsAlive() || player->IsInCombat() || vendor->GetMapId() != player->GetMapId() ||
        getMSTimeDiff(rt.vendorVisitSince, now) > VisitMaxMs)
    {
        finish(vendor ? "it did not get to the vendor" : "the vendor is gone");
        return false;
    }
    if (!player->IsWithinDist(vendor, AtVendor))
    {
        // It walks there itself (it follows its leader otherwise): the core's path, the AI held meanwhile.
        BotDriver::PauseAI(player, 2 * IN_MILLISECONDS);
        if (!player->isMoving())
        {
            float x, y, z;
            vendor->GetNearPoint(player, x, y, z, player->GetCombatReach(), 1.5f, vendor->GetAngle(player));
            player->GetMotionMaster()->MovePoint(0, x, y, z);
        }
        rt.why = Acore::StringFormat("Squad: selling at {}", vendor->GetName());
        return true;
    }
    player->StopMoving();
    // Its own visit: worn gear it will not wear again (the storage's rule, WG1), a repair at an armorer, room.
    sLootService.HandleWornGear(player, vendor);
    if (vendor->IsArmorer() && MinDurabilityPercent(player) < RepairBelowPercent)
    {
        uint32 const before = MinDurabilityPercent(player);
        uint32 const money = player->GetMoney();
        player->DurabilityRepairAll(true, player->GetReputationPriceDiscount(vendor), false);
        if (money > player->GetMoney())
            LOG_INFO(ActivityLog, "activity: {} repaired its gear at {} beside its squad leader for {}c: durability {}% -> {}%", bot.name,
                vendor->GetName(), money - player->GetMoney(), before, MinDurabilityPercent(player));
    }
    if (player->GetFreeInventorySpace() <= SellAtFreeSlots)
        MakeBagSpace(bot, player);
    LOG_INFO(ActivityLog, "activity: {} visited {} with its squad leader: {} free bag slots (was {})", bot.name, vendor->GetName(),
        player->GetFreeInventorySpace(), rt.vendorVisitFree);
    finish("visited");
    return false;
}

bool OrderService::SetAsideOtherLevel(AdventureGuild const& guild, GuildBot const& bot, Player* player, BotRuntime& rt,
    uint32 mapId, float x, float y)
{
    constexpr float TowardsYards = 80.0f;    // the refused step leads this near to the target
    constexpr float OtherLevelYards = 8.0f;  // the target stands this much above or below the bot
    constexpr float RoadNodeYards = 300.0f;  // a node of the road map this near to the target is asked for a way there
    constexpr uint32 LastingMs = 2 * HOUR * IN_MILLISECONDS;
    BotTask& task = rt.task;
    switch (task.kind)
    {
        case TaskKind::Pickup:
        case TaskKind::TurnIn:
        case TaskKind::Vendor:
        case TaskKind::Train:
        case TaskKind::Discover:
        case TaskKind::Event:
        case TaskKind::Objective:
        case TaskKind::Interact:
        case TaskKind::Grind:
            break;
        // The walk of the profession mode to a node (not to a forge, a hunting ground or the water): a mage walked to a
        // Truesilver Deposit in the floor of the Infectis Scar, stood on its rim 46 yd above it for two minutes, its
        // steps refused, and gave it up, and the next miner did the same, round 149.
        case TaskKind::Travel:
            if (!task.gatherRound || task.focus || task.skinHunt || task.huntItem || task.fishTrip)
                return false;
            break;
        default:
            return false;
    }
    if (!task.hasSpot || task.spot.mapId != mapId || mapId != player->GetMapId() || player->GetTransport() || IsSwimming(player) ||
        IsDiving(player) || Dist2d(x, y, task.spot.x, task.spot.y) > TowardsYards)
        return false;
    float const dz = task.spot.z - player->GetPositionZ();
    if (std::fabs(dz) < OtherLevelYards ||
        WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), task.spot.x, task.spot.y, task.spot.z))
        return false;
    // A bot that stands off the walkable ground itself: the usual rules (no target is the problem then).
    if (IsOffNavmesh(player))
        return false;
    // A walk along the whole navmesh's way under way (below): a refused step is its way's to deal with (ViaDungeonPath walks
    // that leg along the navmesh's surface after a refusal), not a sign that no way leads there, a few times. The walk
    // used to end at its first refusal: the hand-in at Apothecary Zinge in the Undercity's Apothecarium, 27 yd below the
    // ring the bots stood on, "the navmesh has a way of 255 yd there, and it walks that way", then 6 to 19 s later "walks
    // to Grand Apothecary Putress first" and "set aside", three times within a day (Graveborn, Morthos, 2026-11-26), and
    // the guild left Zinge alone for a day.
    constexpr uint8 OpenRefusalsMax = 4;
    if (task.openWay && task.openRefusals < OpenRefusalsMax)
    {
        ++task.openRefusals;
        task.navRefusedAt = std::max<uint32>(getMSTime(), 1); // the caller takes the guard's refusals
        return true;
    }
    // The water may lead there: the swim is looked for first (StepSwim, once per task), and only a target it finds no
    // swim to is set aside. (A shaman on a cliff of Feralas had every Sea Spray of quest 7721 set aside, each "on a level
    // the path finder has no way to": they float in the sea below, where the navmesh ends.)
    // (Armed once per task, and only for a target the swim looks at: from 30 yd, StepSwim. A swim armed and not looked at
    // by the next refusal never will be: the step was NewRpg's own, not one of the planner's walks. A shaman stood three
    // minutes in the Valley of Trials under a Scorpid Worker on the rock 34 yd above it, 4.6 yd away on the map: its
    // straight step up was refused every half minute, and each refusal armed the swim again and was taken as handled.)
    constexpr float SwimMinYards = 30.0f; // StepSwim's MinYards
    if (!task.swimLooked && !task.swimArmed && Dist2d(player, task.spot.x, task.spot.y) >= SwimMinYards)
    {
        task.swimArmed = true;
        // (An objective's spot within sight is NewRpg's to walk to, and StepSwim looks only in the planner's walks: the
        // planner takes the walk over until it is within a few yards. Mojowind, 41 yd from a Drysnap Crawler 15 yd down
        // behind a rock of Shadowprey's bay, had the swim armed, never looked at, and the spot set aside at the next
        // refusal, Clam Bait on the j5 instance, 2026-11-26.)
        constexpr float SwimApproachDone = 8.0f;
        if (task.kind == TaskKind::Objective && !task.approach)
        {
            task.tookOver = true;
            task.approach = true;
            task.approachDone = SwimApproachDone;
            task.routePlanned = false;
            rt.stuckSince = 0;
        }
        return true;
    }
    // The whole navmesh may have the way (round a mesa: the core's path finder sees 148 polygons of loaded grids): the
    // bot walks it, once per task. (Thousand Needles, 2026-11-25: the Document Chests of "Grimtotem Spying" on the
    // Grimtotem mesas, 118 yd above the canyon floor and 1470 yd from it by the way up; the test guild's leader set them
    // aside and rode back to Freewind Post.)
    constexpr float OpenWayYards = 2500.0f;
    if (task.OpenTryDue(player->GetPositionX(), player->GetPositionY()))
    {
        float length = 0.0f;
        std::string navWhy;
        std::vector<NavPoint> corners;
        std::string unsafe;
        if (!sDungeonNav.FindWay(mapId, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), task.spot.x, task.spot.y,
                task.spot.z, &corners, &length, player->GetMap(), &navWhy))
            LOG_INFO(ActivityLog, "activity: {} cannot get to {} with the path finder from ({:.0f}, {:.0f}, {:.0f}), and the navmesh has no "
                "way either: {}", bot.name, task.targetName, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), navWhy);
        else if (length <= OpenWayYards && !NavWayIsSafe(player, rt, corners, unsafe))
            LOG_INFO(ActivityLog, "activity: {} cannot get to {} with the path finder from ({:.0f}, {:.0f}, {:.0f}), and the navmesh's way "
                "({:.0f} yd) does not do: {}", bot.name, task.targetName, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                length, unsafe);
        else if (length <= OpenWayYards)
        {
            task.openWay = true;
            task.navActive = false;
            task.ownSteps = false;
            task.stallSince = 0;
            task.routePlanned = false;
            task.route.found = false;
            task.walkX = task.walkY = 0.0f;
            task.nearSince = 0;
            rt.stuckSince = 0;
            LOG_INFO(ActivityLog, "activity: {} cannot get to {} ({}, {:.0f} yd {} it) with the path finder from where it stands: the "
                "navmesh has a way of {:.0f} yd there ({:.0f} yd straight), and it walks that way", bot.name, task.targetName, ToString(task.kind),
                std::fabs(dz), dz > 0.0f ? "above" : "below", length, player->GetExactDist(task.spot.x, task.spot.y, task.spot.z));
            return true;
        }
    }
    RoutePoint node;
    std::string nodeName;
    bool const fromRoad = sRoadMap.FindNearestNode(mapId, task.spot.x, task.spot.y, 0.0f, RoadNodeYards, node, nodeName,
        player->GetTeamId(), player->GetLevel()) && WalkReaches(player, node.x, node.y, node.z, task.spot.x, task.spot.y, task.spot.z);
    // From a road node the path finder has a way there (a balcony of the Sunspire on Sunstrider Isle: the bot's own steps
    // ended below it, and the hand-in at Well Watcher Solanian was set aside for 20 minutes, then tried and set aside
    // again): the bot walks to that node first and goes on from there, once per task.
    if (fromRoad && !task.viaTried && Dist2d(player, node.x, node.y) > 10.0f)
    {
        task.viaTried = true;
        task.viaActive = true;
        task.viaX = node.x;
        task.viaY = node.y;
        task.viaZ = node.z;
        task.viaName = nodeName;
        task.ownSteps = false;
        task.stallSince = 0;
        task.routePlanned = false;
        task.route.found = false;
        task.walkX = task.walkY = 0.0f;
        task.nearSince = 0; // an NPC's task walks again (StepNpcTask), not NewRpg's visit from below
        rt.stuckSince = 0;
        LOG_INFO(ActivityLog, "activity: {} cannot get to {} ({}, {:.0f} yd {} it) from where it stands: it walks to {} first, from where "
            "the path finder has a way there", bot.name, task.targetName, ToString(task.kind), std::fabs(dz), dz > 0.0f ? "above" : "below",
            nodeName);
        return true;
    }
    uint32 const ms = fromRoad ? AvoidSpawnMs : LastingMs;
    LOG_INFO(ActivityLog, "activity: {} cannot get to {} at ({:.0f}, {:.0f}, {:.0f}) map {} ({}): it stands {:.0f} yd {} it, on a level "
        "the path finder has no way to from where it stands{}; set aside for {} min", bot.name, task.targetName, task.spot.x, task.spot.y,
        task.spot.z, mapId, ToString(task.kind), std::fabs(dz), dz > 0.0f ? "above" : "below",
        fromRoad ? Acore::StringFormat(" (from {} there is one)", nodeName) : std::string(", nor from the road map near it"),
        ms / (MINUTE * IN_MILLISECONDS));
    uint32 const now = getMSTime();
    bool const area = task.kind == TaskKind::Objective || task.kind == TaskKind::Grind || task.kind == TaskKind::Interact ||
        task.kind == TaskKind::Discover || !task.spot.spawnId;
    if (area)
    {
        rt.AddAvoidArea(mapId, task.spot.x, task.spot.y, 255, ms, false);
        if (!fromRoad)
            if (uint32 const key = LastingAreaKey(mapId, task.spot.x, task.spot.y))
                rt.lastingAvoids.push_back({ LastingArea, key, now + ms });
    }
    if (task.spot.spawnId)
    {
        rt.Avoid(task.spot, ms);
        if (!fromRoad && task.spot.spawnId <= 0xFFFFFFFFull)
            rt.lastingAvoids.push_back({ task.spot.gameObject ? LastingObject : LastingCreature, uint32(task.spot.spawnId), now + ms });
    }
    NoteNpcUnreached(guild, bot.name, task);
    ClearTask(rt, 5 * IN_MILLISECONDS, "no way to its level");
    BotDriver::Rest(player);
    return true;
}

bool OrderService::RetryRefusedWalk(GuildBot const& bot, Player* player, BotRuntime& rt, bool noWay)
{
    BotTask& task = rt.task;
    if (!noWay || !task.hasSpot || task.route.found || task.ownSteps || player->GetTransport() || IsSwimming(player) ||
        IsDiving(player) || task.spot.mapId != player->GetMapId())
        return false;
    // A short walk that went straight because the path finder had a whole path from where it was asked: asked again from
    // here, and without a whole path the road map is asked (the owner: no walk goes blindly straight). (The guild leader landed
    // at Theramore's flight master, walked towards its ship straight, met a step that ran 156 yd through the air beside the
    // pier, and was put at the graveyard as "cut off"; the road map has the way over the pier, 382 yd.)
    if (task.shortProbed && !task.shortNoPath && Dist2d(player, task.shortFromX, task.shortFromY) > 5.0f)
    {
        task.shortProbed = false;
        return true;
    }
    // A walk whose target the path finder has a whole way to, and whose straight step NewRpg makes is refused: the planner
    // walks the bot along that way itself at once, not after standing (the guild leader on the Searing Gorge's rim, its
    // objective 170 yd below: the refused drop, 24 s standing, then the route down).
    if (!WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), task.spot.x, task.spot.y,
        task.spot.z))
        return false;
    LOG_INFO(ActivityLog, "activity: {} has its straight step towards {} ({}) refused: the planner walks it along the path itself",
        bot.name, task.targetName.empty() ? std::string("its target") : task.targetName, ToString(task.kind));
    task.ownSteps = true;
    task.ownFailStreak = 0;
    task.ownStepAt = 0;
    if (task.kind == TaskKind::Objective)
    {
        task.tookOver = true;
        task.approach = true;
        task.approachDone = SightDistance;
    }
    task.routePlanned = false;
    rt.stuckSince = 0;
    return true;
}

bool OrderService::TravelHearthAllowed(Player const* player) const
{
    // (the owner, 2026-11-14: [quote left out]. On the gaps instance a test character
    // hearthed from Dun Morogh to its inn in Redridge for its zone.)
    switch (sBotSettings.Get(player, Setting::TravelHearthstone))
    {
        case 0:
            return false;
        case 2:
            return true;
        default:
            break;
    }
    if (player->m_homebindMapId != player->GetMapId())
        return true;
    uint32 const innZone = sMapMgr->GetZoneId(PHASEMASK_NORMAL, player->m_homebindMapId, player->m_homebindX, player->m_homebindY,
        player->m_homebindZ);
    // The capital cities of each side.
    static constexpr uint32 AllianceCapitals[] = { 1519, 1537, 1657, 3557 }; // Stormwind, Ironforge, Darnassus, the Exodar
    static constexpr uint32 HordeCapitals[] = { 1637, 1497, 1638, 3487 };    // Orgrimmar, Undercity, Thunder Bluff, Silvermoon
    for (uint32 capital : player->GetTeamId() == TEAM_ALLIANCE ? AllianceCapitals : HordeCapitals)
        if (innZone == capital)
            return true;
    return false;
}

bool OrderService::HearthTowards(Player* player, BotRuntime& rt, uint32 mapId, float x, float y, std::string const& reason)
{
    constexpr float NearYards = 400.0f; // the inn this near to where the bot is going
    if (player->m_homebindMapId != mapId || !TravelHearthAllowed(player))
        return false;
    float const dx = player->m_homebindX - x, dy = player->m_homebindY - y;
    if (dx * dx + dy * dy > NearYards * NearYards)
        return false;
    return HearthHome(player, rt, reason, DefaultHearthYards);
}

bool OrderService::HearthHome(Player* player, BotRuntime& rt, std::string const& reason, float minYards)
{
    if (player->GetMap()->IsDungeon() || !player->IsAlive() || player->IsInCombat() || player->IsInFlight() || player->GetTransport() ||
        !player->HasItemCount(HearthstoneItem, 1) || player->HasSpellCooldown(HearthstoneSpell))
        return false;
    if (player->GetMapId() == player->m_homebindMapId &&
        player->GetDistance2d(player->m_homebindX, player->m_homebindY) < minYards)
        return false;
    LOG_INFO(ActivityLog, "activity: {} uses its hearthstone ({}): from ({:.0f}, {:.0f}) map {} to its inn at ({:.0f}, {:.0f}) map {}",
        player->GetName(), reason, player->GetPositionX(), player->GetPositionY(), player->GetMapId(), player->m_homebindX,
        player->m_homebindY, player->m_homebindMapId);
    BotDriver::Rest(player);
    ClearTask(rt, 10 * IN_MILLISECONDS, "hearthstone");
    // The result of the hearthstone, with its cooldown; the ten seconds of casting are the pause above.
    player->AddSpellCooldown(HearthstoneSpell, HearthstoneItem, HearthstoneCooldownMs);
    // From the inn the ways are other ways. Not for the zone whose way was just refused, though: the refusal was at
    // that zone's door as often as at the bot's (a level-35 druid flew from Booty Bay to Thelsamar for the Badlands,
    // was refused at the pass, hearthed back, picked the Badlands again six seconds later and flew to Thelsamar again).
    // It waits a while longer; the others are open again.
    {
        constexpr uint32 RefusedZoneAfterHearthMs = 10 * MINUTE * IN_MILLISECONDS;
        uint32 const hearthNow = getMSTime();
        auto const refused = rt.unreachableZones.find(rt.targetZone);
        bool const keep = rt.targetZone && refused != rt.unreachableZones.end() && refused->second > hearthNow;
        rt.unreachableZones.clear();
        if (keep)
            rt.unreachableZones[rt.targetZone] = hearthNow + RefusedZoneAfterHearthMs;
    }
    rt.travelStuckZone = 0;
    rt.travelStuckCount = 0;
    rt.zoneChosenAt = 0;
    // (To another continent it is a crossing like a voyage: the bot stays on that side for the hour.)
    if (player->GetMapId() != player->m_homebindMapId)
    {
        rt.lastCrossingAt = std::max<uint32>(getMSTime(), 1);
        rt.errandCrossing = 0;
    }
    player->TeleportTo(player->m_homebindMapId, player->m_homebindX, player->m_homebindY, player->m_homebindZ, player->GetOrientation());
    rt.why = "Back at its inn by hearthstone: " + reason;
    return true;
}

bool OrderService::LeaveBoxedPlace(Player* player, BotRuntime& rt)
{
    // (A level-34 mage south of the Venture Co. camp in Stranglethorn, level 36: every route began at the camp's node and
    // was refused, the ship too, and it ground beside the elites of Zul'Gurub for more than an hour: 23 ways refused in
    // two minutes. Its inn in Booty Bay has a flight master and a ship.)
    constexpr uint8 BoxedRefusals = 8;
    constexpr float BoxedYards = 300.0f;
    constexpr uint32 BoxedWindowMs = 15 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 BoxedNoteMs = 10 * MINUTE * IN_MILLISECONDS;
    constexpr float BoxedMinInnYards = 250.0f;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (rt.refusalsPending)
    {
        bool const fresh = !rt.boxedSince || getMSTimeDiff(rt.boxedSince, now) > BoxedWindowMs || rt.boxedMap != player->GetMapId() ||
            Dist2d(player, rt.boxedX, rt.boxedY) > BoxedYards;
        if (fresh)
        {
            rt.boxedSince = now;
            rt.boxedMap = player->GetMapId();
            rt.boxedX = player->GetPositionX();
            rt.boxedY = player->GetPositionY();
            rt.boxedRefusals = 0;
        }
        rt.boxedRefusals = uint8(std::min<uint32>(rt.boxedRefusals + rt.refusalsPending, 255));
        rt.refusalsPending = 0;
    }
    if (rt.boxedRefusals < BoxedRefusals)
        return false;
    // Not in the middle of something that takes it elsewhere anyway, and not where its inn is.
    if (rt.squad.following || rt.task.kind == TaskKind::Voyage || rt.task.kind == TaskKind::Escort || rt.task.kind == TaskKind::Event ||
        player->IsInCombat() || !player->IsAlive())
        return false;
    // (Not while it grinds where it stands: a bot that has fought in the last three minutes has work there. Saelanya, a
    // level-33 priest, ground the satyrs of Sargeron in Desolace at 30000 experience an hour for six minutes while its
    // ways out were refused, and took its hearthstone a minute after its last fight, round 263.)
    constexpr uint32 FoughtLatelyMs = 3 * MINUTE * IN_MILLISECONDS;
    if (rt.lastFightEndAt && getMSTimeDiff(rt.lastFightEndAt, now) < FoughtLatelyMs)
        return false;
    uint32 const minutes = std::max<uint32>(1, getMSTimeDiff(rt.boxedSince, now) / (MINUTE * IN_MILLISECONDS));
    std::string const boxedWhy = Acore::StringFormat("its ways from here were refused {} times in {} min", uint32(rt.boxedRefusals), minutes);
    // First back the way it came (a rogue beside a camp of 19 Mirefin in Dustwallow Marsh: eight ways refused in a
    // minute, each through the camp, and the hearthstone); the hearthstone when that does not get it out.
    if (StartTrailOut(player, rt, boxedWhy))
    {
        rt.boxedRefusals = 0;
        rt.boxedSince = 0;
        return true;
    }
    if (HearthHome(player, rt, boxedWhy, BoxedMinInnYards))
    {
        rt.boxedRefusals = 0;
        rt.boxedSince = 0;
        return true;
    }
    if (now >= rt.nextBoxedNote)
    {
        rt.nextBoxedNote = now + BoxedNoteMs;
        uint32 const left = player->GetSpellCooldownDelay(HearthstoneSpell) / IN_MILLISECONDS;
        LOG_INFO(ActivityLog, "activity: {} is boxed in at ({:.0f}, {:.0f}): its ways from here were refused {} times in {} min; {}",
            player->GetName(), rt.boxedX, rt.boxedY, uint32(rt.boxedRefusals), minutes,
            !player->HasItemCount(HearthstoneItem, 1) ? "it has no hearthstone" :
            player->HasSpellCooldown(HearthstoneSpell) ? Acore::StringFormat("its hearthstone is ready in {} min", (left + 59) / 60) :
            std::string("its inn is here"));
    }
    return false;
}

// Back on the ground: the nearest node of the road map (a spot with walking links); with none in reach, or right after
// such a rescue did not help, the hearthstone.
bool OrderService::Rescue(Player* player, BotRuntime& rt, char const* what, float minDistance)
{
    // Not out of a dungeon: the road map has no node there, and the hearthstone would end the group's run.
    if (player->GetMap()->IsDungeon())
        return false;

    uint32 const now = getMSTime();
    std::string state = DescribeStandstill(player);
    // Every rescue is a defect to look into (the owner): the line names the task and its target, so the rounds can count them.
    std::string const taskText = rt.task.targetName.empty() ? std::string(ToString(rt.task.kind)) :
        Acore::StringFormat("{} to {}", ToString(rt.task.kind), rt.task.targetName);
    // Out of the water: the water around is not walked through again for a while (a route does not lead through a
    // place that is set aside). A druid on its way through Loch Modan was rescued twice from one spot on the floor of
    // the Loch, (-5220, -3320): a link of the road map crosses the lake there, the path finder has no path for a
    // character in the water column, and the route planned after the rescue was the same route.
    if (IsSwimming(player) || IsDiving(player))
    {
        rt.AddAvoidArea(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), 255, WaterAvoidMs, false);
        state += Acore::StringFormat("; in the water: its routes keep away from here for {} min", WaterAvoidMs / (MINUTE * IN_MILLISECONDS));
    }
    if (std::string const passed = sGroundGuard.DescribeLastPassed(player->GetGUID()); !passed.empty())
        state += "; last walk the ground check let through " + passed;
    RoutePoint spot;
    std::string name;
    // A rescue near the last one within half an hour: the road node did not help, the hearthstone instead. (Not one
    // elsewhere: the guild leader was rescued in the Searing Gorge's Cauldron and, 16 minutes and 400 yd later, sent
    // home to Tanaris by hearthstone from below Thorium Point, the quest it was handing in 124 yd above it; 41 s after
    // that it was put at Theramore's graveyard on the walk to the ship.)
    constexpr float RescueNearYards = 150.0f;
    bool const lately = rt.lastRescueAt && getMSTimeDiff(rt.lastRescueAt, now) < 30 * MINUTE * IN_MILLISECONDS &&
        rt.lastRescueMap == player->GetMapId() && Dist2d(player, rt.lastRescueX, rt.lastRescueY) <= RescueNearYards;
    float const rescuedX = player->GetPositionX(), rescuedY = player->GetPositionY();
    uint32 const rescuedMap = player->GetMapId();
    // The nearest node that stands on the navmesh (not a lift's: a rescue there was followed by a second one).
    auto nodeOnGround = [&]()
    {
        float from = minDistance;
        for (int tries = 0; tries < 6; ++tries)
        {
            if (!sRoadMap.FindNearestNode(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), from, RescueRange, spot,
                name, player->GetTeamId(), player->GetLevel()))
                return false;
            if (NodeOnNavmesh(player, spot))
                return true;
            from = Dist2d(player, spot.x, spot.y) + 10.0f;
        }
        return false;
    };
    if (!lately && nodeOnGround())
    {
        LOG_INFO(ActivityLog, "activity: {} {} at ({:.0f}, {:.0f}, {:.0f}) map {} (task {}; {}): it is put back on the ground at {} "
            "({:.0f}, {:.0f}, {:.0f})", player->GetName(), what, player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ(), player->GetMapId(), taskText, state, name, spot.x, spot.y, spot.z);
        BotDriver::Rest(player);
        ClearTask(rt, 5 * IN_MILLISECONDS, "rescued");
        SnapToNavmesh(player, player->GetMapId(), spot.x, spot.y, spot.z);
        player->TeleportTo(player->GetMapId(), spot.x, spot.y, spot.z + 0.5f, player->GetOrientation());
        rt.why = Acore::StringFormat("Was stuck: put back on the ground at {}", name);
    }
    else if (!player->HasSpellCooldown(HearthstoneSpell))
    {
        LOG_INFO(ActivityLog, "activity: {} {} at ({:.0f}, {:.0f}, {:.0f}) map {} (task {}; {}): it uses its hearthstone",
            player->GetName(), what, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetMapId(),
            taskText, state);
        BotDriver::Rest(player);
        ClearTask(rt, 10 * IN_MILLISECONDS, "rescued: hearthstone");
        // The result of the hearthstone, with its cooldown; the ten seconds of casting are the pause above.
        player->AddSpellCooldown(HearthstoneSpell, HearthstoneItem, HearthstoneCooldownMs);
        player->TeleportTo(player->m_homebindMapId, player->m_homebindX, player->m_homebindY, player->m_homebindZ,
            player->GetOrientation());
        rt.why = "Was stuck: back at its inn by hearthstone";
    }
    else if (GraveyardStruct const* graveyard = sGraveyard->GetClosestGraveyard(player, player->GetTeamId()))
    {
        // No node in reach (or it did not help) and the hearthstone is on cooldown: the graveyard a dead character of
        // its side would wake up at. Always on the ground, always near.
        LOG_INFO(ActivityLog, "activity: {} {} at ({:.0f}, {:.0f}, {:.0f}) map {} (task {}; {}): {} and its hearthstone is on cooldown, "
            "it is put at the graveyard at ({:.0f}, {:.0f}, {:.0f})", player->GetName(), what, player->GetPositionX(),
            player->GetPositionY(), player->GetPositionZ(), player->GetMapId(), taskText, state,
            lately ? "a rescue near here did not help" : "no road map node in reach", graveyard->x, graveyard->y, graveyard->z);
        BotDriver::Rest(player);
        ClearTask(rt, 5 * IN_MILLISECONDS, "rescued: graveyard");
        player->TeleportTo(graveyard->Map, graveyard->x, graveyard->y, graveyard->z + 0.5f, player->GetOrientation());
        rt.why = "Was stuck: put at the nearest graveyard";
    }
    else
    {
        LOG_INFO(ActivityLog, "activity: {} {} at ({:.0f}, {:.0f}, {:.0f}) map {} (task {}; {}); no node of the road map in reach, "
            "its hearthstone is on cooldown and there is no graveyard", player->GetName(), what, player->GetPositionX(),
            player->GetPositionY(), player->GetPositionZ(), player->GetMapId(), taskText, state);
        return false;
    }
    rt.lastRescueAt = now;
    rt.lastRescueMap = rescuedMap;
    rt.lastRescueX = rescuedX;
    rt.lastRescueY = rescuedY;
    rt.hasLastPosition = false; // no jump for the planner
    rt.stuckSince = 0;
    rt.strandSince = 0;
    rt.underGroundSince = 0;
    rt.groundStrikes = 0;
    sGroundGuard.Forget(player->GetGUID());
    return true;
}

namespace
{
    // The profession a bot's profession mode works on alone (the owner, 2026-11-14: [quote left out]): of the professions of its plan it has learned and not taken
    // to their targets, the lowest, while it is more than `behind` points below the highest of them. 0: none.
    uint32 LaggingProfession(GuildBot const& bot, Player* player, uint32 behind, uint32* ahead = nullptr)
    {
        uint32 low = 0, high = 0, lowValue = 0, highValue = 0;
        for (uint32 skill : Professions::PlannedSkills(bot))
        {
            if (!Professions::HasWork(bot, player, skill))
                continue;
            uint32 const value = player->GetPureSkillValue(skill);
            if (!low || value < lowValue)
            {
                low = skill;
                lowValue = value;
            }
            if (!high || value > highValue)
            {
                high = skill;
                highValue = value;
            }
        }
        if (ahead)
            *ahead = high;
        return low && behind && highValue > lowValue + behind ? low : 0;
    }

    bool IsGatheringSkill(uint32 skill)
    {
        return skill == SKILL_MINING || skill == SKILL_HERBALISM || skill == SKILL_SKINNING || skill == SKILL_FISHING;
    }

    std::string SkillName(uint32 skill)
    {
        SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill);
        return line ? line->name[0] : "a profession";
    }
}

void OrderService::RunProfessions(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    rt.targetZone = 0; // the walks of this mode end at places, not in a zone
    if (GatherDetour(bot, player, rt))
        return;

    // A walk of this mode: to a place where a node spawns, to a hunting ground, to an anvil, a forge or a fire.
    if (rt.task.kind == TaskKind::Travel && rt.task.gatherRound)
    {
        SpawnPoint const spot = rt.task.spot;
        float const distance = Dist2d(player, spot);
        if (rt.task.focus)
        {
            if (Professions::FocusNear(player, rt.task.focus))
            {
                ClearTask(rt, 0, "at the place to craft");
                rt.nextCraftCheck = 0;
                return;
            }
        }
        else if (rt.task.skinHunt || rt.task.huntItem)
        {
            if (distance <= HuntGroundNear)
            {
                ClearTask(rt, 0, "near the hunting ground");
                rt.nextHuntLook = 0;
                rt.nextCraftCheck = 0;
                return;
            }
        }
        else if (rt.task.fishTrip)
        {
            // (A school of fish lies out in the water: in sight of it the shore is looked for.)
            if (distance <= (rt.task.fishNear ? 150.0f : GatherPlaceReached))
            {
                ClearTask(rt, 0, "at the water");
                rt.nextShoreLook = 0;
                return;
            }
        }
        else if (distance <= GatherPlaceReached)
        {
            // The node (if it is there) is the detour's: for a few seconds. (A node the detour does not take, for its own
            // rules, held the bot there: two miners stood five minutes beside a vein in Duskwood and Stranglethorn,
            // "rpg idle", until the rescue put them on a road.)
            constexpr uint32 NodeWaitMs = 15 * IN_MILLISECONDS;
            if (!rt.task.nearSince)
                rt.task.nearSince = now;
            if (getMSTimeDiff(rt.task.nearSince, now) < NodeWaitMs &&
                Professions::FindNode(player, GetSettings().professionsGatherDetour > 0.0f ? GetSettings().professionsGatherDetour : 30.0f))
                return; // the detour takes it on its next look
            rt.Avoid(spot, GatherPlaceGapMs);
            rt.gatherRefusals = 0;
            ClearTask(rt, 0, "place visited");
            return;
        }
        // Far away: the travel step, which books flights (no target zone: it ends where this mode says, above).
        // (A fishing trip at any distance: near as the crow flies can be a journey by road, and it is one walk, not a
        // round. From Menethil the water of Ironforge is 1021 yd away and 8815 yd on foot.)
        if (distance > GatherRoundDistance || rt.task.fishTrip)
        {
            // (A way that is under way for a while has passed its gate.)
            bool const fresh = getMSTimeDiff(rt.task.startedAt, now) < GatherRefusalMs;
            if (!StepTask(guild, bot, player, rt) && rt.task.kind == TaskKind::None)
            {
                rt.AddAvoidArea(spot.mapId, spot.x, spot.y, 255, AvoidSpawnMs, false);
                if (fresh)
                    ++rt.gatherRefusals;
            }
            return;
        }
        if (UpdateStuck(player, rt, spot.x, spot.y))
        {
            rt.Avoid(spot, AvoidSpawnMs);
            rt.AddAvoidArea(spot.mapId, spot.x, spot.y, 255, AvoidSpawnMs, false);
            NoteNpcUnreached(guild, bot.name, rt.task);
            ClearTask(rt, 5 * IN_MILLISECONDS, "place not reached");
            return;
        }
        WalkTo(player, rt, spot.mapId, spot.x, spot.y, spot.z);
        // A way the walk gate refuses: the next place of the round lies behind the same creatures more often than not
        // (a druid's catch-up planned 61 hunting grounds in Darkshore from Astranaar, each refused for the same camp
        // on the road; a miner 76 veins). Three in a row pause the gathering.
        if (LeaveDangerousWalk(bot, player, rt, GetSettings().walksMaxLevelsAbove, true))
            ++rt.gatherRefusals;
        return;
    }
    if (rt.task.kind != TaskKind::None && StepTask(guild, bot, player, rt))
        return;
    // (The task ended and left a journey in its place, StartJourney: that is the task now.)
    if (rt.task.kind == TaskKind::Travel && rt.task.journey)
        return;
    if (now < rt.waitUntil)
        return;
    if (PlanUpkeep(bot, player, rt))
        return;

    // A profession far behind the others is worked on alone until it is within Professions.FocusBehind of the highest
    // (LaggingProfession; not on a catch-up of the autopilot). A crafting profession by crafting, with the materials it
    // makes itself first; a gathering profession by gathering (a miner smelts while that gives skill). When it can do
    // nothing for that profession as things are, its other professions have their turns for a while.
    constexpr uint32 FocusPauseMs = 2 * MINUTE * IN_MILLISECONDS;
    uint32 ahead = 0;
    uint32 const behind = rt.catchUp.skill ? 0 : uint32(sBotSettings.Get(player, Setting::ProfessionsFocusBehind));
    uint32 const lagging = behind ? LaggingProfession(bot, player, behind, &ahead) : 0;
    if (lagging != rt.professionFocus)
    {
        if (lagging)
            LOG_INFO(ActivityLog, "activity: {} works on its {} ({}) alone in the profession mode: it is more than {} points behind its {} ({})",
                bot.name, SkillName(lagging), player->GetPureSkillValue(lagging), behind, SkillName(ahead), player->GetPureSkillValue(ahead));
        else
            LOG_INFO(ActivityLog, "activity: {} has caught up on its {} ({}): its professions take turns again", bot.name,
                SkillName(rt.professionFocus), player->GetPureSkillValue(rt.professionFocus));
        rt.professionFocus = lagging;
        rt.professionFocusPausedUntil = 0;
    }
    uint32 focus = now < rt.professionFocusPausedUntil ? 0 : lagging;
    auto pauseFocus = [&](char const* why)
    {
        LOG_INFO(ActivityLog, "activity: {} can do nothing for its {} ({}) now ({}): its other professions for {} min", bot.name,
            SkillName(focus), player->GetPureSkillValue(focus), why, FocusPauseMs / (MINUTE * IN_MILLISECONDS));
        rt.professionFocusPausedUntil = now + FocusPauseMs;
        focus = 0;
    };
    // (The next look right after a batch: with 30 s the bot had set off for a vein between a smelt and the piece the
    // bars were for, and came back to smelt again five minutes later.)
    constexpr uint32 CraftAgainMs = 3 * IN_MILLISECONDS;

    // Crafting first: what gives skill, from the bags and the guild storage. The planner leaves a crafting bot alone.
    // What the best recipe lacks and the bot can get by itself, it gets: a material from a vendor, the walk to an
    // anvil, a forge, a fire.
    if (now >= rt.nextCraftCheck)
    {
        rt.nextCraftCheck = now + ProfessionModeCraftGapMs;
        // Its bags fill with what it makes and gathers: into the guild storage with it (auto loot gathers nothing
        // into nearly full bags, and the recipes take their materials from the storage).
        if (player->GetFreeInventorySpace() < ProfessionModeFreeSlots)
            StoreProfessionGoods(bot, player);
        std::string text;
        // (A catch-up of a gathering skill on the autopilot, CatchUpGathering, only gathers.) And nothing is crafted
        // or enchanted with something hostile around (IsQuietToCraft): the bot gathers on, and with nothing else to
        // do it walks to a quiet place first (PlanQuietPlace, below).
        uint32 hostile = 0;
        bool const crafts = rt.catchUp.skill == 0 && IsQuietToCraft(player, &hostile);
        rt.unquietHostiles = rt.catchUp.skill == 0 ? hostile : 0;
        if (rt.unquietHostiles && now >= rt.nextUnquietNote)
        {
            rt.nextUnquietNote = now + UnquietNoteMs;
            LOG_INFO(ActivityLog, "activity: {} does not craft at ({:.0f}, {:.0f}): {} hostile creatures within {:.0f} yd", bot.name,
                player->GetPositionX(), player->GetPositionY(), hostile, CraftQuietYards);
        }
        // What the guild ordered comes before what only gives skill.
        if (crafts && sCraftOrders.Work(guild, player, text))
        {
            BotDriver::Rest(player);
            rt.why = "Crafting for the guild's order: " + text;
            rt.nextCraftCheck = now + CraftAgainMs;
            return;
        }
        // A piece of gear for the guild, once in five minutes, before what only gives skill (as at a hub).
        if (crafts && CraftGearForGuild(guild, bot, player, rt, true))
        {
            rt.professionIdleSince = 0;
            rt.nextCraftCheck = now + CraftAgainMs;
            return;
        }
        // As at a hub (RunAdventure): the professions take turns, each with its own look (WorkOnProfessions); a
        // profession far behind alone.
        ProfessionWork work = ProfessionWork::None;
        if (crafts && focus)
        {
            work = WorkOnProfessions(guild, bot, player, rt, ProfessionModeBatch, true, focus);
            if (work == ProfessionWork::None && !IsGatheringSkill(focus))
                pauseFocus("nothing of it can be made, bought or fetched");
        }
        if (crafts && !focus && work == ProfessionWork::None)
            work = WorkOnProfessions(guild, bot, player, rt, ProfessionModeBatch, true);
        if (work != ProfessionWork::None)
        {
            rt.professionIdleSince = 0;
            if (work == ProfessionWork::Crafting)
                rt.nextCraftCheck = now + CraftAgainMs;
            return;
        }
    }
    // A crafting profession far behind: nothing is gathered for the others meanwhile (the next look comes; with
    // something hostile around, first to a quiet place).
    if (focus && !IsGatheringSkill(focus))
    {
        if (rt.unquietHostiles && PlanQuietPlace(bot, player, rt))
            return;
        // No quiet place in reach either: its other professions have their turns (it stood 45 minutes on the Westfall
        // coast among murlocs, "does not craft" every three minutes, its squad member beside it).
        if (rt.unquietHostiles)
            pauseFocus("no place without hostile creatures to craft at in reach");
        else
        {
            rt.why = Acore::StringFormat("Profession mode: working on its {} ({}), far behind its other professions", SkillName(focus),
                player->GetPureSkillValue(focus));
            return;
        }
    }
    // Then gathering: a round over the nodes its skills open, a hunt for skins, a fishing trip; the lowest skill first.
    constexpr uint32 NoWork = std::numeric_limits<uint32>::max();
    // (A catch-up on the autopilot is about one skill; so is a gathering profession far behind the others.)
    uint32 const only = rt.catchUp.skill;
    uint32 const gatherOnly = only ? only : focus;
    uint32 nodeSkill = NoWork;
    for (uint32 skill : { uint32(SKILL_MINING), uint32(SKILL_HERBALISM) })
        if ((!gatherOnly || gatherOnly == skill) && Professions::HasWork(bot, player, skill))
            nodeSkill = std::min<uint32>(nodeSkill, player->GetPureSkillValue(skill));
    uint32 const skinSkill = (!gatherOnly || gatherOnly == SKILL_SKINNING) && Professions::HasWork(bot, player, SKILL_SKINNING) ?
        player->GetPureSkillValue(SKILL_SKINNING) : NoWork;
    uint32 const fishSkill = (!gatherOnly || gatherOnly == SKILL_FISHING) && Professions::HasWork(bot, player, SKILL_FISHING) ?
        player->GetPureSkillValue(SKILL_FISHING) : NoWork;
    std::array<std::pair<uint32, int>, 3> ways{ { { nodeSkill, 0 }, { skinSkill, 1 }, { fishSkill, 2 } } };
    std::sort(ways.begin(), ways.end());
    // A miner catching up smelts the guild's ore while that is orange or yellow for it (the owner, 2026-11-14: [quote left out]): at a forge where it stands, else a forge within
    // SmeltForgeYards when the ore is there (Professions::CraftForSkill passes green smelts over; the guild storage's
    // reserves hold). The bars go into the guild storage, where the smiths and engineers take them.
    constexpr uint32 SmeltLookMs = 20 * IN_MILLISECONDS;
    constexpr float SmeltForgeYards = 1500.0f;
    if (only == SKILL_MINING && now >= rt.nextSmeltLook && now >= rt.gatherPausedUntil)
    {
        rt.nextSmeltLook = now + SmeltLookMs;
        std::string text;
        CraftNeed need;
        // (Away from a forge nothing can be smelted, and the look says where to go: also where something hostile is
        // near. At a forge only when it is quiet, IsQuietToCraft.)
        constexpr uint32 ForgeFocus = 3;
        bool const looks = IsQuietToCraft(player) || !Professions::FocusNear(player, ForgeFocus);
        if (looks && sProfessions.CraftForSkill(bot, player, ProfessionModeBatch, text, &need, 0, nullptr, SKILL_MINING))
        {
            LOG_INFO(ActivityLog, "activity: {} smelts for its Mining (catching up): {}", bot.name, text);
            // (The next look as soon as the casts are done, while it stands at the forge: the other ore, the next batch.)
            rt.nextSmeltLook = 0;
            BotDriver::Rest(player);
            rt.why = "Profession mode: smelting ore of the guild storage: " + text;
            rt.professionIdleSince = 0;
            return;
        }
        if (need.spell && need.focus && !need.buyItem && PlanFocusTrip(bot, player, rt, need.focus, SmeltForgeYards))
        {
            SpellInfo const* smelt = sSpellMgr->GetSpellInfo(need.spell);
            LOG_INFO(ActivityLog, "activity: {} walks to {} to smelt for its Mining (catching up): {}", bot.name, rt.task.targetName,
                smelt ? smelt->SpellName[0] : "ore");
            rt.why = Acore::StringFormat("Profession mode: on the way to {} to smelt", rt.task.targetName);
            rt.professionIdleSince = 0;
            return;
        }
    }
    if (rt.gatherRefusals >= GatherRefusalsMax)
    {
        rt.gatherRefusals = 0;
        rt.gatherPausedUntil = now + GatherPauseMs;
        LOG_INFO(ActivityLog, "activity: {} was refused {} ways to places to gather at, one after the other: no gathering for {} min",
            bot.name, uint32(GatherRefusalsMax), GatherPauseMs / (MINUTE * IN_MILLISECONDS));
    }
    for (auto const& [skill, way] : ways)
    {
        if (skill == NoWork || now < rt.gatherPausedUntil)
            break;
        if ((way == 0 && PlanGatherRound(bot, player, rt)) || (way == 1 && PlanSkinHunt(bot, player, rt)) ||
            (way == 2 && PlanFishing(bot, player, rt)))
        {
            rt.professionIdleSince = 0;
            return;
        }
    }
    // A gathering profession far behind with nothing to gather for it in reach: the others for a while (next tick).
    if (focus && !only)
    {
        pauseFocus("nothing in reach it can gather for it");
        return;
    }

    // (A catch-up on the autopilot: CatchUpGathering decides what follows.)
    if (only)
    {
        rt.catchUp.idle = true;
        rt.why = "Profession mode: looking for a place its skill can gather at";
        rt.waitUntil = now + HoldReplanMs;
        return;
    }
    // Nothing to gather, and the last look did not craft for what stands around: to a quiet place, and craft there.
    if (rt.unquietHostiles && PlanQuietPlace(bot, player, rt))
    {
        rt.professionIdleSince = 0;
        return;
    }

    // Nothing it can do for its professions right now. After two minutes of that (the looks of the single ways come
    // round in that time) the order is the autopilot again (the owner, 2026-11-13: [quote left out]). It does not go back to its professions by itself.
    constexpr uint32 ProfessionModeDoneMs = 2 * MINUTE * IN_MILLISECONDS;
    // (Not crafting for what stands around is not "nothing to craft": with no quiet node near, the walk to its safe
    // spot below, RunIdle, takes the bot to an inn, and the look there decides. Ten minutes for that.)
    // (A bot at the level cap was sent into the profession mode for one job of the guild's, which its cap looks found:
    // done with it, it grinds again after 30 s. It walked to an inn and stood there two minutes after every batch of
    // Thorium Bars, 2026-11-27.)
    constexpr uint32 CapModeDoneMs = 30 * IN_MILLISECONDS;
    // (Also when it did not craft for what stood around: the druid of 03:24 had walked to a quiet place and found
    // nothing to make there, then walked 236 s to an inn in the Hinterlands for the ten minutes.)
    bool const atCap = IsAtLevelCap(player);
    uint32 const doneMs = atCap ? CapModeDoneMs : rt.unquietHostiles ? 5 * ProfessionModeDoneMs : ProfessionModeDoneMs;
    if (!rt.professionIdleSince)
        rt.professionIdleSince = now;
    else if (now - rt.professionIdleSince >= doneMs)
    {
        rt.professionIdleSince = 0;
        std::string skills;
        for (uint32 skill : Professions::PlannedSkills(bot))
            if (player->HasSkill(skill))
                if (SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill))
                    skills += Acore::StringFormat("{}{} {}", skills.empty() ? "" : ", ", line->name[0], player->GetPureSkillValue(skill));
        // (At the level cap the autopilot grinds and gives the professions another look after CapCraftRetryMs.)
        bool const capped = IsAtLevelCap(player);
        if (capped)
        {
            capCraftRetry[bot.guid] = now + CapCraftRetryMs;
            // The cap's looks for the guild's jobs wait their time too: they set their clocks only when they found
            // nothing, and a warrior was sent back into the profession mode for the same Dense Weightstone 2 s after it
            // had handed back (2026-11-27).
            rt.nextAmmoLook = now + AmmoLookMs;
            rt.nextConsumableLook = now + ConsumableLookMs;
            rt.nextProcessLook = now + ProcessLookMs;
            rt.nextScrollLook = now + ScrollLookMs;
            // The consumable it was sent in for and did not make (its look saw a way, the craft did not start: a mage
            // was sent in for Wizard Oil every quarter of an hour and made none): left out for an hour.
            if (rt.capConsumable)
            {
                if (uint32 const owner = sStorageService.GetOwnerOf(player);
                    owner && sStorageService.GetAmount(owner, rt.capConsumable, 0) <= rt.capConsumableHad)
                {
                    rt.consumableSkip[rt.capConsumable] = now + ConsumableSkipMs;
                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(rt.capConsumable);
                    LOG_INFO(ActivityLog, "activity: {} made no {} in the profession mode it was sent into for it: left out for {} min",
                        bot.name, proto ? proto->Name1 : "consumable", ConsumableSkipMs / (MINUTE * IN_MILLISECONDS));
                }
                rt.capConsumable = 0;
            }
            // (Its claims on the guild's jobs go with it, or the next crafter waits out the half hour: a mage waited for
            // a warlock that had handed back 25 minutes before, 2026-11-27.)
            ReleaseConsumableClaim(guild, bot.guid);
            ReleaseProcessClaim(guild, bot.guid);
        }
        LOG_INFO(ActivityLog, "activity: {} has nothing left to craft or gather for its professions ({}): its order is the autopilot "
            "again{}", bot.name, skills.empty() ? "none learned" : skills,
            capped ? Acore::StringFormat("; at the level cap it grinds and looks at its professions again in {} min",
                CapCraftRetryMs / (MINUTE * IN_MILLISECONDS)) : std::string());
        RecordEvent(guild, "ZONE_DONE", bot.guid, Acore::StringFormat("{} has nothing left to craft or gather for its professions and "
            "is back on the autopilot ({}).", bot.name, skills.empty() ? "no profession learned" : skills), player->GetLevel(),
            player->GetZoneId());
        bot.order.type = OrderType::Auto;
        bot.order.zoneId = 0;
        sGuildAdventureMgr.SaveOrder(bot);
        OnOrderChanged(guild, bot);
        return;
    }
    // (At the level cap no walk to an inn for the 30 s: it grinds again where it stands.)
    if (atCap)
    {
        BotDriver::Rest(player);
        rt.why = "Profession mode: nothing to craft or gather right now";
    }
    else
    {
        RunIdle(bot, player, rt);
        rt.why = "Profession mode: nothing to craft or gather right now (" + rt.why + ")";
    }
    rt.waitUntil = now + HoldReplanMs;
}

bool OrderService::CatchUpGathering(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    constexpr uint32 CatchUpLookMs = MINUTE * IN_MILLISECONDS;          // how often the zone is held against the skills
    constexpr uint32 CatchUpIdleMs = 2 * MINUTE * IN_MILLISECONDS;      // nothing to gather for this long: given up ...
    constexpr uint32 CatchUpNoGainMs = 20 * MINUTE * IN_MILLISECONDS;   // ... or no skill point for this long ...
    constexpr uint32 CatchUpRetryMs = 30 * MINUTE * IN_MILLISECONDS;    // ... and looked at again this much later
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    BotRuntime::CatchUp& state = rt.catchUp;
    auto nameOf = [](uint32 skill)
    {
        SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill);
        return std::string(line ? line->name[0] : "its gathering skill");
    };
    auto stop = [&](uint32 retryMs)
    {
        if (rt.task.kind != TaskKind::None)
            ClearTask(rt, 0, "catch-up over");
        state = {};
        rt.nextCatchUpLook = now + retryMs;
        rt.waitUntil = 0;
        BotDriver::Rest(player);
    };
    // A catch-up that was given up is looked at again later each time (half an hour, then one, two, four hours): every
    // try is a journey there and back (the owner, 2026-11-13: no travelling far for a couple of points and straight back).
    uint32 const retry = CatchUpRetryMs << std::min<uint8>(rt.catchUpFails, 3);
    auto giveUp = [&]()
    {
        rt.catchUpFails = std::min<uint8>(rt.catchUpFails + 1, 3);
        stop(retry);
    };
    if (bot.order.type != OrderType::Auto || !sBotSettings.IsOn(player, Setting::ProfessionsCatchUpGathering))
    {
        if (state.skill)
            stop(0);
        return false;
    }

    if (!state.skill)
    {
        // Looked at between two tasks, for the zone it quests in or is on its way to, wherever it stands: what a
        // catch-up has reached does not outlive a restart of the server, and a bot that had travelled for one would
        // go back to its zone first only to set out again from there.
        ZoneTemplate const* target = rt.targetZone ? sGuildAdventureMgr.GetZone(rt.targetZone) : nullptr;
        if (rt.task.kind != TaskKind::None || now < rt.nextCatchUpLook || !target || !player->IsAlive() || player->IsInCombat() ||
            rt.capIdleUntil)
            return false;
        uint32 const zoneId = target->zoneId;
        rt.nextCatchUpLook = now + CatchUpLookMs;
        // A catch-up is a journey: not for a gap of a few points (the owner, 2026-11-13: [quote left out]).
        // (A bot that has nothing to do has the time for any gap.)
        uint32 const minGap = rt.idleHolds ? 1 :
            std::max<uint32>(uint32(sBotSettings.Get(player, Setting::ProfessionsCatchUpMinGap)), 1);
        uint32 behind = 0, floor = 0, gap = 0;
        for (uint32 skill : { uint32(SKILL_MINING), uint32(SKILL_HERBALISM), uint32(SKILL_SKINNING) })
        {
            if (!Professions::HasWork(bot, player, skill))
                continue;
            // Against what most of the zone's nodes ask for, not the lowest of them: a Silver Vein (75) stands somewhere
            // in almost every zone, so no miner was ever "behind" (the owner, 2026-11-14, on a level-45 warrior with Mining
            // 110 in the Searing Gorge: [quote left out]; twelve of his
            // fourteen miners of level 28 to 45 stood between 44 and 110).
            uint32 const need = sProfessions.ZoneGatherUsual(target->continent, zoneId, skill);
            uint32 const value = player->GetSkillValue(skill);
            if (GetSettings().debug)
                LOG_INFO("module", "mod-guild-adventure: {} for {}: {} {}, most of the zone asks for {}{}", bot.name, ZoneName(zoneId),
                    nameOf(skill), value, need, need <= value ? "" : (need - value >= minGap ? " (behind)" :
                        " (behind by less than a journey is worth)"));
            if (need > value && need - value >= minGap && need - value > gap)
            {
                behind = skill;
                floor = need;
                gap = need - value;
            }
        }
        if (!behind)
            return false;
        state.skill = behind;
        state.target = floor;
        state.zone = zoneId;
        state.since = now;
        state.startValue = state.lastValue = player->GetSkillValue(behind);
        state.lastGainAt = now;
        LOG_INFO(ActivityLog, "activity: {} cannot gather most of what {} has with {} {} (most of it asks for {}): it catches the "
            "skill up before it quests on{}", bot.name, ZoneName(zoneId), nameOf(behind), state.startValue, floor,
            rt.idleHolds ? " (it has nothing else to do)" : "");
    }

    uint32 const value = player->GetSkillValue(state.skill);
    std::string const name = nameOf(state.skill);
    if (value >= state.target || !Professions::HasWork(bot, player, state.skill))
    {
        LOG_INFO(ActivityLog, "activity: {} has caught up: {} {} (was {}, {} min): back to its quests in {}", bot.name, name, value,
            state.startValue, getMSTimeDiff(state.since, now) / (MINUTE * IN_MILLISECONDS), ZoneName(state.zone));
        rt.catchUpFails = 0;
        stop(0);
        return false;
    }
    if (value > state.lastValue)
    {
        state.lastValue = value;
        state.lastGainAt = now;
    }
    else if (getMSTimeDiff(state.lastGainAt, now) >= CatchUpNoGainMs)
    {
        LOG_INFO(ActivityLog, "activity: {} gained no point of {} in {} min ({} of the {} that {} asks for): it quests on and "
            "looks again in {} min", bot.name, name, CatchUpNoGainMs / (MINUTE * IN_MILLISECONDS), value, state.target,
            ZoneName(state.zone), retry / (MINUTE * IN_MILLISECONDS));
        giveUp();
        return false;
    }

    // The profession mode's gathering, for this skill alone (RunProfessions reads rt.catchUp).
    RunProfessions(guild, bot, player, rt);
    if (now < rt.gatherPausedUntil && rt.task.kind == TaskKind::None)
    {
        LOG_INFO(ActivityLog, "activity: {} cannot get to a place its {} {} can gather at (the ways there were refused): it quests on "
            "and looks again in {} min", bot.name, name, value, retry / (MINUTE * IN_MILLISECONDS));
        rt.gatherPausedUntil = 0;
        giveUp();
        return false;
    }
    if (rt.task.kind != TaskKind::None || rt.gatherNode)
    {
        state.idle = false;
        state.idleSince = 0;
    }
    else if (state.idle)
    {
        if (!state.idleSince)
            state.idleSince = now;
        else if (getMSTimeDiff(state.idleSince, now) >= CatchUpIdleMs)
        {
            LOG_INFO(ActivityLog, "activity: {} finds nothing its {} {} can gather in reach ({} asks for {}): it quests on and "
                "looks again in {} min", bot.name, name, value, ZoneName(state.zone), state.target,
                retry / (MINUTE * IN_MILLISECONDS));
            giveUp();
            return false;
        }
    }
    static std::string const prefix = "Profession mode: ";
    if (rt.why.compare(0, prefix.size(), prefix) == 0)
        rt.why = Acore::StringFormat("Catching up on {} ({} of {} for {}): {}", name, value, state.target, ZoneName(state.zone),
            rt.why.substr(prefix.size()));
    return true;
}

bool OrderService::PlanGatherRound(GuildBot const& bot, Player* player, BotRuntime& rt)
{
    // (A catch-up on the autopilot is about one skill.)
    uint32 const only = rt.catchUp.skill;
    bool const mining = (!only || only == SKILL_MINING) && Professions::HasWork(bot, player, SKILL_MINING);
    bool const herbs = (!only || only == SKILL_HERBALISM) && Professions::HasWork(bot, player, SKILL_HERBALISM);
    if (!mining && !herbs)
        return false;
    uint8 const level = player->GetLevel();
    // The places of nodes its skill opens and that are orange or yellow for it (the owner, 2026-11-14: [quote left out]): a node gives a
    // point every time up to 25 above what it asks, three times in four up to 50, once in four up to 100 (green), then
    // nothing (Player::UpdateGatherSkill). Green ones only where nothing orange or yellow is in reach at all (Mining 51
    // to 64: copper is green, tin opens at 65). Nearest first; the nearest few are looked at closely.
    constexpr uint32 YellowSpan = 50;
    std::vector<std::pair<float, NodeSpawn const*>> candidates, greens;
    for (NodeSpawn const& node : sProfessions.GetNodeSpawns(player->GetMapId()))
    {
        if (!(node.skill == SKILL_MINING ? mining : herbs))
            continue;
        uint32 const value = player->GetSkillValue(node.skill);
        if (node.required > value || value >= uint32(node.required) + 100)
            continue;
        bool const green = value >= uint32(node.required) + YellowSpan;
        if (rt.IsAvoided(node.spawn) || rt.IsInAvoidedArea(node.spawn.mapId, node.spawn.x, node.spawn.y, level))
            continue;
        // A place whose node is not there (a place of a pool that is not in use, a vein that was mined) is not walked
        // to where the map can tell, which is around the bot. (On the live server two miners walked to 106 places in
        // twenty minutes and found a node at twenty of them.)
        if (IsSpawnDown(player, node.spawn))
            continue;
        // (A node far above or below is in a cave or on a ridge: three times its height counts as way.)
        (green ? greens : candidates).emplace_back(Dist2d(player, node.spawn) + 3.0f * std::fabs(node.spawn.z - player->GetPositionZ()), &node);
    }
    Region const here = GetRegion(player);
    // Whether an orange or yellow node lies within its reach at all: on this land mass, in a zone not above it.
    bool yellowInReach = false;
    for (auto const& [distance, node] : candidates)
    {
        ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(sWorldIndex.GetSpawnZone(node->spawn));
        if ((distance <= GatherRoundDistance || sWorldIndex.GetSpawnRegion(node->spawn) == here) && !(zone && zone->minLevel > level))
        {
            yellowInReach = true;
            break;
        }
    }
    bool const onGreen = !yellowInReach;
    if (onGreen)
        candidates.swap(greens);
    std::size_t const looked = std::min(candidates.size(), ProfessionPlaceChecks);
    std::partial_sort(candidates.begin(), candidates.begin() + looked, candidates.end(),
        [](auto const& a, auto const& b) { return a.first < b.first; });
    NodeSpawn const* best = nullptr;
    for (std::size_t i = 0; i < looked && !best; ++i)
    {
        NodeSpawn const& node = *candidates[i].second;
        // On this land mass, not in a zone above the bot, nor at a place the risk rules keep it from.
        if (candidates[i].first > GatherRoundDistance && sWorldIndex.GetSpawnRegion(node.spawn) != here)
            continue;
        ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(sWorldIndex.GetSpawnZone(node.spawn));
        if ((zone && zone->minLevel > level) || !Risk::JudgeSpot(player, rt, node.spawn).usable)
            continue;
        // Nor one whose straight way leads past creatures the walk gate would refuse (a level-31 miner in the
        // Hillsbrad Foothills planned a vein beside Alterac's ogres 32 times in twenty minutes, each time to be told
        // "the way passes level-47 creatures").
        if (int32(sRoadMap.DangerAlong(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), node.spawn.x, node.spawn.y,
            player->GetTeamId())) - int32(level) > int32(sBotSettings.Get(player, Setting::WalksMaxLevelsAbove)))
            continue;
        // Nor one whose straight way leads into a place it has just kept away from (a pack, a camp): a shaman in the
        // profession mode set out for three Stranglekelp beside the Bloodsail camp of the Southern Savage Coast in 45
        // seconds, kept away from the camp twice and was caught on the third way (Shammpower, round 108).
        if (rt.CrossesAvoidedArea(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), node.spawn.x, node.spawn.y, level))
            continue;
        // Nor one among creatures four or more levels above the bot (a priest of 38 catching up on its Herbalism went
        // for the Wild Steelbloom beside the Mosh'Ogg ogre mound of Stranglethorn, level 42 to 44 on their rounds, kept
        // away from them for twenty minutes and died there).
        if (StrongNearNode(player, node.spawn.mapId, node.spawn.x, node.spawn.y, node.spawn.z))
            continue;
        // Nor one among more creatures than it takes at once, or in the middle of a camp it cannot take (the Sungrass
        // in the Twilight Base Camp of Silithus, round 223), nor one whose straight way leads into such a camp.
        if (NodeAmongMany(player, rt, node.spawn.mapId, node.spawn.x, node.spawn.y, node.spawn.z) ||
            Risk::InCampItCannotTake(player, rt, node.spawn.mapId, node.spawn.x, node.spawn.y, node.spawn.z) ||
            Risk::EntersCamp(player, rt, player->GetMapId(), player->GetPositionX(), player->GetPositionY(), node.spawn.x, node.spawn.y))
            continue;
        // Nor one the path finder does not lead to (near ones; far ones are walked along routes): a mage inside
        // Hive'Ashi in Silithus set out three times for an ooze-covered vein whose path ended in a pocket 25 yd short
        // of it, from which nothing led on or back, round 146. Such a node is set aside for half an hour.
        // Nor one the guild's characters could not get to three times within a day (NoteNpcUnreached).
        if (IsNpcBlocked(player, node.spawn))
            continue;
        constexpr float NodeWalkCheckYards = 300.0f;
        if (Dist2d(player, node.spawn) <= NodeWalkCheckYards &&
            !WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), node.spawn.x, node.spawn.y,
                node.spawn.z))
        {
            rt.Avoid(node.spawn, 30 * MINUTE * IN_MILLISECONDS);
            continue;
        }
        best = &node;
    }
    if (!best)
        return false;

    BotTask task;
    task.kind = TaskKind::Travel;
    task.gatherRound = true;
    task.hasSpot = true;
    task.spot = best->spawn;
    task.targetName = SpawnName(best->spawn);
    StartTask(player, rt, task);
    rt.why = Acore::StringFormat("Profession mode: on the way to a place of {} ({} {} of {}{})", rt.task.targetName,
        best->skill == SKILL_MINING ? "Mining" : "Herbalism", player->GetPureSkillValue(best->skill),
        player->GetPureMaxSkillValue(best->skill), onGreen ? "; nothing orange or yellow in reach, a green one" : "");
    return true;
}

bool OrderService::IsPlaceUsable(Player const* player, BotRuntime const& rt, uint32 mapId, float x, float y, float z) const
{
    SpawnPoint place;
    place.mapId = uint16(mapId);
    place.x = x;
    place.y = y;
    place.z = z;
    place.gameObject = true;
    return Risk::JudgeSpot(player, rt, place).usable;
}

bool OrderService::CraftGearForGuild(AdventureGuild& guild, GuildBot const& bot, Player* player, BotRuntime& rt, bool professionMode)
{
    // One piece per look, and a look every few minutes: the materials are the guild's. What a piece lacks is seen to
    // in steps a few seconds apart (a material made, a purchase), a handful of them per hand-in at a hub.
    constexpr uint32 GearCraftGapMs = 5 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 GearStepGapMs = 3 * IN_MILLISECONDS;
    constexpr uint8 MaxHubGearSteps = 6;
    constexpr uint8 MaxHubPurchases = 3;
    constexpr float HubShopYards = 300.0f;
    constexpr float HubFocusYards = 200.0f;
    constexpr uint8 MaxHubFocusTrips = 2;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (now < rt.nextGearCraft || rt.gearCraftItem || !sBotSettings.IsOn(player, Setting::ProfessionsCraftGear))
        return false;
    if (!professionMode && rt.hubGearSteps >= MaxHubGearSteps)
        return false;
    rt.nextGearCraft = now + GearCraftGapMs;

    // Who could wear it: the guild's characters the AI plays, in the world; the crafter first.
    std::vector<Player*> wearers{ player };
    auto consider = [&](ObjectGuid guid)
    {
        Player* other = guid != player->GetGUID() ? ObjectAccessor::FindPlayer(guid) : nullptr;
        if (other && other->IsInWorld() && other->IsAlive() && BotDriver::GetAI(other))
            wearers.push_back(other);
    };
    for (auto const& [guid, member] : guild.bots)
        consider(guid);
    consider(guild.leader);

    // (A piece whose next step cannot be taken here, a vendor too far, is passed over for the next best one.)
    std::unordered_set<uint32> skip = rt.gearFailed;
    for (uint8 look = 0; look < 4; ++look)
    {
        std::string text;
        uint32 item = 0;
        Player* wearer = nullptr;
        bool forRole = false;
        GearStep step;
        bool const started = sProfessions.CraftGearFor(bot, player, wearers, text, item, wearer, forRole, &step, true, &skip);
        if (step.kind == GearStep::Kind::None || !wearer || !item)
            return false;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
        std::string const piece = proto ? proto->Name1 : "a piece";
        std::string const whom = wearer == player ? std::string("itself") : wearer->GetName();
        auto standStill = [&]()
        {
            BotDriver::Rest(player);
            // (Nothing is crafted in an animal form.)
            if (player->getClass() == CLASS_DRUID || player->GetShapeshiftForm() == FORM_GHOSTWOLF)
                player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
        };
        switch (step.kind)
        {
            case GearStep::Kind::Piece:
                if (!started)
                    break; // the queue did not take it (it is busy, the bot fights): the next best piece
                rt.gearCraftItem = item;
                rt.gearCraftFor = wearer->GetGUID();
                rt.gearCraftRole = forRole;
                rt.gearCraftHad = player->GetItemCount(item, true) + sStorageService.GetAmount(sStorageService.GetOwnerOf(player), item, 0);
                LOG_INFO(ActivityLog, "activity: {} crafts gear for the guild: {} (item level {}) for {}{} ({} item levels above what it "
                    "has there)", bot.name, piece, proto ? proto->ItemLevel : 0, whom,
                    forRole ? Acore::StringFormat(": for its dungeon set as {}", ToText(sRoleService.GetRole(wearer->GetGUID()))) : std::string(),
                    step.gain);
                standStill();
                rt.why = Acore::StringFormat("Crafting {} for {}", piece, whom);
                return true;
            case GearStep::Kind::Material:
            {
                if (!started)
                    break;
                ItemTemplate const* material = sObjectMgr->GetItemTemplate(step.item);
                LOG_INFO(ActivityLog, "activity: {} makes a material first for gear for the guild: {} ({} for {})", bot.name, text, piece, whom);
                rt.nextGearCraft = now + GearStepGapMs;
                ++rt.hubGearSteps;
                standStill();
                rt.why = Acore::StringFormat("Making {} for {} ({})", material ? material->Name1 : "a material", piece, whom);
                return true;
            }
            case GearStep::Kind::Buy:
                if ((professionMode || (rt.craftPurchases < MaxHubPurchases && rt.hubGearSteps < MaxHubGearSteps)) &&
                    PlanMaterialPurchase(bot, player, rt, step.item, step.count, professionMode ? 0.0f : HubShopYards, "Gear for the guild"))
                {
                    ItemTemplate const* material = sObjectMgr->GetItemTemplate(step.item);
                    LOG_INFO(ActivityLog, "activity: {} buys {} x {} from {} for gear for the guild: {} for {}", bot.name, rt.task.buyCount,
                        material ? material->Name1 : "a material", rt.task.targetName, piece, whom);
                    ++rt.craftPurchases;
                    ++rt.hubGearSteps;
                    rt.nextGearCraft = now + GearStepGapMs;
                    return true;
                }
                break;
            case GearStep::Kind::Focus:
                // (On a town trip: an anvil or forge next to the hub, twice per hand-in at most.)
                if (professionMode ? PlanFocusTrip(bot, player, rt, step.focus) :
                    (rt.hubFocusTrips < MaxHubFocusTrips && PlanFocusTrip(bot, player, rt, step.focus, HubFocusYards)))
                {
                    if (!professionMode)
                        ++rt.hubFocusTrips;
                    rt.nextGearCraft = now + GearStepGapMs;
                    LOG_INFO(ActivityLog, "activity: {} walks to {} for gear for the guild: {} for {}", bot.name, rt.task.targetName, piece, whom);
                    rt.why = Acore::StringFormat("On the way to {} to make {} for {}", rt.task.targetName, piece, whom);
                    return true;
                }
                break;
            default:
                return false;
        }
        skip.insert(item);
    }
    return false;
}

void OrderService::FinishGearCraft(GuildBot const& bot, Player* player, BotRuntime& rt)
{
    uint32 const entry = rt.gearCraftItem;
    ObjectGuid const forWhom = rt.gearCraftFor;
    bool const forRole = rt.gearCraftRole;
    rt.gearCraftItem = 0;
    rt.gearCraftFor.Clear();
    rt.gearCraftRole = false;
    // Nothing more of it than before: the cast failed (a tool or a place the look did not know of). Not again: an
    // engineer "crafted" Bright-Eye Goggles for itself eleven times in a night, each time without its spanner.
    if (player->GetItemCount(entry, true) + sStorageService.GetAmount(sStorageService.GetOwnerOf(player), entry, 0) <= rt.gearCraftHad)
    {
        // (The second time: once may have been a fight that broke the cast.)
        ItemTemplate const* failed = sObjectMgr->GetItemTemplate(entry);
        bool const again = !rt.gearFailedOnce.insert(entry).second;
        if (again)
            rt.gearFailed.insert(entry);
        LOG_INFO(ActivityLog, "activity: {} could not make {} (its cast failed){}", bot.name, failed ? failed->Name1 : "the piece",
            again ? " for the second time: it does not try that piece again" : "");
        return;
    }
    Item* made = player->GetItemByEntry(entry);
    if (!made || made->IsEquipped())
        return; // it wears it, or it went into the storage with what it made
    ItemTemplate const* proto = made->GetTemplate();
    if (forWhom == player->GetGUID() && forRole)
    {
        // For its own dungeon set: the role's next look takes it out of the bags into its role store.
        sRoleService.LookSoon(player->GetGUID());
        LOG_INFO(ActivityLog, "activity: {} made {} for its own dungeon set", bot.name, proto->Name1);
        return;
    }
    if (forWhom == player->GetGUID())
    {
        BotDriver::EquipUpgrades(player);
        LOG_INFO(ActivityLog, "activity: {} made {} for itself", bot.name, proto->Name1);
        return;
    }
    std::string name;
    sCharacterCache->GetCharacterNameByGuid(forWhom, name);
    std::string error;
    if (sStorageService.DepositItem(player, made, error))
        LOG_INFO(ActivityLog, "activity: {} made {} for {}{}: it lies in the guild storage for it", bot.name, proto->Name1, name,
            forRole ? "'s dungeon set" : "");
    else
        LOG_INFO(ActivityLog, "activity: {} made {} for {} and keeps it: {}", bot.name, proto->Name1, name, error);
}

uint32 OrderService::StoreProfessionGoods(GuildBot const& bot, Player* player)
{
    // Not what a quest in its log asks for: the bot took 2 Thorium Bars and 5 Arcane Crystals out of the guild storage
    // for two quests and put them straight back, four times in five minutes, round 166.
    auto keep = [player](Item* item) { return QuestLogAsksFor(player, item->GetEntry()); };
    uint32 stacks = sStorageService.DepositMaterials(player, nullptr, keep).first;
    // The products of recipes (bars, stones, bandages, what it sewed or forged), unless the bot uses them itself.
    std::vector<Item*> products;
    auto look = [&](Item* item)
    {
        if (item && !sProfessions.RecipesFor(item->GetEntry()).empty() && !BotDriver::WantsItem(player, item) && !keep(item) &&
            !sLootService.IsToolOf(player, item->GetTemplate()))
            products.push_back(item);
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        look(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = player->GetBagByPos(bagSlot))
            for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                look(bag->GetItemByPos(uint8(i)));
    for (Item* item : products)
    {
        std::string error;
        if (sStorageService.DepositItem(player, item, error, true)) // made by a recipe: ammunition too
            ++stacks;
    }
    if (stacks)
        LOG_INFO(ActivityLog, "activity: {} put {} stack{} of what it gathered and made into the guild storage (profession mode)", bot.name,
            stacks, stacks == 1 ? "" : "s");
    return stacks;
}

namespace
{
    // A creature that lives under water: it only swims, or its spawn lies more than a few yards under the surface.
    bool LivesUnderWater(Player const* player, SpawnPoint const& spawn)
    {
        constexpr float UnderWaterYards = 3.0f;
        if (CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(spawn.entry);
            info && !info->Movement.IsGroundAllowed() && info->Movement.IsSwimAllowed())
            return true;
        if (spawn.mapId != player->GetMapId())
            return false;
        LiquidData const liquid = player->GetMap()->GetLiquidData(player->GetPhaseMask(), spawn.x, spawn.y, spawn.z, 2.0f, {});
        return liquid.Level > INVALID_HEIGHT && liquid.Level - spawn.z > UnderWaterYards;
    }
}

bool OrderService::PlanSkinHunt(GuildBot const& bot, Player* player, BotRuntime& rt)
{
    if (!Professions::HasWork(bot, player, SKILL_SKINNING))
        return false;
    uint8 const level = player->GetLevel();
    // Beasts its skill is enough for and still learns from, and that the risk rules let it fight (far below its level
    // is fine: it is here for the skins), nearest first.
    std::vector<std::pair<float, MobSpawn const*>> candidates, greens;
    uint8 lowest = 255, highest = 0;
    // Orange and yellow skins (the owner, 2026-11-14; as the nodes of a gather round): what a beast asks for plus 50; green
    // ones only where there is nothing better.
    int32 const skill = int32(player->GetPureSkillValue(SKILL_SKINNING));
    auto asksFor = [](uint8 mobLevel) { return mobLevel < 10 ? 0 : mobLevel < 20 ? (int32(mobLevel) - 10) * 10 : int32(mobLevel) * 5; };
    for (MobSpawn const& mob : sWorldIndex.GetMobSpawns(player->GetMapId()))
    {
        if (!Professions::CanSkin(player, mob.maxLevel) || !Professions::SkinTeaches(player, mob.minLevel) ||
            !Professions::IsSkinKind(mob.spawn.entry) || !WorldIndex::IsAttackableFaction(mob.factionTemplate, player))
            continue;
        if (skill >= asksFor(mob.maxLevel) + 50)
        {
            if (!(mob.maxLevel > level && !Risk::CanFight(player, rt, mob.maxLevel)) && !rt.IsAvoided(mob.spawn) &&
                !rt.IsInAvoidedArea(mob.spawn.mapId, mob.spawn.x, mob.spawn.y, level))
                greens.emplace_back(Dist2d(player, mob.spawn), &mob);
            continue;
        }
        if (mob.maxLevel > level && !Risk::CanFight(player, rt, mob.maxLevel))
            continue;
        if (rt.IsAvoided(mob.spawn) || rt.IsInAvoidedArea(mob.spawn.mapId, mob.spawn.x, mob.spawn.y, level))
            continue;
        candidates.emplace_back(Dist2d(player, mob.spawn), &mob);
        lowest = std::min(lowest, mob.minLevel);
        highest = std::max(highest, mob.maxLevel);
    }
    if (candidates.empty() && !greens.empty())
    {
        candidates.swap(greens);
        for (auto const& [distance, mob] : candidates)
        {
            lowest = std::min(lowest, mob->minLevel);
            highest = std::max(highest, mob->maxLevel);
        }
    }
    std::size_t const looked = std::min(candidates.size(), ProfessionPlaceChecks);
    std::partial_sort(candidates.begin(), candidates.begin() + looked, candidates.end(),
        [](auto const& a, auto const& b) { return a.first < b.first; });
    Region const here = GetRegion(player);
    // Of the nearest, the place with the most skins to learn from within GroundYards of each other for the way
    // there (as FindHuntGround counts): a beast counts by the chance of a skill point its skin gives (all of it
    // while the skill is within 25 points of what the beast asks, three quarters up to 50, a quarter beyond), and 300
    // yd of way count like one beast less. (The nearest single spawn made a hunter walk from one empty place to the
    // next: three points in four minutes.)
    int32 const skinSkill = int32(player->GetPureSkillValue(SKILL_SKINNING));
    auto worth = [skinSkill](MobSpawn const& mob)
    {
        int32 const asks = mob.maxLevel < 10 ? 0 : mob.maxLevel < 20 ? (int32(mob.maxLevel) - 10) * 10 : int32(mob.maxLevel) * 5;
        return skinSkill < asks + 25 ? 1.0f : skinSkill < asks + 50 ? 0.75f : 0.25f;
    };
    MobSpawn const* best = nullptr;
    float bestDistance = 0.0f, bestScore = 0.0f;
    for (std::size_t i = 0; i < looked; ++i)
    {
        MobSpawn const& mob = *candidates[i].second;
        float around = 0.0f;
        for (auto const& [distance, other] : candidates)
        {
            float const dx = other->spawn.x - mob.spawn.x, dy = other->spawn.y - mob.spawn.y;
            if (dx * dx + dy * dy <= GroundYards * GroundYards)
                around += worth(*other);
        }
        float const score = std::min(around, 12.0f) - candidates[i].first / 300.0f;
        if (best && score <= bestScore)
            continue;
        if (candidates[i].first > GatherRoundDistance && sWorldIndex.GetSpawnRegion(mob.spawn) != here)
            continue;
        ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(sWorldIndex.GetSpawnZone(mob.spawn));
        if ((zone && zone->minLevel > level) || !Risk::JudgeSpot(player, rt, mob.spawn).usable)
            continue;
        // Not for skins under water (the frenzies of the Loch, 15 yd down: a hunt there is one dive after another).
        if (LivesUnderWater(player, mob.spawn))
            continue;
        best = &mob;
        bestDistance = candidates[i].first;
        bestScore = score;
    }
    if (!best)
        return false;

    BotTask task;
    task.hasSpot = true;
    task.spot = best->spawn;
    task.targetName = SpawnName(best->spawn);
    task.skinHunt = true;
    if (bestDistance > HuntGroundNear)
    {
        // A journey first; the hunt is planned when it is near.
        task.kind = TaskKind::Travel;
        task.gatherRound = true;
    }
    else
    {
        task.kind = TaskKind::Grind;
        task.grindMin = lowest;
        task.grindMax = highest;
    }
    StartTask(player, rt, task);
    rt.why = Acore::StringFormat("Profession mode: {} {} for skins (Skinning {} of {})", task.kind == TaskKind::Grind ? "hunting" :
        "on the way to", rt.task.targetName, player->GetPureSkillValue(SKILL_SKINNING), player->GetPureMaxSkillValue(SKILL_SKINNING));
    return true;
}

GuildAdventure::BotRuntime const* OrderService::GetRuntime(ObjectGuid guid) const
{
    auto itr = _runtime.find(guid);
    return itr != _runtime.end() ? &itr->second : nullptr;
}

std::optional<OrderService::HuntGround> OrderService::FindHuntGround(Player* player, BotRuntime const& rt, uint32 item, uint8 maxLevel,
    uint32 minGroup) const
{
    uint8 const level = player->GetLevel();
    // The spawns of creatures that drop it and that the character may fight alone.
    struct Source
    {
        SpawnPoint const* spawn;
        float distance;
        uint8 minLevel, maxLevel;
    };
    std::vector<SpawnPoint> const sources = sWorldIndex.GetItemSources(item);
    std::vector<Source> usable;
    std::unordered_map<uint32, std::pair<bool, std::pair<uint8, uint8>>> kinds; // entry -> (fits, its levels)
    for (SpawnPoint const& spawn : sources)
    {
        if (spawn.gameObject || spawn.mapId != player->GetMapId())
            continue;
        auto kind = kinds.find(spawn.entry);
        if (kind == kinds.end())
        {
            CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(spawn.entry);
            bool fits = cinfo && cinfo->rank == CREATURE_ELITE_NORMAL && WorldIndex::IsAttackableFaction(cinfo->faction, player);
            if (fits && maxLevel)
                fits = cinfo->maxlevel <= maxLevel;
            else if (fits && cinfo->maxlevel > level)
                fits = Risk::CanFight(player, rt, uint8(std::min<uint32>(cinfo->maxlevel, 255)));
            kind = kinds.emplace(spawn.entry, std::make_pair(fits, std::make_pair(uint8(cinfo ? std::min<uint32>(cinfo->minlevel, 255) : 0),
                uint8(cinfo ? std::min<uint32>(cinfo->maxlevel, 255) : 0)))).first;
        }
        if (!kind->second.first || rt.IsAvoided(spawn) || rt.IsInAvoidedArea(spawn.mapId, spawn.x, spawn.y, level))
            continue;
        usable.push_back({ &spawn, Dist2d(player, spawn), kind->second.second.first, kind->second.second.second });
    }
    if (usable.empty())
        return std::nullopt;

    // The nearest of them are looked at as the middle of a ground: how many of the others stand around each, and the
    // one with the most for its way wins (a way of 300 yd counts like one creature less).
    std::size_t const looked = std::min(usable.size(), ProfessionPlaceChecks * 2);
    std::partial_sort(usable.begin(), usable.begin() + looked, usable.end(), [](Source const& a, Source const& b) { return a.distance < b.distance; });
    Region const here = GetRegion(player);
    std::optional<HuntGround> best;
    float bestScore = 0.0f;
    for (std::size_t i = 0; i < looked; ++i)
    {
        Source const& middle = usable[i];
        HuntGround ground;
        ground.spot = *middle.spawn;
        ground.distance = middle.distance;
        ground.minLevel = middle.minLevel;
        ground.maxLevel = middle.maxLevel;
        for (Source const& other : usable)
        {
            float const dx = other.spawn->x - middle.spawn->x, dy = other.spawn->y - middle.spawn->y;
            if (dx * dx + dy * dy > GroundYards * GroundYards)
                continue;
            ++ground.spawns;
            ground.minLevel = std::min(ground.minLevel, other.minLevel);
            ground.maxLevel = std::max(ground.maxLevel, other.maxLevel);
        }
        if (ground.spawns < minGroup)
            continue;
        float const score = float(std::min<uint32>(ground.spawns, 12)) - middle.distance / 300.0f;
        if (best && score <= bestScore)
            continue;
        // (The closer looks last: terrain and risk lookups.)
        if (middle.distance > GatherRoundDistance && sWorldIndex.GetSpawnRegion(*middle.spawn) != here)
            continue;
        ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(sWorldIndex.GetSpawnZone(*middle.spawn));
        if ((zone && zone->minLevel > level) || !Risk::JudgeSpot(player, rt, *middle.spawn).usable)
            continue;
        best = ground;
        bestScore = score;
    }
    return best;
}

bool OrderService::PlanMaterialHunt(GuildBot const& bot, Player* player, BotRuntime& rt, uint32 item, uint32 count)
{
    // (The list of an item's sources is long, cloth has hundreds of kinds of creatures: a look every two minutes.)
    constexpr uint32 HuntLookMs = 2 * MINUTE * IN_MILLISECONDS;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    if (!proto || !count || now < rt.nextHuntLook)
        return false;
    rt.nextHuntLook = now + HuntLookMs;
    std::optional<HuntGround> const ground = FindHuntGround(player, rt, item);
    if (!ground)
        return false;

    uint32 const owner = sStorageService.GetOwnerOf(player);
    BotTask task;
    task.hasSpot = true;
    task.spot = ground->spot;
    task.targetName = SpawnName(ground->spot);
    task.huntItem = item;
    task.huntCount = player->GetItemCount(item) + (owner ? sStorageService.GetAmount(owner, item, 0) : 0) + count;
    if (ground->distance > HuntGroundNear)
    {
        // A journey first; the hunt is planned when it is near.
        task.kind = TaskKind::Travel;
        task.gatherRound = true;
    }
    else
    {
        task.kind = TaskKind::Grind;
        task.grindMin = ground->minLevel;
        task.grindMax = ground->maxLevel;
    }
    StartTask(player, rt, task);
    LOG_INFO(ActivityLog, "activity: {} hunts {} for {} x {} that a recipe of its profession lacks ({:.0f} yd{}; {} of them and their "
        "like within {:.0f} yd of each other)", bot.name, rt.task.targetName, count, proto->Name1, ground->distance,
        task.kind == TaskKind::Travel ? ", a journey first" : "", ground->spawns, GroundYards);
    rt.why = Acore::StringFormat("Profession mode: {} {} for {}", task.kind == TaskKind::Grind ? "hunting" : "on the way to",
        rt.task.targetName, proto->Name1);
    return true;
}

bool OrderService::PlanFocusTrip(GuildBot const& bot, Player* player, BotRuntime& rt, uint32 focus, float reach)
{
    uint8 const level = player->GetLevel();
    std::vector<std::pair<float, FocusSpawn const*>> candidates;
    for (FocusSpawn const& place : sProfessions.GetFocusSpawns(player->GetMapId()))
    {
        if (place.focus != focus || rt.IsAvoided(place.spawn) ||
            rt.IsInAvoidedArea(place.spawn.mapId, place.spawn.x, place.spawn.y, level))
            continue;
        if (reach > 0.0f && Dist2d(player, place.spawn) > reach)
            continue;
        candidates.emplace_back(Dist2d(player, place.spawn), &place);
    }
    if (candidates.empty() && reach > 0.0f)
        return false; // none next to the hub: nothing to say about it
    std::size_t const looked = std::min(candidates.size(), ProfessionPlaceChecks);
    std::partial_sort(candidates.begin(), candidates.begin() + looked, candidates.end(),
        [](auto const& a, auto const& b) { return a.first < b.first; });
    Region const here = GetRegion(player);
    // A bot stands at such a place for minutes: a quiet one first (no hostile creature of any level spawns within
    // QuietPlaceYards: a town's forge, an inn's fire), then one with two such spawns at most, never one in a camp.
    // (A level-27 priest cooked at a campfire on Westfall's coast: the risk verdict leaves out creatures ten levels
    // below a character, and eight murlocs of level 12 to 15 came.)
    for (uint32 const mostHostile : { 0u, QuietPlaceMostHostile })
    for (std::size_t i = 0; i < looked; ++i)
    {
        FocusSpawn const& place = *candidates[i].second;
        if (candidates[i].first > GatherRoundDistance && sWorldIndex.GetSpawnRegion(place.spawn) != here)
            continue;
        ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(sWorldIndex.GetSpawnZone(place.spawn));
        if ((zone && zone->minLevel > level) || !Risk::IsSafePlace(player, rt, place.spawn.mapId, place.spawn.x, place.spawn.y, place.spawn.z))
            continue;
        // Not in a town of the other side (RoadMap::InHostileTown): a level-35 Horde miner catching up in Duskwood
        // walked to the forge of Goldshire to smelt, every travel out of the town was then refused for the town it
        // stood in, and it went home by hearthstone, round 244.
        if (sRoadMap.InHostileTown(place.spawn.mapId, place.spawn.x, place.spawn.y, player->GetTeamId()))
            continue;
        if (sWorldIndex.CountHostileSpawns(player, place.spawn.mapId, place.spawn.x, place.spawn.y, place.spawn.z, QuietPlaceYards) > mostHostile)
            continue;
        // On the ground and on its floor: Booty Bay has ten stoves from the docks at 4 yd up to the roofs at 34 yd, and
        // the nearest by the map lay on an upper floor 18 yd above a priest on the docks, with no stair to it from
        // there; it walked under it, into the bay, and was rescued, at the same place in two rounds. A near place the
        // path finder does not lead to on its floor is left out; up to the routes' distance one it does not lead to at
        // all (from Booty Bay's eastern docks every path into the town ends in the water under the piers; a route's
        // first stretch went there too). A far one is the route's matter.
        constexpr float OwnFloorYards = 120.0f;
        if (candidates[i].first <= OwnFloorYards ? !PathEndsAt(player, place.spawn.x, place.spawn.y, place.spawn.z) :
            candidates[i].first <= sBotSettings.GetFloat(player, Setting::RoadsMinDistance) &&
                !WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), place.spawn.x, place.spawn.y,
                    place.spawn.z))
            continue;
        // Within reach by its walk, not only on the map: Skelwyn (33), catching its Mining up in Desolace, set out for the
        // forge of Shadowprey Village 1410 yd away for a few silver bars, a route of 5077 yd round the centaurs' land; it
        // walked eight minutes and gave it up for a pack on its way, 2026-11-26.
        constexpr float WalkReachFactor = 1.5f;
        if (reach > 0.0f && candidates[i].first > OwnFloorYards)
            if (float const walk = WalkLength(player, place.spawn.mapId, place.spawn.x, place.spawn.y, place.spawn.z);
                walk < 0.0f || walk > reach * WalkReachFactor)
                continue;

        BotTask task;
        task.kind = TaskKind::Travel;
        task.gatherRound = reach <= 0.0f;
        task.hubFocus = reach > 0.0f;
        task.focus = focus;
        task.hasSpot = true;
        task.spot = place.spawn;
        task.targetName = SpawnName(place.spawn);
        StartTask(player, rt, task);
        rt.why = Acore::StringFormat("{}: on the way to {} ({}) to craft there", reach > 0.0f ? "At the hub" : "Profession mode",
            rt.task.targetName, RoughDistance(candidates[i].first));
        return true;
    }
    if (reach <= 0.0f)
        LOG_INFO(ActivityLog, "activity: {} finds no place to craft at that it can go to (spell focus {})", bot.name, focus);
    return false;
}

OrderService::ProfessionWork OrderService::WorkOnProfessions(AdventureGuild& guild, GuildBot const& bot, Player* player, BotRuntime& rt,
    uint32 batch, bool professionMode, uint32 onlySkill)
{
    // At a hub (AUTO, after a hand-in): a vendor near, at most three purchases per hand-in, a few stacks milled.
    constexpr float HubShopYards = 300.0f;
    constexpr uint8 MaxHubPurchases = 3;
    constexpr float HubFocusYards = 200.0f;
    constexpr uint8 MaxHubFocusTrips = 2;
    char const* const where = professionMode ? "(profession mode)" : "at the hub";
    uint32 const millStacks = professionMode ? 8 : 4;
    uint32 const owner = sStorageService.GetOwnerOf(player);

    // An enchanter's look at the guild storage: surplus crafted gear is disenchanted, whatever its recipes lack
    // (CraftOrders::DisenchantSurplus; once a minute, three pieces a look).
    constexpr uint32 SurplusLookMs = MINUTE * IN_MILLISECONDS;
    if (uint32 const now = std::max<uint32>(getMSTime(), 1); owner && player->HasSkill(SKILL_ENCHANTING) && now >= rt.nextSurplusLook)
    {
        rt.nextSurplusLook = now + SurplusLookMs;
        sCraftOrders.DisenchantSurplus(guild, player);
    }

    // A cook's food for the guild (the owner, 2026-11-13: [quote left out]): before
    // its recipes for skill, batch after batch until the storage holds the stock of every food the guild's characters
    // eat at their levels (Professions::CookForGuild); then again after Professions.CookMinutes. The profession mode
    // walks to a fire for it; on a town trip the bot cooks when it stands at one.
    if (uint32 const now = std::max<uint32>(getMSTime(), 1); owner && player->HasSkill(SKILL_COOKING) && now >= rt.nextCookLook)
    {
        uint32 const stock = uint32(sBotSettings.Get(player, Setting::ProfessionsCookStock));
        std::vector<uint8> levels;
        auto consider = [&](ObjectGuid guid)
        {
            Player const* member = ObjectAccessor::FindConnectedPlayer(guid);
            if (uint8 const level = member ? member->GetLevel() : uint8(sCharacterCache->GetCharacterLevelByGuid(guid)))
                levels.push_back(level);
        };
        for (auto const& [guid, member] : guild.bots)
            if (member.state == BotState::Active)
                consider(guid);
        consider(guild.leader);
        uint32 fire = 0;
        std::string food;
        if (stock && sProfessions.CookForGuild(player, levels, stock, batch, food, &fire))
        {
            LOG_INFO(ActivityLog, "activity: {} cooks for the guild {}: {} (the guild storage is to hold {} of each food its "
                "characters eat)", bot.name, where, food, stock);
            BotDriver::Rest(player);
            if (player->getClass() == CLASS_DRUID || player->GetShapeshiftForm() == FORM_GHOSTWOLF)
                player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
            rt.why = "Cooking for the guild: " + food;
            return ProfessionWork::Crafting;
        }
        if (stock && fire && professionMode && PlanFocusTrip(bot, player, rt, fire))
        {
            rt.why = "Profession mode: on the way to a fire to cook for the guild";
            return ProfessionWork::Walking;
        }
        // Nothing to cook, as things are: later again. (On a town trip a fire may be a few steps on: the next look.)
        if (!fire || professionMode)
            rt.nextCookLook = now + uint32(sBotSettings.Get(player, Setting::ProfessionsCookMinutes)) * MINUTE * IN_MILLISECONDS;
    }

    // An engineer's ammunition for the guild (the owner, 2026-11-22: [quote left out]): the kind each of the guild's hunters with a gun would shoot, below the stock in the
    // guild storage (Professions::AmmoForGuild); then again after a quarter of an hour. ("Each type": a kind somebody
    // shoots; no profession makes arrows in this client.)
    if (uint32 const now = std::max<uint32>(getMSTime(), 1); owner && player->HasSkill(SKILL_ENGINEERING) && now >= rt.nextAmmoLook &&
        !AmmoClaimedByOther(guild, bot.guid, now))
    {
        uint32 const stock = uint32(sBotSettings.Get(player, Setting::ProfessionsAmmoStock));
        std::vector<Player*> const shooters = GunHunters(guild);
        uint32 forge = 0;
        std::string made;
        if (stock && !shooters.empty() && sProfessions.AmmoForGuild(player, shooters, stock, batch, made, &forge))
        {
            LOG_INFO(ActivityLog, "activity: {} makes ammunition for the guild {}: {} (the guild storage is to hold {} of each "
                "kind its hunters shoot)", bot.name, where, made, stock);
            BotDriver::Rest(player);
            if (player->getClass() == CLASS_DRUID || player->GetShapeshiftForm() == FORM_GHOSTWOLF)
                player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
            rt.why = "Making ammunition for the guild: " + made;
            ammoMaker[guild.ownerAccount] = { bot.guid, now + AmmoClaimMs };
            return ProfessionWork::Crafting;
        }
        if (stock && forge && professionMode && PlanFocusTrip(bot, player, rt, forge))
        {
            rt.why = "Profession mode: on the way to a forge for the guild's ammunition";
            ammoMaker[guild.ownerAccount] = { bot.guid, now + AmmoClaimMs };
            return ProfessionWork::Walking;
        }
        if (!forge || professionMode)
        {
            rt.nextAmmoLook = now + AmmoLookMs;
            if (auto const claim = ammoMaker.find(guild.ownerAccount); claim != ammoMaker.end() && claim->second.first == bot.guid)
                ammoMaker.erase(claim);
        }
    }

    // Endgame consumables for the guild's dungeon runs (the owner, 2026-11-25: [quote left out]): of each kind the best its recipes make for the guild's highest level, below
    // the stock in the guild storage (Professions::ConsumablesForGuild); a material it makes first, a vendor's material
    // bought, a place walked to; then again after a quarter of an hour.
    if (uint32 const now = std::max<uint32>(getMSTime(), 1); owner && now >= rt.nextConsumableLook &&
        !ConsumablesClaimedByOther(guild, bot.guid, now))
    {
        uint32 const stock = uint32(sBotSettings.Get(player, Setting::ProfessionsConsumableStock));
        uint8 const top = GuildTopLevel(guild);
        std::unordered_set<uint32> const skip = ConsumableSkips(player, rt, owner, now);
        GearStep step;
        std::string made;
        bool const found = stock && sProfessions.ConsumablesForGuild(player, top, stock, batch, made, step, true, &skip);
        ItemTemplate const* product = found ? sObjectMgr->GetItemTemplate(step.gain) : nullptr;
        std::string const what = product ? product->Name1 : std::string("a consumable");
        if (found && (step.kind == GearStep::Kind::Piece || step.kind == GearStep::Kind::Material))
        {
            LOG_INFO(ActivityLog, "activity: {} makes consumables for the guild's dungeon runs {}: {} (the guild storage is to hold {} of "
                "each kind, the best its recipes make for level {})", bot.name, where, made, stock, uint32(top));
            if (step.kind == GearStep::Kind::Piece)
            {
                rt.consumableItem = step.gain;
                rt.consumableHad = player->GetItemCount(step.gain, true) + sStorageService.GetAmount(owner, step.gain, 0);
            }
            BotDriver::Rest(player);
            if (player->getClass() == CLASS_DRUID || player->GetShapeshiftForm() == FORM_GHOSTWOLF)
                player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
            rt.why = "Making consumables for the guild: " + made;
            consumableMaker[guild.ownerAccount] = { bot.guid, now + ConsumableClaimMs };
            return ProfessionWork::Crafting;
        }
        if (found && step.kind == GearStep::Kind::Buy &&
            (professionMode || rt.craftPurchases < MaxHubPurchases) &&
            PlanMaterialPurchase(bot, player, rt, step.item, step.count, professionMode ? 0.0f : HubShopYards, "Consumables for the guild"))
        {
            ItemTemplate const* material = sObjectMgr->GetItemTemplate(step.item);
            LOG_INFO(ActivityLog, "activity: {} buys {} x {} from {} for consumables for the guild: {}", bot.name, rt.task.buyCount,
                material ? material->Name1 : "a material", rt.task.targetName, what);
            ++rt.craftPurchases;
            consumableMaker[guild.ownerAccount] = { bot.guid, now + ConsumableClaimMs };
            return ProfessionWork::Shopping;
        }
        if (found && step.kind == GearStep::Kind::Focus &&
            (professionMode ? PlanFocusTrip(bot, player, rt, step.focus) :
                (rt.hubFocusTrips < MaxHubFocusTrips && PlanFocusTrip(bot, player, rt, step.focus, HubFocusYards))))
        {
            if (!professionMode)
                ++rt.hubFocusTrips;
            LOG_INFO(ActivityLog, "activity: {} walks to {} to make consumables for the guild: {}", bot.name, rt.task.targetName, what);
            rt.why = Acore::StringFormat("On the way to {} to make {} for the guild", rt.task.targetName, what);
            consumableMaker[guild.ownerAccount] = { bot.guid, now + ConsumableClaimMs };
            return ProfessionWork::Walking;
        }
        // A material it cannot buy here or a place it does not get to: that consumable is left out for a while, so the
        // look at the level cap does not send the bot into the profession mode for it again.
        if (found && (step.kind == GearStep::Kind::Buy || step.kind == GearStep::Kind::Focus))
        {
            rt.consumableSkip[step.gain] = now + ConsumableSkipMs;
            LOG_INFO(ActivityLog, "activity: {} cannot make {} for the guild here ({}): left out for {} min", bot.name, what,
                step.kind == GearStep::Kind::Buy ? "no vendor in reach sells what it lacks" : "no place to make it in reach",
                ConsumableSkipMs / (MINUTE * IN_MILLISECONDS));
        }
        rt.nextConsumableLook = now + ConsumableLookMs;
        ReleaseConsumableClaim(guild, bot.guid);
    }

    // Raw materials in large amounts processed for the guild (the owner, 2026-11-25: [quote left out], [quote left out]):
    // Professions::ProcessForGuild; a vendor's material bought, an anvil or forge walked to; then again after a quarter of
    // an hour.
    if (uint32 const now = std::max<uint32>(getMSTime(), 1); owner && now >= rt.nextProcessLook &&
        !ProcessClaimedByOther(guild, bot.guid, now))
    {
        uint32 const above = uint32(sBotSettings.Get(player, Setting::ProfessionsProcessAbove));
        uint32 const stock = uint32(sBotSettings.Get(player, Setting::ProfessionsProcessStock));
        GearStep step;
        std::string made;
        bool found = above && sProfessions.ProcessForGuild(player, above, stock, batch, made, step, true);
        // A blacksmith's skeleton keys for the lockboxes of the guild storage (the owner, 2026-11-26): KeysForGuild.
        bool const keys = !found && sProfessions.KeysForGuild(player, batch, made, step, true);
        found = found || keys;
        ItemTemplate const* product = found ? sObjectMgr->GetItemTemplate(step.gain) : nullptr;
        std::string const what = product ? product->Name1 : std::string("a processed material");
        if (found && (step.kind == GearStep::Kind::Piece || step.kind == GearStep::Kind::Material))
        {
            if (keys)
                LOG_INFO(ActivityLog, "activity: {} makes skeleton keys for the lockboxes of the guild storage {}: {}", bot.name, where, made);
            else
                LOG_INFO(ActivityLog, "activity: {} processes raw materials of the guild storage {}: {}", bot.name, where, made);
            BotDriver::Rest(player);
            if (player->getClass() == CLASS_DRUID || player->GetShapeshiftForm() == FORM_GHOSTWOLF)
                player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
            rt.why = (keys ? "Making skeleton keys for the guild: " : "Processing materials for the guild: ") + made;
            processMaker[guild.ownerAccount] = { bot.guid, now + ProcessClaimMs };
            return ProfessionWork::Crafting;
        }
        if (found && step.kind == GearStep::Kind::Buy && (professionMode || rt.craftPurchases < MaxHubPurchases) &&
            PlanMaterialPurchase(bot, player, rt, step.item, step.count, professionMode ? 0.0f : HubShopYards, "Materials for the guild"))
        {
            ItemTemplate const* material = sObjectMgr->GetItemTemplate(step.item);
            LOG_INFO(ActivityLog, "activity: {} buys {} x {} from {} to process the guild's materials: {}", bot.name, rt.task.buyCount,
                material ? material->Name1 : "a material", rt.task.targetName, what);
            ++rt.craftPurchases;
            processMaker[guild.ownerAccount] = { bot.guid, now + ProcessClaimMs };
            return ProfessionWork::Shopping;
        }
        if (found && step.kind == GearStep::Kind::Focus &&
            (professionMode ? PlanFocusTrip(bot, player, rt, step.focus) :
                (rt.hubFocusTrips < MaxHubFocusTrips && PlanFocusTrip(bot, player, rt, step.focus, HubFocusYards))))
        {
            if (!professionMode)
                ++rt.hubFocusTrips;
            LOG_INFO(ActivityLog, "activity: {} walks to {} to process the guild's materials: {}", bot.name, rt.task.targetName, what);
            rt.why = Acore::StringFormat("On the way to {} to make {} for the guild", rt.task.targetName, what);
            processMaker[guild.ownerAccount] = { bot.guid, now + ProcessClaimMs };
            return ProfessionWork::Walking;
        }
        rt.nextProcessLook = now + ProcessLookMs;
        ReleaseProcessClaim(guild, bot.guid);
    }

    // Enchantments for the guild's gear (the owner, 2026-11-25: [quote left out]): the enchanter writes the best enchantment it can make for a piece of a
    // guild character's gear on a vellum (Professions::EnchantScrollForGuild), the scroll goes into the guild storage and
    // the wearer puts it on (UseEnchantScroll, in its supply step). A vellum the storage lacks is a craft order for the
    // guild's scribes.
    if (uint32 const now = std::max<uint32>(getMSTime(), 1); owner && player->HasSkill(SKILL_ENCHANTING) && now >= rt.nextScrollLook &&
        sBotSettings.IsOn(player, Setting::ProfessionsEnchantScrolls))
    {
        std::string made;
        uint32 lack = 0;
        if (sProfessions.EnchantScrollForGuild(player, GuildEnchantWearers(guild), made, lack, true))
        {
            LOG_INFO(ActivityLog, "activity: {} enchants a vellum for the guild {}: {}", bot.name, where, made);
            BotDriver::Rest(player);
            if (player->getClass() == CLASS_DRUID || player->GetShapeshiftForm() == FORM_GHOSTWOLF)
                player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
            rt.why = "Enchanting a scroll for the guild: " + made;
            return ProfessionWork::Crafting;
        }
        rt.nextScrollLook = now + ScrollLookMs;
        if (ItemTemplate const* vellum = lack ? sObjectMgr->GetItemTemplate(lack) : nullptr;
            vellum && !sCraftOrders.HasOpen(guild.ownerAccount, lack))
        {
            // Who of the guild in the world can make it (a scribe).
            std::string maker;
            std::vector<uint32> const& recipes = sProfessions.RecipesFor(lack);
            auto const knows = [&](Player* who)
            {
                for (uint32 recipe : recipes)
                    if (who->HasSpell(recipe))
                        return true;
                return false;
            };
            for (auto const& [guid, member] : guild.bots)
                if (Player* who = ObjectAccessor::FindConnectedPlayer(guid); who && knows(who))
                {
                    maker = member.name;
                    break;
                }
            std::string code, orderText;
            if (maker.empty())
                LOG_INFO(ActivityLog, "activity: {} has no {} to write an enchantment for the guild's gear on, and nobody of the guild in "
                    "the world knows its recipe", bot.name, vellum->Name1);
            else if (sCraftOrders.Add(guild, lack, VellumOrder, code, orderText))
                LOG_INFO(ActivityLog, "activity: {} orders {} x {} from the guild's scribes ({} can make it): it writes the enchantments "
                    "for the guild's gear on them", bot.name, VellumOrder, vellum->Name1, maker);
            else
                LOG_INFO(ActivityLog, "activity: {} cannot order {} x {}: {}", bot.name, VellumOrder, vellum->Name1, orderText);
        }
    }

    // The professions take turns: the one after the last batch's comes first. Each is looked at by itself. (They used
    // to share one look: the materials of a blocked recipe were fetched only when no profession at all could make
    // anything, and only for the cheapest blocked recipe of all. A tailor with linen in the guild storage never got
    // the rod of its Enchanting, a scribe that cooks never its pigments, an alchemist never its vials: live round 52,
    // 98 hand-ins, and Enchanting, Inscription and Alchemy stood at 1 for every character that had them.)
    std::vector<uint32> turns = onlySkill ? std::vector<uint32>{ onlySkill } : Professions::PlannedSkills(bot);
    auto const last = std::find(turns.begin(), turns.end(), rt.lastCraftSkill);
    if (last != turns.end())
        std::rotate(turns.begin(), last + 1, turns.end());
    bool disenchanted = false;
    std::string text;
    for (uint32 skill : turns)
    {
        if (!player->HasSkill(skill))
            continue;
        // A tool its orange and yellow recipes lack that another profession makes (the owner, 2026-11-14: [quote left out]).
        SeeToLackedTool(guild, bot, player, rt, skill);
        for (int look = 0; look < 3; ++look)
        {
            CraftNeed need, enchantNeed;
            // An enchanter's skill comes from enchantments on its own gear; its rod and wands are recipes like any other.
            bool const enchants = skill == SKILL_ENCHANTING && sProfessions.EnchantForSkill(bot, player, batch, text, &enchantNeed);
            bool const crafts = !enchants && sProfessions.CraftForSkill(bot, player, batch, text, &need, 0, nullptr, skill);
            if (enchants || crafts)
            {
                if (enchants)
                    LOG_INFO(ActivityLog, "activity: {} enchants its own gear for its profession {}: {}", bot.name, where, text);
                else
                    LOG_INFO(ActivityLog, "activity: {} crafts for its profession {}: {}", bot.name, where, text);
                rt.lastCraftSkill = skill;
                BotDriver::Rest(player);
                // (Nothing is crafted in an animal form: a druid's batches ended with their first cast.)
                if (player->getClass() == CLASS_DRUID || player->GetShapeshiftForm() == FORM_GHOSTWOLF)
                    player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
                rt.why = (professionMode ? "Profession mode: " : "Crafting for its profession: ") + text;
                return ProfessionWork::Crafting;
            }
            // Nothing of this profession can be made as things are. What its best blocked recipes lack and the guild
            // storage has the makings of: pigments out of its herbs, dust and essences out of pieces nobody wears.
            bool fetched = false;
            for (CraftNeed const* lacks : { &enchantNeed, &need })
            {
                if (!lacks->makeItem || !owner)
                    continue;
                if (sProfessions.IsPigment(lacks->makeItem))
                    fetched = sProfessions.MillStored(owner, player, lacks->makeItem, lacks->makeCount, millStacks) || fetched;
                else if (!disenchanted)
                {
                    // (Three pieces at most per look, whatever the recipes lack.)
                    disenchanted = true;
                    fetched = sCraftOrders.DisenchantForMaterials(guild, player, { enchantNeed.makeItem, enchantNeed.makeItem2,
                        need.makeItem, need.makeItem2 }, "for its Enchanting") || fetched;
                }
            }
            if (fetched)
                continue; // look again: the materials are in the storage now
            // A material it makes itself with a recipe it knows (bars out of the guild storage's ore, a bolt out of its
            // cloth), whatever that recipe's colour: made first, then the recipe that lacked it.
            for (CraftNeed const* lacks : { &enchantNeed, &need })
            {
                std::string made, error;
                if (!lacks->matSpell || !sCraftService.Start(player, lacks->matSpell, lacks->matCount, error, made))
                    continue;
                SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill);
                ItemTemplate const* material = sObjectMgr->GetItemTemplate(lacks->matItem);
                LOG_INFO(ActivityLog, "activity: {} makes a material first for its {} {}: {} ({} for its recipes)", bot.name,
                    line ? line->name[0] : "profession", where, made, material ? material->Name1 : "a material");
                BotDriver::Rest(player);
                if (player->getClass() == CLASS_DRUID || player->GetShapeshiftForm() == FORM_GHOSTWOLF)
                    player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
                rt.why = (professionMode ? "Profession mode: " : "Crafting for its profession: ") + made;
                return ProfessionWork::Crafting;
            }
            // A vendor's material (vials, thread, parchment, a rod).
            if (professionMode || rt.craftPurchases < MaxHubPurchases)
                for (CraftNeed const* lacks : { &enchantNeed, &need })
                    if (lacks->buyItem && PlanMaterialPurchase(bot, player, rt, lacks->buyItem, lacks->buyCount,
                            professionMode ? 0.0f : HubShopYards, professionMode ? "Profession mode" : "For its profession"))
                    {
                        ++rt.craftPurchases;
                        return ProfessionWork::Shopping;
                    }
            // The place it is made at: the profession mode walks there; on a town trip the bot walks to an anvil or
            // forge next to the hub, twice per hand-in at most (a smith's skill stood at 99 at level 47: no quest
            // giver stands at an anvil).
            for (CraftNeed const* lacks : { &enchantNeed, &need })
                if (lacks->focus && !lacks->buyItem && (professionMode ? PlanFocusTrip(bot, player, rt, lacks->focus) :
                    (rt.hubFocusTrips < MaxHubFocusTrips && PlanFocusTrip(bot, player, rt, lacks->focus, HubFocusYards))))
                {
                    if (!professionMode)
                        ++rt.hubFocusTrips;
                    return ProfessionWork::Walking;
                }
            // A material creatures drop (cloth, meat): the profession mode hunts for it.
            if (professionMode)
                for (CraftNeed const* lacks : { &enchantNeed, &need })
                    if (lacks->farmItem && PlanMaterialHunt(bot, player, rt, lacks->farmItem, lacks->farmCount))
                        return ProfessionWork::Walking;
            // Nothing orange or yellow can be made here, or what such a recipe lacks cannot be had here: the green
            // recipe it passed over (a look without what it lacks takes the best it can make now).
            bool const greenEnchant = enchantNeed.greenSpell && sProfessions.EnchantForSkill(bot, player, batch, text, nullptr);
            if (greenEnchant || (need.greenSpell && sProfessions.CraftForSkill(bot, player, batch, text, nullptr, 0, nullptr, skill)))
            {
                LOG_INFO(ActivityLog, "activity: {} {} for its profession {}: {} (green: nothing orange or yellow it can make here)",
                    bot.name, greenEnchant ? "enchants its own gear" : "crafts", where, text);
                rt.lastCraftSkill = skill;
                BotDriver::Rest(player);
                if (player->getClass() == CLASS_DRUID || player->GetShapeshiftForm() == FORM_GHOSTWOLF)
                    player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
                rt.why = (professionMode ? "Profession mode: " : "Crafting for its profession: ") + text;
                return ProfessionWork::Crafting;
            }
            break;
        }
    }
    return ProfessionWork::None;
}

void OrderService::LeaveSquadForCrafting(AdventureGuild& guild, GuildBot const& bot)
{
    auto const nameOf = [&](ObjectGuid guid) -> std::string
    {
        auto const itr = guild.bots.find(guid);
        return itr != guild.bots.end() ? itr->second.name : std::string("its leader");
    };
    std::string error;
    // A member: out of the squad of its leader (not out of a dungeon group: its leader has the order DUNGEON).
    if (ObjectGuid const leader = sSquadService.GetLeader(bot.guid); !leader.IsEmpty())
    {
        auto const itr = guild.bots.find(leader);
        bool const dungeon = itr != guild.bots.end() && itr->second.order.type == OrderType::Dungeon;
        if (!dungeon && sSquadService.Set(guild, bot.guid, ObjectGuid::Empty, error))
            LOG_INFO(ActivityLog, "activity: {} leaves the squad of {}: its order is the profession mode", bot.name, nameOf(leader));
    }
    // A leader: its members go their own ways.
    for (ObjectGuid const member : sSquadService.GetMembers(bot.guid))
        if (sSquadService.Set(guild, member, ObjectGuid::Empty, error))
            LOG_INFO(ActivityLog, "activity: {} leaves the squad of {}: its leader's order is the profession mode", nameOf(member), bot.name);
}

void OrderService::SeeToLackedTool(AdventureGuild& guild, GuildBot const& bot, Player* player, BotRuntime& rt, uint32 skill)
{
    constexpr uint32 ToolLookMs = 10 * MINUTE * IN_MILLISECONDS;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (now < rt.nextToolLook || !GetSettings().storageEnable)
        return;
    std::vector<Professions::ToolNeed> needs;
    if (!sProfessions.LackedTool(bot, player, skill, needs))
        return;
    rt.nextToolLook = now + ToolLookMs;
    SkillLineEntry const* line = sSkillLineStore.LookupEntry(skill);
    char const* const skillName = line ? line->name[0] : "profession";
    for (Professions::ToolNeed const& tool : needs)
    {
        ItemTemplate const* toolProto = sObjectMgr->GetItemTemplate(tool.tool);
        if (!toolProto)
            continue;
        if (tool.inStorage)
        {
            // Out of the guild storage into its bags.
            if (sStorageService.Remove(guild.ownerAccount, tool.tool, 0, 1) == 1)
            {
                if (player->AddItem(tool.tool, 1))
                    LOG_INFO(ActivityLog, "activity: {} took 1 x {} from the guild storage: the tool its {} recipes lack", bot.name,
                        toolProto->Name1, skillName);
                else
                    sStorageService.Add(guild.ownerAccount, toolProto, 0, 1); // no room in its bags: back it goes
            }
            continue;
        }
        ItemTemplate const* partProto = sObjectMgr->GetItemTemplate(tool.part);
        if (!partProto || !tool.count || sCraftOrders.HasOpen(guild.ownerAccount, tool.part))
            continue;
        // Who of the guild in the world can make it.
        std::string maker;
        std::vector<uint32> const& recipes = sProfessions.RecipesFor(tool.part);
        auto const knows = [&](Player* who)
        {
            for (uint32 recipe : recipes)
                if (who->HasSpell(recipe))
                    return true;
            return false;
        };
        for (auto const& [guid, member] : guild.bots)
            if (Player* who = ObjectAccessor::FindConnectedPlayer(guid); who && who != player && knows(who))
            {
                maker = member.name;
                break;
            }
        if (maker.empty())
            if (Player* leader = ObjectAccessor::FindConnectedPlayer(guild.leader); leader && leader != player && knows(leader))
                maker = leader->GetName();
        if (maker.empty())
        {
            LOG_INFO(ActivityLog, "activity: {} lacks {} for its {} recipes: {} x {} would come from another profession, and nobody "
                "of the guild in the world knows its recipe", bot.name, toolProto->Name1, skillName, tool.count, partProto->Name1);
            continue;
        }
        std::string code, text;
        if (sCraftOrders.Add(guild, tool.part, tool.count, code, text))
            LOG_INFO(ActivityLog, "activity: {} orders {} x {} from the guild's crafters for {} ({} can make it): its {} recipes lack {}",
                bot.name, tool.count, partProto->Name1, tool.part == tool.tool ? "itself" : Acore::StringFormat("its {}", toolProto->Name1),
                maker, skillName, toolProto->Name1);
        else
            LOG_INFO(ActivityLog, "activity: {} cannot order {} x {} for its {}: {}", bot.name, tool.count, partProto->Name1,
                toolProto->Name1, text);
    }
}

bool OrderService::PlanMaterialPurchase(GuildBot const& bot, Player* player, BotRuntime& rt, uint32 item, uint32 count, float reach,
    char const* why)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    if (!proto || !proto->BuyPrice || !count)
        return false;
    // As many as the guild's budget pays for.
    uint32 const bundle = std::max<uint32>(1, proto->BuyCount);
    uint64 const available = GuildBudget::Available(player);
    uint32 const bundles = uint32(std::min<uint64>((count + bundle - 1) / bundle, available / proto->BuyPrice));
    if (!bundles)
        return false;
    // A vendor with a limited stock of it only when it may have what is wanted, and not while a guild character found it
    // sold out. Coarse Blasting Powder is sold two to four at a time, back three hours later: four level-60 engineers
    // walked from vendor to vendor across Arathi, the Wetlands, Hillsbrad and the Hinterlands for 25 of it, and 879
    // purchases bought nothing in two days, round 257.
    uint32 const wanted = std::min(count, bundles * bundle);
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    std::vector<SpawnPoint> vendors;
    for (SpawnPoint const& spawn : sWorldIndex.GetItemVendors(item))
    {
        VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(spawn.entry);
        VendorItem const* sold = items ? items->FindItemCostPair(item, 0) : nullptr;
        if (sold && sold->maxcount)
        {
            if (sold->maxcount * bundle < wanted)
                continue;
            if (auto out = _soldOut.find((uint64(spawn.spawnId) << 32) | item); out != _soldOut.end() && int32(out->second - now) > 0)
                continue;
        }
        vendors.push_back(spawn);
    }
    std::optional<SpawnPoint> vendor = NearestSpawn(vendors, player, 0, true, reach > 0.0f ? reach : MaxServiceDistance, rt);
    if (!vendor)
        return false;

    BotTask task;
    task.kind = TaskKind::Vendor;
    task.hasSpot = true;
    task.spot = *vendor;
    task.targetName = SpawnName(*vendor);
    task.buyItem = item;
    task.buyCount = wanted;
    StartTask(player, rt, task);
    rt.why = Acore::StringFormat("{}: buying {} x {} from {} for a recipe", why, rt.task.buyCount, proto->Name1, rt.task.targetName);
    (void)bot;
    return true;
}

bool OrderService::StrongNearNode(Player const* player, uint32 mapId, float x, float y, float z) const
{
    constexpr float StrongNodeYards = 60.0f;
    constexpr uint8 StrongNodeLevels = 4;
    return sRoadMap.StrongestNear(mapId, x, y, z, player->GetTeamId(), StrongNodeYards) >= player->GetLevel() + StrongNodeLevels;
}

bool OrderService::NodeAmongMany(Player const* player, BotRuntime const& rt, uint32 mapId, float x, float y, float z) const
{
    // (A druid of level 60 grinding in Silithus made a detour of 93 yd for a Sungrass in the middle of the Twilight Base
    // Camp, seven Twilight Masters, Overlords and Avengers of level 58 to 61 within 50 yd of it, standing too far apart
    // for a camp by the camp rule and fewer than three within 30 yd for a guarded place; it fought two of them, then
    // three, and died on its way out, round 223.)
    constexpr float NodeYards = 45.0f;
    constexpr uint32 ManyFoes = 4;
    uint8 strongest = 0;
    uint32 const count = Risk::GuardsNear(player, mapId, x, y, z, NodeYards, &strongest);
    return count >= ManyFoes && !Risk::CanFight(player, rt, strongest, 3);
}

bool OrderService::GatherDetour(GuildBot const& bot, Player* player, BotRuntime& rt)
{
    float const reach = sBotSettings.GetFloat(player, Setting::ProfessionsGatherDetour);
    if (reach <= 0.0f || !player->IsAlive() || player->IsInCombat() || player->GetTransport() || player->IsInFlight())
        return false;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (rt.gatherNode && rt.gatherNode.IsAnyTypeCreature())
    {
        // A corpse to skin: up to it (auto loot takes its loot on the way, then skins it from a few yards).
        Creature* corpse = ObjectAccessor::GetCreature(*player, rt.gatherNode);
        bool const open = corpse && Professions::IsSkinnableCorpse(player, corpse);
        if (!open || now > rt.gatherUntil)
        {
            if (open)
                rt.gatherSkipped = rt.gatherNode;
            rt.gatherNode.Clear();
            rt.task.walkX = rt.task.walkY = 0.0f;
            rt.task.lastAssert = 0;
            rt.nextGatherCheck = now + (open ? GatherLookMs : 0);
            if (!rt.whyBeforeGather.empty())
                rt.why = std::move(rt.whyBeforeGather);
            rt.whyBeforeGather.clear();
            return false;
        }
        if (player->GetDistance(corpse) > SkinStandDistance)
        {
            if (getMSTimeDiff(rt.gatherAssert, now) >= AssertGapMs)
            {
                BotDriver::GoTo(player, player->GetMapId(), corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ(), false);
                rt.gatherAssert = now;
            }
        }
        else if (BotDriver::GetRpgState(player) != RpgState::Rest)
            BotDriver::Rest(player);
        PauseStuck(rt);
        rt.why = Acore::StringFormat("Skinning {}", corpse->GetName());
        return true;
    }
    if (rt.gatherNode)
    {
        GameObject* node = ObjectAccessor::GetGameObject(*player, rt.gatherNode);
        if (!node || node->getLootState() != GO_READY || !node->isSpawned() || now > rt.gatherUntil)
        {
            // Gathered (auto loot did it), gone, or not reached: on with the task, which sets its walk again.
            if (node && node->getLootState() == GO_READY)
                rt.gatherSkipped = rt.gatherNode;
            rt.gatherNode.Clear();
            rt.task.walkX = rt.task.walkY = 0.0f;
            rt.task.lastAssert = 0;
            rt.nextGatherCheck = now + GatherLookMs;
            // (The task's own line again: it is set when the task is planned, and a bot showed "Gathering Copper Vein on
            // the way" for the six minutes after its detour.)
            if (!rt.whyBeforeGather.empty())
                rt.why = std::move(rt.whyBeforeGather);
            rt.whyBeforeGather.clear();
            return false;
        }
        // Up to the node, then it stands: auto loot opens it from the spell's reach (a few yards).
        if (player->GetDistance(node) > GatherStandDistance)
        {
            if (getMSTimeDiff(rt.gatherAssert, now) >= AssertGapMs)
            {
                BotDriver::GoTo(player, player->GetMapId(), node->GetPositionX(), node->GetPositionY(), node->GetPositionZ(), false);
                rt.gatherAssert = now;
            }
        }
        else if (BotDriver::GetRpgState(player) != RpgState::Rest)
            BotDriver::Rest(player);
        PauseStuck(rt);
        rt.why = Acore::StringFormat("Gathering {} on the way", node->GetGOInfo()->name);
        return true;
    }

    if (now < rt.nextGatherCheck)
        return false;
    rt.nextGatherCheck = now + GatherLookMs;
    // Only while a task walks (not at an NPC, in an event, with an escort, at sea).
    switch (rt.task.kind)
    {
        case TaskKind::Travel:
        case TaskKind::Pickup:
        case TaskKind::TurnIn:
        case TaskKind::Vendor:
        case TaskKind::Objective:
        case TaskKind::Grind:
            break;
        default:
            return false;
    }
    // (Auto loot does nothing with fewer free bag slots than this: no detour for a node it would not gather.)
    if (player->GetFreeInventorySpace() < GatherMinFreeSlots)
        return false;
    // A corpse it can skin comes first (it is gone in a few minutes).
    if (Creature* corpse = Professions::FindSkinnable(player, reach); corpse && corpse->GetGUID() != rt.gatherSkipped)
    {
        rt.whyBeforeGather = rt.why;
        rt.gatherNode = corpse->GetGUID();
        rt.gatherUntil = now + GatherDetourMs;
        rt.gatherAssert = 0;
        return true;
    }
    GameObject* node = Professions::FindNode(player, reach);
    if (!node || node->GetGUID() == rt.gatherSkipped)
        return false;
    // (Nor one the path finder does not lead to: the same pocket in Hive'Ashi, round 146.)
    // (Nor one among more creatures than it takes at once, in a camp it cannot take, or whose straight way leads past
    // creatures the walk gate would refuse or into a place it keeps away from: the gather round's rules, round 223.)
    uint8 const level = player->GetLevel();
    if (!IsPlaceUsable(player, rt, node->GetMapId(), node->GetPositionX(), node->GetPositionY(), node->GetPositionZ()) ||
        StrongNearNode(player, node->GetMapId(), node->GetPositionX(), node->GetPositionY(), node->GetPositionZ()) ||
        NodeAmongMany(player, rt, node->GetMapId(), node->GetPositionX(), node->GetPositionY(), node->GetPositionZ()) ||
        Risk::InCampItCannotTake(player, rt, node->GetMapId(), node->GetPositionX(), node->GetPositionY(), node->GetPositionZ()) ||
        Risk::EntersCamp(player, rt, player->GetMapId(), player->GetPositionX(), player->GetPositionY(), node->GetPositionX(),
            node->GetPositionY()) ||
        rt.CrossesAvoidedArea(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), node->GetPositionX(), node->GetPositionY(),
            level) ||
        int32(sRoadMap.DangerAlong(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), node->GetPositionX(),
            node->GetPositionY(), player->GetTeamId())) - int32(level) > int32(sBotSettings.Get(player, Setting::WalksMaxLevelsAbove)) ||
        !WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), node->GetPositionX(),
            node->GetPositionY(), node->GetPositionZ()))
    {
        rt.gatherSkipped = node->GetGUID();
        return false;
    }
    rt.whyBeforeGather = rt.why;
    rt.gatherNode = node->GetGUID();
    rt.gatherUntil = now + GatherDetourMs;
    rt.gatherAssert = 0;
    LOG_INFO(ActivityLog, "activity: {} makes a detour of {:.0f} yd for {} (profession plan)", bot.name, player->GetDistance(node),
        node->GetGOInfo()->name);
    return true;
}

bool OrderService::SwimToSurface(Player* player) const
{
    LiquidData const liquid = player->GetMap()->GetLiquidData(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(),
        player->GetPositionZ(), player->GetCollisionHeight(), {});
    if (liquid.Level <= INVALID_HEIGHT)
        return false;
    if (BotDriver::GetRpgState(player) != RpgState::Rest)
        BotDriver::Rest(player);
    // Straight up: no path is needed in water.
    player->GetMotionMaster()->MovePoint(0, player->GetPositionX(), player->GetPositionY(), liquid.Level - 0.3f, FORCED_MOVEMENT_NONE, 0.0f,
        0.0f, false, false);
    return true;
}

void OrderService::TrackBreath(Player* player, BotRuntime& rt)
{
    constexpr uint32 RegenFactor = 10; // Player::UpdateMirrorTimers
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    uint32 const passed = rt.breathAt ? std::min<uint32>(getMSTimeDiff(rt.breathAt, now), 10 * IN_MILLISECONDS) : 0;
    rt.breathAt = now;
    if (!player->IsAlive())
        rt.breathUsedMs = 0;
    else if (IsDiving(player) && BreathMs(player))
        rt.breathUsedMs += passed;
    else
        rt.breathUsedMs = rt.breathUsedMs > passed * RegenFactor ? rt.breathUsedMs - passed * RegenFactor : 0;

    // The fatigue bar the same way (Player::HandleDrowning): dark water, the open sea of the core's map data.
    bool dark = false;
    if (player->IsAlive() && !player->IsInFlight() && !player->GetTransport())
    {
        LiquidData const liquid = player->GetMap()->GetLiquidData(player->GetPhaseMask(), player->GetPositionX(),
            player->GetPositionY(), player->GetPositionZ(), player->GetCollisionHeight(), {});
        dark = liquid.Status != LIQUID_MAP_NO_WATER && (liquid.Flags & MAP_LIQUID_TYPE_DARK_WATER);
    }
    if (!player->IsAlive())
        rt.fatigueUsedMs = 0;
    else if (dark)
        rt.fatigueUsedMs += passed;
    else
        rt.fatigueUsedMs = rt.fatigueUsedMs > passed * RegenFactor ? rt.fatigueUsedMs - passed * RegenFactor : 0;
}

uint32 OrderService::BreathMs(Player* player)
{
    if (!player || !player->IsAlive() || player->HasWaterBreathingAura())
        return 0;
#ifdef GUILDADVENTURE_CP8_PERK_HOOKS
    bool breathes = false;
    sScriptMgr->OnPlayerCheckWaterBreathing(player, breathes);
    if (breathes)
        return 0;
#endif
    float const breath = float(sWorld->getIntConfig(CONFIG_WATER_BREATH_TIMER)) *
        player->GetTotalAuraMultiplier(SPELL_AURA_MOD_WATER_BREATHING);
    return breath > 0.0f ? uint32(breath) : 0;
}

bool OrderService::CheckBreath(Player* player, BotRuntime& rt)
{
    // Up for air when the breath left (the game's count: the breath the character has, BreathMs, less what it has used,
    // TrackBreath) is just enough for the way up and this reserve; in a fight with the smaller one (moving up breaks
    // the fight: the bot's casts stop, and three bots died in fights they were pulled out of with two minutes of
    // breath left). The way up counts half again (a bot 30 yd down needs seven seconds, more past a ledge). Until
    // 2026-11-25 a bot came up after a third of the server's WaterBreath.Timer, 60 s of 180, whatever was left (the owner:
    // [quote left out]), and an undead's longer breath and a Water Breathing aura or the guild perk
    // Gills were not counted at all. The breath counted is the game's, not the time since its last breath: a mage
    // hunting crawlers on a lake's floor bobbed up for a moment now and then, its dive clock started anew each time,
    // and it drowned 65 s into a dive. Then this long at the surface; the way up may take what is left of the breath
    // less a few seconds (at most SurfaceMs), then the bot is brought up.
    constexpr uint32 ReserveMs = 25 * IN_MILLISECONDS;
    constexpr uint32 ReserveInFightMs = 12 * IN_MILLISECONDS;
    constexpr uint32 WayUpSpareMs = 8 * IN_MILLISECONDS;
    constexpr float RiseYardsPerSecond = 4.0f;
    constexpr uint32 AirMs = 8 * IN_MILLISECONDS;
    constexpr uint32 SurfaceMs = 45 * IN_MILLISECONDS;

    uint32 const now = std::max<uint32>(getMSTime(), 1);
    bool const diving = IsDiving(player);
    auto swimUp = [&]() { return SwimToSurface(player); };

    if (rt.airUntil)
    {
        if (!diving)
        {
            // At the surface: a few breaths (until the breath is back, at most twice the time), then on with the task;
            // its AI is held there (its hunt took it down again at once, and the breaths did not count).
            if (!rt.airReachedAt)
                rt.airReachedAt = now;
            if (now - rt.airReachedAt < AirMs || (rt.breathUsedMs && now - rt.airReachedAt < 2 * AirMs))
            {
                BotDriver::PauseAI(player, 2 * std::max(TickMs, rt.stepGapMs));
                rt.why = "Catching its breath at the surface";
                return true;
            }
        }
        else if (now < rt.airUntil)
        {
            // (Also in a fight: the bot only comes up in one when its breath is nearly gone.)
            // The bot's AI is held on the way up: its hunt for the next target moved it at once, and the way up
            // never began (a level-31 warrior hunting the frenzies of the Loch "came up for air" three times at the
            // same point, 15 yd down, and drowned). The step up is set again whenever the bot has not risen.
            BotDriver::PauseAI(player, 2 * std::max(TickMs, rt.stepGapMs));
            if (!player->isMoving() || player->GetPositionZ() < rt.airLastZ + 0.5f)
                swimUp();
            rt.airLastZ = player->GetPositionZ();
            rt.why = "Coming up for air";
            return true;
        }
        else
        {
            // Its time for the way up is over and it is still under water: it is brought to the surface (the breath
            // the game gives does not wait), and takes its breaths there.
            LiquidData const liquid = player->GetMap()->GetLiquidData(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(),
                player->GetPositionZ(), player->GetCollisionHeight(), {});
            if (liquid.Level > INVALID_HEIGHT && !player->IsInCombat())
            {
                LOG_INFO(ActivityLog, "activity: {} did not get to the surface in time (it is at ({:.0f}, {:.0f}, {:.0f}), the surface at "
                    "{:.0f}): it is brought up", player->GetName(), player->GetPositionX(),
                    player->GetPositionY(), player->GetPositionZ(), liquid.Level);
                BotDriver::PauseAI(player, 2 * std::max(TickMs, rt.stepGapMs));
                BotDriver::Hop(player, player->GetPositionX(), player->GetPositionY(), liquid.Level - 0.3f);
                rt.hasLastPosition = false; // no jump for the planner
                rt.airUntil = now + AirMs + 2 * IN_MILLISECONDS;
                rt.airReachedAt = now;
                rt.why = "Catching its breath at the surface";
                return true;
            }
            // (In a fight it stays where it is; the dive's clock runs on, so the next look sends it up again.)
            rt.airUntil = 0;
            rt.airReachedAt = 0;
            return false;
        }
        rt.airUntil = 0;
        rt.airReachedAt = 0;
        rt.diveSince = 0;
        rt.resumeAfterAir = true;
        return false;
    }

    if (!diving)
    {
        rt.diveSince = 0;
        return false;
    }
    if (!rt.diveSince)
    {
        rt.diveSince = now;
        return false;
    }
    uint32 const breath = BreathMs(player);
    if (!breath)
        return false; // it breathes under water
    LiquidData const above = player->GetMap()->GetLiquidData(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(),
        player->GetPositionZ(), player->GetCollisionHeight(), {});
    float const depth = above.Level > INVALID_HEIGHT ? std::max(0.0f, above.Level - player->GetPositionZ()) : 0.0f;
    uint32 const riseMs = uint32(depth / RiseYardsPerSecond * float(IN_MILLISECONDS) * 1.5f);
    uint32 const left = breath > rt.breathUsedMs ? breath - rt.breathUsedMs : 0;
    if (left > riseMs + (player->IsInCombat() ? ReserveInFightMs : ReserveMs) || !swimUp())
        return false;

    LOG_INFO(ActivityLog, "activity: {} comes up for air at ({:.0f}, {:.0f}, {:.0f}) map {} after {} s under water ({:.0f} yd deep{}; "
        "{} s of its {} s of breath used)", player->GetName(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
        player->GetMapId(), (now - rt.diveSince) / IN_MILLISECONDS, depth, player->IsInCombat() ? ", in a fight" : "",
        rt.breathUsedMs / IN_MILLISECONDS, breath / IN_MILLISECONDS);
    rt.airUntil = now + std::min<uint32>(SurfaceMs, std::max<uint32>({ left > WayUpSpareMs ? left - WayUpSpareMs : 0, riseMs,
        4 * IN_MILLISECONDS }));
    rt.airReachedAt = 0;
    rt.airLastZ = player->GetPositionZ();
    BotDriver::PauseAI(player, 2 * std::max(TickMs, rt.stepGapMs));
    rt.why = "Coming up for air";
    return true;
}

bool OrderService::IsAtWaterTarget(Player* player, BotRuntime const& rt) const
{
    constexpr float WaterTargetYards = 150.0f;
    BotTask const& task = rt.task;
    if (!task.hasSpot || task.spot.mapId != player->GetMapId() || (!IsSwimming(player) && !IsDiving(player)))
        return false;
    // (And a gather round's walk to a node: a death knight mined an Iron Deposit in a sea cave under Faldir's Cove, 65 yd
    // down, and its next node 25 yd from it was "cut off from every road", round 183.)
    if (task.kind != TaskKind::Objective && task.kind != TaskKind::Interact && task.kind != TaskKind::Event &&
        task.kind != TaskKind::Grind && task.kind != TaskKind::Fish && !(task.kind == TaskKind::Travel && (task.gatherRound || task.fishTrip)))
        return false;
    return Dist2d(player, task.spot) <= WaterTargetYards;
}

bool OrderService::IsCutOff(Player* player) const
{
    // The twelve nearest nodes of the road map (eight until the Cauldron's floor, whose way out leads up to Dustfire
    // Valley, 650 yd off, past the nodes of its scaffolds and of Thorium Point above), at least a few steps away: the path
    // finder walks to none of them. (Three
    // were too few: below Thorium Point the four nearest are the town's nodes on the rim, and the Cauldron's floor, where
    // the guild leader stood, has its own node 360 yd away that it walked to.)
    float minDistance = 30.0f;
    int asked = 0;
    int skipped = 0;
    for (; asked < 12; ++asked)
    {
        RoutePoint node;
        std::string name;
        if (!sRoadMap.FindNearestNode(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), minDistance, RescueRange, node,
            name))
            break;
        minDistance = std::sqrt((node.x - player->GetPositionX()) * (node.x - player->GetPositionX()) +
            (node.y - player->GetPositionY()) * (node.y - player->GetPositionY())) + 10.0f;
        // (Not a node off the navmesh, a lift's: nobody walks to it, and it is not asked in place of a real one. The
        // Cauldron's floor had a druid "cut off" while its way out to the south was open.)
        if (!NodeOnNavmesh(player, node))
        {
            --asked;
            if (++skipped > 8)
                break;
            continue;
        }
        if (WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), node.x, node.y, node.z))
            return false;
    }
    return asked > 0;
}

void OrderService::SnapToNavmesh(Player* player, uint32 mapId, float& x, float& y, float& z) const
{
    if (mapId != player->GetMapId())
        return;
    // Towards a node of the road map some way off: any walk from the spot starts the same way.
    RoutePoint toward;
    std::string name;
    if (!sRoadMap.FindNearestNode(mapId, x, y, 40.0f, RescueRange, toward, name))
        return;
    float sx = 0.0f, sy = 0.0f, sz = 0.0f;
    if (!NavmeshStart(player, x, y, z, toward.x, toward.y, toward.z, sx, sy, sz))
        return;
    float const off = std::sqrt((sx - x) * (sx - x) + (sy - y) * (sy - y) + (sz - z) * (sz - z));
    if (off <= 3.0f || off > 40.0f)
        return;
    x = sx;
    y = sy;
    z = sz;
}

bool OrderService::StandsOnNavmesh(Player* player) const
{
    constexpr float StepYards = 3.0f;
    float const x = player->GetPositionX();
    float const y = player->GetPositionY();
    float const z = player->GetPositionZ();
    for (int i = 0; i < 4; ++i)
    {
        float const px = x + std::cos(float(i) * float(M_PI) / 2.0f) * StepYards;
        float const py = y + std::sin(float(i) * float(M_PI) / 2.0f) * StepYards;
        PathReport const step = ProbePath(player, x, y, z, px, py, z);
        if (step.valid && !(step.type & (PATHFIND_NOT_USING_PATH | PATHFIND_FARFROMPOLY_START | PATHFIND_NOPATH)))
            return true;
    }
    return false;
}

bool OrderService::IsOffNavmesh(Player* player) const
{
    // A swimmer is not off the walkable ground: under water the path finder has straight lines only, and that is fine.
    // (Nor one on the floor of a lake with the water over it: the core counts no water where a bot's height lies a hair
    // under the floor, and a shaman that had fought Plague Ravagers on the floor of Lake Mereldar, 7 yd under its
    // surface, was rescued as standing off the walkable ground, round 156.)
    if (player->GetTransport() || IsSwimming(player) || player->IsFlying() || player->IsFalling() ||
        player->GetMap()->IsInWater(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ() + 1.0f,
            player->GetCollisionHeight()))
        return false;

    // The ground right around the bot first: a path from where it stands to a point of the ground a few yards away. With
    // a polygon under the bot the core answers with a path in some direction. (The nearest road nodes alone mislead:
    // the lifts of the Searing Gorge's Cauldron are nodes without a polygon of their own, the core answers "not using a
    // path" to them, and the guild leader was rescued from the Cauldron's floor while it stood on the navmesh.)
    {
        constexpr float AroundYards = 4.0f;
        Map* map = player->GetMap();
        int asked = 0;
        for (int i = 0; i < 8; ++i)
        {
            float const px = player->GetPositionX() + std::cos(float(i) * float(M_PI) / 4.0f) * AroundYards;
            float const py = player->GetPositionY() + std::sin(float(i) * float(M_PI) / 4.0f) * AroundYards;
            float const pz = map->GetHeight(player->GetPhaseMask(), px, py, player->GetPositionZ() + 3.0f, true, 8.0f);
            if (pz <= INVALID_HEIGHT)
                continue;
            ++asked;
            PathReport const step = ProbePath(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), px, py, pz);
            if (step.valid && !(step.type & (PATHFIND_NOT_USING_PATH | PATHFIND_FARFROMPOLY_START)))
                return false;
        }
        if (asked < 3)
            return false;
    }
    // The path finder answers a start (or an end) outside the navmesh with a straight line. Road map nodes lie on the
    // navmesh, so a straight line to the nearest two of them says it is the start. One node could be a bad one.
    float minDistance = 15.0f;
    for (int i = 0; i < 2; ++i)
    {
        RoutePoint node;
        std::string name;
        if (!sRoadMap.FindNearestNode(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), minDistance, RescueRange,
            node, name))
            return false;
        PathReport const report = ProbePath(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), node.x,
            node.y, node.z);
        // "Not using a path": the path finder found no polygon under the start (the node has one). "Start far from the
        // navmesh": the nearest polygon is more than seven yards away (a bot on a slope above a cave in Darkshore had
        // fourteen movements cancelled and four targets set aside one after the other). "No path" alone is a spot on
        // the navmesh that no way leads from to that node (a pit, a far node), which is not this.
        if (!report.valid || !(report.type & (PATHFIND_NOT_USING_PATH | PATHFIND_FARFROMPOLY_START)))
            return false;
        minDistance = std::sqrt((node.x - player->GetPositionX()) * (node.x - player->GetPositionX()) +
            (node.y - player->GetPositionY()) * (node.y - player->GetPositionY())) + 10.0f;
    }
    return true;
}

bool OrderService::CheckStranded(GuildBot const& /*bot*/, Player* player, BotRuntime& rt)
{
    // Not moved this far in this time, with a task that walks: stranded.
    constexpr float StrandedDistance = 15.0f;
    constexpr uint32 StrandedMs = 5 * MINUTE * IN_MILLISECONDS;
    // Under the ground: no floor this far below the feet and the surface this far above the head, for this long.
    constexpr float UnderFloorSearch = 15.0f;
    constexpr float UnderSurface = 10.0f;
    constexpr uint32 UnderGroundMs = 10 * IN_MILLISECONDS;
    constexpr uint32 UnderGroundCheckMs = 3 * IN_MILLISECONDS;
    // Standing this long with a task that walks: is the bot off the navmesh?
    constexpr uint32 OffMeshMs = 30 * IN_MILLISECONDS;

    uint32 const now = getMSTime();
    auto rescue = [&](char const* what, float minDistance) { return Rescue(player, rt, what, minDistance); };

    // (0) In the open sea, where the fatigue bar runs (the core's own test: dark water, Player::HandleDrowning), by the
    // bar as the game counts it (TrackBreath): 40 of its 60 s used and it is put back on the ground, before the damage
    // begins. A bot got there on a route whose link crossed the open sea (round 37: such links are left out since); the
    // short swims the road map keeps, to Fray Island or the Echo Isles, cross some dark water too, and a rule of 8 s in it
    // took a bot out of the strait to Fray Island halfway across (round 272).
    {
        // (40 s: a swim the road map lets stand, up to 250 yd to Fray Island off the Barrens, takes about half a minute,
        // and three bots were rescued 8 s into it, 2026-11-26/27; the test guild thread's number for the same case.)
        constexpr uint32 FatigueRescueMs = 40 * IN_MILLISECONDS;
        if (rt.fatigueUsedMs >= FatigueRescueMs && !player->IsInFlight() && !player->GetTransport() && player->IsAlive())
        {
            rt.fatigueUsedMs = 0;
            if (rescue("swims in the open sea, where fatigue kills", 0.0f))
                return true;
        }
    }

    // (1) Under the ground (a straight spline through a hill that was cut short, a bad height after a teleport): no
    // task can get the bot out of there.
    if (now >= rt.nextUnderGroundCheck)
    {
        rt.nextUnderGroundCheck = now + UnderGroundCheckMs;
        bool under = false;
        if (!player->IsFalling() && !IsSwimming(player) && !player->IsFlying())
        {
            Map const* map = player->GetMap();
            float const z = player->GetPositionZ();
            float const floor = map->GetHeight(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(), z + 2.0f, true,
                UnderFloorSearch);
            float const surface = map->GetHeight(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(), MAX_HEIGHT, true,
                MAX_HEIGHT);
            under = (floor <= INVALID_HEIGHT || floor < z - UnderFloorSearch) && surface > INVALID_HEIGHT && surface - z > UnderSurface;
            // (Not on the path finder's ground: a gully of the Eastern Plaguelands, a vein and a Diseased Flayer in it,
            // 32 yd below the terrain, has a navmesh and no floor of the collision data; a warrior that rested there
            // after its escape was "rescued" from it 16 seconds later, round 150. A bot inside rock or under the world
            // has no polygon under its feet.)
            if (under && StandsOnNavmesh(player))
                under = false;
        }
        if (!under)
            rt.underGroundSince = 0;
        else if (!rt.underGroundSince)
            rt.underGroundSince = now;
        else if (getMSTimeDiff(rt.underGroundSince, now) >= UnderGroundMs &&
            (LiftOutOfGround(player, rt, "it was under the ground") || rescue("is under the ground", 0.0f)))
            return true;
    }

    // (2) Not moved although the task walks.
    bool const walks = TaskWalks(rt.task);
    if (!rt.strandSince || Dist2d(player, rt.strandX, rt.strandY) > StrandedDistance)
    {
        rt.strandX = player->GetPositionX();
        rt.strandY = player->GetPositionY();
        rt.strandSince = now;
        return false;
    }
    // An interaction that stands still: the bot has not moved for four minutes and the quest has not got anywhere in
    // that time (a warlock stood at a used-up Lumber Pile for more than six minutes, "task interact (368 s)", moved
    // 0 yd). The task ends, its spot is set aside and it counts against the quest like any blocked end.
    if (rt.task.kind == TaskKind::Interact && rt.task.questId)
    {
        constexpr uint32 InteractIdleMs = 4 * MINUTE * IN_MILLISECONDS;
        uint32 const mark = QuestMark(player, rt.task.questId);
        if (rt.interactQuest != rt.task.questId || mark != rt.interactMark)
        {
            rt.interactQuest = rt.task.questId;
            rt.interactMark = mark;
            rt.interactMarkAt = now;
        }
        if (!player->IsInCombat() && getMSTimeDiff(rt.strandSince, now) >= InteractIdleMs &&
            getMSTimeDiff(rt.interactMarkAt, now) >= InteractIdleMs)
        {
            LOG_INFO(ActivityLog, "activity: {} has stood for {} s at {} without quest {} getting anywhere: the interaction ends, the "
                "spot is set aside for {} min", player->GetName(), getMSTimeDiff(rt.strandSince, now) / IN_MILLISECONDS,
                rt.task.targetName.empty() ? "its spot" : rt.task.targetName, rt.task.questId, SpotRetryMs / (MINUTE * IN_MILLISECONDS));
            if (rt.task.hasSpot)
            {
                rt.Avoid(rt.task.spot, SpotRetryMs);
                rt.AddAvoidArea(rt.task.spot.mapId, rt.task.spot.x, rt.task.spot.y, 255, SpotRetryMs, false);
            }
            rt.missQuest = rt.task.questId;
            rt.missMark = mark;
            rt.strandSince = now;
            ClearTask(rt, 0, "no progress standing still");
        }
        return false;
    }
    // An objective at a quest area (no spawn: the quest's point) where the bot stands and its quest gets nowhere for four
    // minutes: the task ends and the quest waits a quarter of an hour. (A druid stood at the quest area of "Bloodpetal
    // Poison" in Un'Goro for five minutes, NewRpg "doing" the quest, until a rescue put it on a road.)
    // (Also once it had come near the area and NewRpg sent it on from there: a rogue stood five minutes, mounted, 300 yd
    // from the area of "The Twilight Lexicon" in Silithus, where NewRpg's walk to the next point of the quest had
    // stopped, until the rescue; the stuck rules of the way there end once the bot was near, round 143.)
    if (rt.task.kind == TaskKind::Objective && rt.task.questId && (rt.task.hasSpot || rt.task.questArea) && !rt.task.spot.spawnId &&
        (Dist2d(player, rt.task.spot) <= 60.0f || rt.task.nearSince) && !player->IsInCombat())
    {
        constexpr uint32 AreaIdleMs = 4 * MINUTE * IN_MILLISECONDS;
        uint32 const mark = QuestMark(player, rt.task.questId);
        if (rt.areaIdleQuest != rt.task.questId || mark != rt.areaIdleMark || !rt.areaIdleSince)
        {
            rt.areaIdleQuest = rt.task.questId;
            rt.areaIdleMark = mark;
            rt.areaIdleSince = now ? now : 1;
        }
        else if (getMSTimeDiff(rt.areaIdleSince, now) >= AreaIdleMs && getMSTimeDiff(rt.strandSince, now) >= AreaIdleMs)
        {
            LOG_INFO(ActivityLog, "activity: {} has stood at the quest area of quest {} \"{}\" for {} s without the quest getting "
                "anywhere: the task ends, the quest waits a quarter of an hour", player->GetName(), rt.task.questId,
                QuestTitle(rt.task.questId), getMSTimeDiff(rt.areaIdleSince, now) / IN_MILLISECONDS);
            BotRuntime::Danger& danger = rt.dangerousQuests[rt.task.questId];
            danger.level = player->GetLevel();
            danger.until = now + SpotRetryMs;
            rt.AddAvoidArea(rt.task.spot.mapId, rt.task.spot.x, rt.task.spot.y, 255, SpotRetryMs, false);
            rt.areaIdleSince = 0;
            rt.strandSince = now;
            ClearTask(rt, 0, "no progress at the quest area");
            return false;
        }
    }
    // Between two tasks, and while a task has the bot wait (a hold, the inn, a ship), the clock stands still: a walk
    // that fails every two minutes and starts again must still add up. So it does while the bot waits on purpose: away
    // from a creature, or for one on its way to move on (a druid beside Stonard waited 75 s for a patrolling grunt to
    // move on, its task ended, and one second later it was "rescued" for five minutes without moving).
    bool const waitsOnPurpose = rt.escape.active || (rt.task.roamerWaitUntil && now < rt.task.roamerWaitUntil);
    if (!walks || waitsOnPurpose)
    {
        rt.strandSince += rt.stepGapMs;
        return false;
    }
    // Off the navmesh (a slope the bot slid onto in a fight, a roof): NewRpg finds no path from there and does not even
    // start to walk, so nothing is cancelled and nothing else notices. No need to wait the five minutes.
    if (getMSTimeDiff(rt.strandSince, now) >= OffMeshMs && now >= rt.nextOffMeshCheck)
    {
        rt.nextOffMeshCheck = now + OffMeshMs;
        // A bot in the water swims first (StepSwim), once per task: the water has no navmesh (a warrior farming the
        // Loch's frenzies was put on a road from the lake's floor every few minutes).
        if ((IsSwimming(player) || IsDiving(player)) && rt.task.hasSpot && !rt.task.swimLooked)
        {
            rt.task.swimArmed = true;
            return false;
        }
        if (IsOffNavmesh(player) && (LiftOutOfGround(player, rt, "it stood off the walkable ground") ||
            StepOffLedge(player, rt, "it stands off the walkable ground") ||
            rescue("stands off the walkable ground", 0.0f)))
            return true;
        // On a patch of navmesh that nothing leads from (a boulder, a stump: there is a polygon under the bot, but the
        // path finder gets nowhere from it): every movement is cancelled as a straight line ("shortcut + no path"), and
        // the nearest road nodes are out of reach.
        constexpr uint32 CutOffWindowMs = 2 * MINUTE * IN_MILLISECONDS;
        constexpr uint32 CutOffRefusals = 3;
        if (sGroundGuard.GetRefusals(player->GetGUID(), CutOffWindowMs, false).count >= CutOffRefusals && !IsAtWaterTarget(player, rt) &&
            IsCutOff(player))
        {
            rt.strandSince = now;
            if (CutOffWayOut(player, rt, "is cut off from every road"))
                return true;
        }
    }
    if (getMSTimeDiff(rt.strandSince, now) < StrandedMs)
        return false;

    rt.strandSince = now; // one try per period
    return rescue("has not moved for 5 min", 30.0f);
}

bool OrderService::HopDownToFight(Player* player, BotRuntime& rt)
{
    // Dire Maul West, 2026-11-20: when the pylons were down the tank walked onto the top of Immol'thar's force field,
    // 18 yd above the boss (the path finder's ground has it), and stood there for 40 minutes in the fight, every step
    // down refused as a drop with no way back up; the group below died. Inside a dungeon it is put on the ground
    // beside what it fights, as the group is put before a boss it has no way on foot to (DG6).
    constexpr float MinHeight = 8.0f;
    constexpr float MaxAcross = 40.0f;
    Map* map = player->GetMap();
    if (!map || !map->IsDungeon() || !GetSettings().dungeonsHopWhenNoWay || !player->IsAlive())
        return false;
    constexpr uint32 LookEveryMs = 3 * IN_MILLISECONDS;
    constexpr uint32 StuckMs = 8 * IN_MILLISECONDS;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (rt.fightHopAt && getMSTimeDiff(rt.fightHopAt, now) < MINUTE * IN_MILLISECONDS)
        return false;
    if (rt.fightHopLook && getMSTimeDiff(rt.fightHopLook, now) < LookEveryMs)
        return false;
    rt.fightHopLook = now;
    Unit* enemy = player->GetVictim();
    if (!enemy || !enemy->IsAlive())
    {
        enemy = nullptr;
        for (auto const& [guid, ref] : player->GetCombatManager().GetPvECombatRefs())
            if (Unit* other = ref->GetOther(player); other && other->IsAlive() && other->IsInWorld() &&
                other->GetMapId() == player->GetMapId() && (!enemy || player->GetExactDist(other) < player->GetExactDist(enemy)))
                enemy = other;
    }
    if (!enemy || std::fabs(enemy->GetPositionZ() - player->GetPositionZ()) < MinHeight || player->GetExactDist2d(enemy) > MaxAcross ||
        WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), enemy->GetPositionX(),
            enemy->GetPositionY(), enemy->GetPositionZ()))
    {
        rt.fightHopSince = 0;
        return false;
    }
    // A way on the whole navmesh, down a drop or across water as players go (Blackfathom Deeps, 2026-11-25: the healer
    // above a pool was put beside a myrmidon 17 yd below it in the water, where a jump leads): it walks that way.
    constexpr float FightWayYards = 120.0f;
    if (float length = 0.0f; sDungeonNav.FindWay(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
            enemy->GetPositionX(), enemy->GetPositionY(), enemy->GetPositionZ(), nullptr, &length, map) && length <= FightWayYards &&
        WalkDungeonWay(player, enemy->GetPositionX(), enemy->GetPositionY(), enemy->GetPositionZ(), true))
    {
        rt.fightHopSince = 0;
        return true;
    }
    // (For some seconds: not for a creature that only jumps down a ledge.)
    if (!rt.fightHopSince)
        rt.fightHopSince = now;
    if (getMSTimeDiff(rt.fightHopSince, now) < StuckMs)
        return false;
    rt.fightHopSince = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (!FindHopSpot(player, enemy->GetPositionX(), enemy->GetPositionY(), enemy->GetPositionZ(), x, y, z, 6.0f, 12.0f))
        return false;
    rt.fightHopAt = now;
    LOG_INFO(ActivityLog, "activity: {} fights {} (level {}) {:.0f} yd {} it in {} with no way on foot to it: it is put beside it at ({:.0f}, {:.0f}, {:.0f})",
        player->GetName(), enemy->GetName(), uint32(enemy->GetLevel()), std::fabs(enemy->GetPositionZ() - player->GetPositionZ()),
        enemy->GetPositionZ() < player->GetPositionZ() ? "below" : "above", map->GetMapName(), x, y, z);
    sGroundGuard.Forget(player->GetGUID());
    player->NearTeleportTo(x, y, z, player->GetAngle(enemy));
    rt.hasLastPosition = false; // no jump for the planner
    return true;
}

bool OrderService::GroundWayOut(Player* player, BotRuntime& rt, char const* what)
{
    // A teleporter in the room (the sanctum of Stormwind's wizard tower has no other way out): the bot takes it, as a
    // player who steps into it.
    constexpr float PortalRange = 60.0f;
    constexpr uint32 GroundPassMs = MINUTE * IN_MILLISECONDS;
    float portalX = 0.0f, portalY = 0.0f, portalZ = 0.0f, portalO = 0.0f;
    if (sWorldIndex.FindLocalPortal(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
        PortalRange, portalX, portalY, portalZ, portalO))
    {
        LOG_INFO(ActivityLog, "activity: {} {} at ({:.0f}, {:.0f}, {:.0f}) map {} where no way on the ground leads on: it takes the "
            "teleporter next to it to ({:.0f}, {:.0f}, {:.0f})", player->GetName(), what, player->GetPositionX(),
            player->GetPositionY(), player->GetPositionZ(), player->GetMapId(), portalX, portalY, portalZ);
        sGroundGuard.Forget(player->GetGUID());
        player->TeleportTo(player->GetMapId(), portalX, portalY, portalZ, portalO);
        rt.hasLastPosition = false; // no jump for the planner
        return true;
    }

    // Off the navmesh (Magefrost on a slope above Loch Modan, 2026-11-12): from there the core has only straight
    // lines, and with the ground check off the bot would walk one through the hill, which is how bots end up under
    // the map. It is put back on the ground.
    if (IsOffNavmesh(player) && Rescue(player, rt, Acore::StringFormat("{}, off the walkable ground,", what).c_str(), 0.0f))
        return true;

    // Else nothing: the ground check is not switched off any more (it let the core's straight line through for a
    // minute, the way the bots walked before the guard; the owner: no walk goes blindly straight). The caller ends what it
    // was doing.
    (void)GroundPassMs;
    LOG_INFO(ActivityLog, "activity: {} {} at ({:.0f}, {:.0f}, {:.0f}) map {} where no way on the ground leads on: it gives that up",
        player->GetName(), what, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetMapId());
    return false;
}

void OrderService::LeaveUnreachableFoe(Player* player, BotRuntime& rt)
{
    // Letting go of the target alone did not hold: the creature stayed in the fight, mod-playerbots took it again at once
    // and the ground guard refused its steps, "gives that up" every 2 s for minutes (Shamanheal at the foot of the rock
    // beside Light's Hope, a Noxious Plaguebat 8 yd above it on the rock's top, round 221). The creature is left as a foe
    // that does not die is (TrackFight): 15 minutes, its spawn too, the fight ended and the bot away from it.
    constexpr uint32 AsideMs = 15 * MINUTE * IN_MILLISECONDS;
    // The creature: what it attacks, else what its AI means to attack, else the nearest that attacks it, else the nearest
    // that has it on its threat list. (A Noxious Plaguebat on the rock's top that cannot reach the bot either regenerates
    // in its place and attacks nobody, and a melee bot that cannot get to its target has no victim: with only the first
    // and the third asked the rule found nothing and let go for 40 s, "gives that up" every 2-3 s, Enhancer, round 275.)
    auto isFoe = [player](Unit* unit)
    {
        return unit && unit->IsAlive() && unit->ToCreature() && unit->IsInMap(player) && unit->IsHostileTo(player);
    };
    Unit* foe = player->GetVictim();
    if (!isFoe(foe))
        foe = BotDriver::IntendedTarget(player);
    if (!isFoe(foe))
    {
        foe = nullptr;
        for (Unit* attacker : player->getAttackers())
            if (isFoe(attacker) && (!foe || player->GetExactDist(attacker) < player->GetExactDist(foe)))
                foe = attacker;
    }
    if (!foe)
        for (auto const& [guid, ref] : player->GetThreatMgr().GetThreatenedByMeList())
        {
            Unit* owner = ref ? ref->GetOwner() : nullptr;
            if (isFoe(owner) && (!foe || player->GetExactDist(owner) < player->GetExactDist(foe)))
                foe = owner;
        }
    Creature* creature = foe ? foe->ToCreature() : nullptr;
    if (!creature || player->GetMap()->IsDungeon() || rt.escape.active)
    {
        // (Said once in half a minute, so a case where the rule still does not take hold shows why.)
        static std::unordered_map<ObjectGuid, uint32> noted;
        uint32 const now = getMSTime();
        if (!player->GetMap()->IsDungeon() && (!noted.count(player->GetGUID()) || getMSTimeDiff(noted[player->GetGUID()], now) >= 30 * IN_MILLISECONDS))
        {
            noted[player->GetGUID()] = std::max<uint32>(now, 1);
            LOG_INFO(ActivityLog, "activity: {} cannot get to what it fights and lets go of its target: {}", player->GetName(),
                rt.escape.active ? "it is running" : "it finds no creature that fights it");
        }
        BotDriver::DropTarget(player);
        return;
    }
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    ObjectGuid const guid = creature->GetGUID();
    rt.undyingFoes[guid] = now + AsideMs;
    if (creature->GetSpawnId())
    {
        SpawnPoint spawn;
        spawn.spawnId = creature->GetSpawnId();
        spawn.entry = creature->GetEntry();
        spawn.mapId = uint16(creature->GetMapId());
        creature->GetRespawnPosition(spawn.x, spawn.y, spawn.z);
        rt.Avoid(spawn, AsideMs);
    }
    LOG_INFO(ActivityLog, "activity: {} cannot get to {} (level {}, {:.0f} yd away, {:.0f} yd {}): it ends the fight and leaves that "
        "creature alone for {} min", player->GetName(), creature->GetName(), uint32(creature->GetLevel()), player->GetExactDist(creature),
        std::fabs(creature->GetPositionZ() - player->GetPositionZ()), creature->GetPositionZ() > player->GetPositionZ() ? "above it" : "below it",
        AsideMs / (MINUTE * IN_MILLISECONDS));
    BotDriver::LetGo(player, creature);
    if (Pet* pet = player->GetPet())
        pet->CombatStop(true);
    player->CombatStop(true);
    if (rt.task.kind == TaskKind::Objective || rt.task.kind == TaskKind::Interact || rt.task.kind == TaskKind::Grind)
        ClearTask(rt, 0, "a foe it cannot get to");
    if (!player->IsInCombat())
        Escape::WalkAway(player, rt, creature);
}

bool OrderService::UpdateStuck(Player* player, BotRuntime& rt, float targetX, float targetY)
{
    uint32 const now = getMSTime();
    // Waiting for a lift, or riding one, is no standing still (StepLift gives up by itself).
    if (rt.task.liftAt)
    {
        rt.stuckSince = 0;
        return false;
    }
    float const distance = Dist2d(player, targetX, targetY);
    // What is left to walk: along the route while the task follows one (a route may lead away from its destination for
    // minutes: around a lake, over a pass; "no yard closer in three minutes" called that stuck), else the straight line.
    float left = distance;
    uint32 leftKey = 0;
    if (rt.task.route.found && rt.task.routeIndex < rt.task.route.points.size())
    {
        std::vector<RoutePoint> const& points = rt.task.route.points;
        left = Dist2d(player, points[rt.task.routeIndex].x, points[rt.task.routeIndex].y);
        for (std::size_t i = rt.task.routeIndex; i + 1 < points.size(); ++i)
            left += std::sqrt((points[i + 1].x - points[i].x) * (points[i + 1].x - points[i].x) +
                (points[i + 1].y - points[i].y) * (points[i + 1].y - points[i].y));
        leftKey = uint32(rt.task.route.length) + 1;
    }
    // Along the navmesh's way (a dungeon, or the way up a mesa in the open world: the leader climbing to the Document
    // Chests of "Grimtotem Spying" rode round the mesa for minutes and no nearer as the crow flies, 2026-11-25).
    else if (rt.task.navActive && rt.task.navLeft > 0.0f)
    {
        left = rt.task.navLeft;
        leftKey = 0x40000000u;
    }
    if (!rt.stuckSince)
    {
        rt.stuckX = player->GetPositionX();
        rt.stuckY = player->GetPositionY();
        rt.stuckSince = now;
        rt.approachBest = left;
        rt.approachKey = leftKey;
        rt.approachSince = now;
        return false;
    }
    // The walk found no route and NewRpg's straight walk does not get going either (a target on a mesa, the bot in
    // the canyon below: NewRpg has no path and does not move at all): no five minutes of standing for that.
    if (rt.task.noWay)
    {
        LOG_INFO(ActivityLog, "activity: {} finds no way to its target {:.0f} yd away (task {}): no route, and it has not moved since",
            player->GetName(), distance, ToString(rt.task.kind));
        return true;
    }

    float const dx = player->GetPositionX() - rt.stuckX;
    float const dy = player->GetPositionY() - rt.stuckY;
    if (dx * dx + dy * dy > StuckDistance * StuckDistance)
    {
        rt.stuckX = player->GetPositionX();
        rt.stuckY = player->GetPositionY();
        rt.stuckSince = now;
    }
    if (leftKey != rt.approachKey)
    {
        // Another route, or none any more: another measure.
        rt.approachKey = leftKey;
        rt.approachBest = left;
        rt.approachSince = now;
    }
    else if (left + ApproachDistance < rt.approachBest)
    {
        rt.approachBest = left;
        rt.approachSince = now;
    }
    // (A clock ahead of now is no standing still: it starts again here.)
    if (int32(now - rt.stuckSince) < 0)
        rt.stuckSince = now;
    if (int32(now - rt.approachSince) < 0)
        rt.approachSince = now;
    bool const stuck = now - rt.stuckSince >= StuckTimeMs || now - rt.approachSince >= NoApproachMs;
    // Stuck where the ground guard cancels the movements (a ledge, a roof, a root the navmesh connects to nothing): one
    // way out per task (GroundWayOut).
    constexpr uint32 GroundPassWindowMs = 3 * MINUTE * IN_MILLISECONDS;
    if (stuck)
        LOG_INFO(ActivityLog, "activity: {} is stuck at ({:.0f}, {:.0f}) map {}, {:.0f} yd from its target (task {}): {}{}{}", player->GetName(),
            player->GetPositionX(), player->GetPositionY(), player->GetMapId(), distance, ToString(rt.task.kind),
            DescribeStandstill(player),
            rt.task.ownRounds ? Acore::StringFormat("; the planner's own steps: {} begun, {} without a way{}{}", uint32(rt.task.ownMade),
                uint32(rt.task.ownFailed), rt.task.ownNote.empty() ? std::string() : " (the last: " + rt.task.ownNote + ")",
                rt.task.ownStepAt ? Acore::StringFormat(", the last step to ({:.0f}, {:.0f}, {:.0f}) {} s ago, {:.0f} yd from it, its spline {}",
                    rt.task.ownStepX, rt.task.ownStepY, rt.task.ownStepZ, getMSTimeDiff(rt.task.ownStepAt, now) / IN_MILLISECONDS,
                    player->GetExactDist(rt.task.ownStepX, rt.task.ownStepY, rt.task.ownStepZ),
                    player->movespline->Finalized() ? "ended" : "running") : std::string()) : std::string(),
            rt.task.route.found ? "; on the route " + rt.task.route.via : std::string());
    GroundRefusals const cancelled = stuck && !rt.task.groundPass ?
        sGroundGuard.GetRefusals(player->GetGUID(), GroundPassWindowMs, false) : GroundRefusals();
    if (cancelled.lastAt && getMSTimeDiff(cancelled.lastAt, now) <= GroundPassWindowMs)
    {
        rt.task.groundPass = true;
        if (GroundWayOut(player, rt, "is stuck"))
        {
            rt.stuckSince = 0;
            return false;
        }
        return true; // the stuck rule ends the task
    }
    return stuck;
}

void OrderService::OnTravelStuck(GuildBot const& bot, BotRuntime& rt, float distance)
{
    // The walk of a quest partner to its leader (QuestPartners.h): the meeting is given up, nothing else.
    if (rt.task.partnerMeet)
    {
        rt.squad.meetFailed = true;
        ClearTask(rt, 5 * IN_MILLISECONDS, rt.squad.rejoining ? "stuck on the way back to its squad leader" : "stuck on the way to its quest partner");
        return;
    }
    // On a journey to a task's spot: the spot waits, the target zone has nothing to do with it.
    if (rt.task.journey)
    {
        if (rt.task.spot.spawnId)
            rt.Avoid(rt.task.spot, AvoidSpawnMs);
        else
            rt.AddAvoidArea(rt.task.spot.mapId, rt.task.spot.x, rt.task.spot.y, 255, AvoidSpawnMs, false);
        ClearTask(rt, 5 * IN_MILLISECONDS, "stuck on its journey");
        return;
    }
    // On an errand (its travel, or the walk to a dock for it): the errand is given up after a few attempts, the target
    // zone has nothing to do with it.
    if (rt.errandQuest && (rt.task.errand || rt.task.kind == TaskKind::Voyage))
    {
        if (++rt.errandStuck >= ErrandStuckLimit || rt.task.farAround)
        {
            DropErrand(bot, rt, "stuck or killed on the way");
            ClearTask(rt, 5 * IN_MILLISECONDS, "stuck, errand given up");
            return;
        }
        LOG_INFO("module", "mod-guild-adventure: {} is stuck on the way to {} ({:.0f} yd left, errand attempt {})", bot.name,
            rt.task.targetName, distance, uint32(rt.errandStuck));
        rt.why = Acore::StringFormat("Stuck on the way to {}; trying again shortly", rt.task.targetName);
        ClearTask(rt, 30 * IN_MILLISECONDS, "stuck");
        return;
    }

    uint32 const zone = rt.targetZone;
    if (zone && zone == rt.travelStuckZone)
        ++rt.travelStuckCount;
    else
    {
        rt.travelStuckZone = zone;
        rt.travelStuckCount = 1;
    }
    // (No way worth walking, WalkTo: another try finds the same.)
    if (zone && rt.task.farAround)
        rt.travelStuckCount = TravelStuckLimit;

    if (zone && rt.travelStuckCount >= TravelStuckLimit)
    {
        rt.unreachableZones[zone] = getMSTime() + UnreachableZoneMs;
        rt.travelStuckZone = 0;
        rt.travelStuckCount = 0;
        rt.zoneChosenAt = 0;
        if (rt.task.farAround)
            LOG_INFO("module", "mod-guild-adventure: {} has no way worth walking to {} ({:.0f} yd left); the zone is set aside for "
                "{} min", bot.name, ZoneName(zone), distance, UnreachableZoneMs / (MINUTE * IN_MILLISECONDS));
        else
        LOG_INFO("module", "mod-guild-adventure: {} got stuck {} times on the way to {} ({:.0f} yd left); the zone is set aside "
            "for {} min", bot.name, uint32(TravelStuckLimit), ZoneName(zone), distance, UnreachableZoneMs / (MINUTE * IN_MILLISECONDS));
        rt.why = Acore::StringFormat("Cannot find a way to {}; trying again later", ZoneName(zone));
        ClearTask(rt, 5 * IN_MILLISECONDS, "stuck, zone set aside");
        return;
    }

    LOG_INFO("module", "mod-guild-adventure: {} is stuck on the way to {} ({:.0f} yd left, attempt {})", bot.name,
        rt.task.targetName, distance, uint32(rt.travelStuckCount));
    rt.why = Acore::StringFormat("Stuck on the way to {}; trying again shortly", rt.task.targetName);
    ClearTask(rt, 30 * IN_MILLISECONDS, "stuck");
}

bool OrderService::StraightWayIsSafe(Player* player, BotRuntime& rt, float x, float y, float z, bool beforeOnly)
{
    int32 const myLevel = int32(player->GetLevel());
    int32 const margin = int32(sBotSettings.Get(player, Setting::WalksMaxLevelsAbove));
    if (int32(sRoadMap.DangerAlong(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), x, y, player->GetTeamId(),
        beforeOnly ? RoadMap::EndStretch : 0.0f, player->GetPositionZ(), z)) - myLevel > margin)
        return false;
    uint8 crowdLevel = 0;
    uint8 const crowd = sRoadMap.CrowdAlong(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), x, y,
        player->GetTeamId(), crowdLevel, player->GetPositionZ(), z, RoadMap::CrowdStartStretch);
    if (RoadMap::GroupCounts(crowd, crowdLevel, myLevel) && !Risk::CanFight(player, rt, crowdLevel, crowd))
        return false;
    // Not through a place it has set aside (unless it stands in one: it has to get out).
    if (!rt.avoidAreas.empty() && !rt.IsInAvoidedArea(player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
        uint8(myLevel)))
    {
        float const length = Dist2d(player, x, y);
        for (float along = 20.0f; along < length; along += 20.0f)
        {
            float const share = along / length;
            if (rt.IsInAvoidedArea(player->GetMapId(), player->GetPositionX() + (x - player->GetPositionX()) * share,
                player->GetPositionY() + (y - player->GetPositionY()) * share, uint8(myLevel)))
                return false;
        }
    }
    return StandsAtCamp(player, rt) || !Risk::EntersCamp(player, rt, player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
        x, y, nullptr, nullptr, 10.0f, 45.0f);
}

bool OrderService::LeaveDangerousWalk(GuildBot const& bot, Player* player, BotRuntime& rt, int32 margin, bool beforeOnly)
{
    BotTask& task = rt.task;
    if (task.dangerChecked || task.nearSince || !sBotSettings.IsOn(player, Setting::RoadsEnable) || task.spot.mapId != player->GetMapId())
        return false;
    float const distance = Dist2d(player, task.spot);
    if (distance <= NearDistance)
        return false;

    uint8 danger = 0;
    uint8 dangerWhole = 0;
    if (task.route.found)
    {
        danger = beforeOnly ? task.route.dangerBefore : task.route.danger;
        dangerWhole = task.route.danger;
    }
    else if (task.routePlanned || distance <= sBotSettings.GetFloat(player, Setting::RoadsMinDistance))
    {
        danger = sRoadMap.DangerAlong(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), task.spot.x, task.spot.y,
            player->GetTeamId(), beforeOnly ? RoadMap::EndStretch : 0.0f, player->GetPositionZ(), task.spot.z);
        dangerWhole = beforeOnly ? sRoadMap.DangerAlong(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), task.spot.x,
            task.spot.y, player->GetTeamId(), 0.0f, player->GetPositionZ(), task.spot.z) : danger;
    }
    else
        return false; // its route is planned on one of the next ticks
    task.dangerChecked = true;

    // A route the gate refuses, while the path finder has a straight way the gate lets through: that way. The road map
    // joins its places by its roads, and from a valley it may lead out only past what the bot must not pass: Roguecombat
    // (60) entered the world in The Crystal Vale of Silithus after a restart, every route out began over the node of
    // Hive'Ashi (an elite on it, level 68 on the danger map) or through the Twilight camp, eight walks were refused in a
    // minute and it went home by hearthstone, while the straight line to Cenarion Hold (1267 yd) passes nothing above
    // level 59, round 228. (Asked once a task; the objectives' rule for a way far around asks the same.)
    auto straightInstead = [&](std::string const& refused)
    {
        constexpr float StraightMaxDetour = 1.6f;
        if (!task.route.found || task.straightTried)
            return false;
        task.straightTried = true;
        float chain = 0.0f;
        if (!ChainReaches(player, task.spot.x, task.spot.y, task.spot.z, chain) || chain > task.route.direct * StraightMaxDetour + 100.0f ||
            !StraightWayIsSafe(player, rt, task.spot.x, task.spot.y, task.spot.z, beforeOnly))
            return false;
        LOG_INFO(ActivityLog, "activity: {} takes the straight way to {} ({}): the road map's way ({}) {}; the path finder has one of "
            "{:.0f} yd ({:.0f} yd straight) past nothing it may not pass", bot.name, task.targetName, ToString(task.kind), task.route.via,
            refused, chain, task.route.direct);
        task.route.found = false;
        task.routeBlocked = true; // the rest of the task walks without the road map
        task.walkX = task.walkY = 0.0f;
        return true;
    };

    int32 const level = player->GetLevel();
    // The last stretch before a target (beforeOnly) is the spot's own matter: its target, which the risk model judged.
    // Not creatures far above the bot beside it: they ring the place, and from there every way out passes them. (The
    // guild leader, level 51, walked to a Redstone Crystalhide 40 yd from the Dreadmaul ogres, levels 58 to 60, of the
    // Blasted Lands; from there all its ways were refused for them, and it used its hearthstone.) What stands right at
    // the spot (12 yd) is the target and its company.
    if (beforeOnly && dangerWhole > danger && int32(dangerWhole) - level > margin && dangerWhole < RoadMap::TownDanger)
    {
        constexpr float SpotOwnYards = 12.0f;
        uint8 const atSpot = sRoadMap.StrongestNear(task.spot.mapId, task.spot.x, task.spot.y, task.spot.z, player->GetTeamId(),
            SpotOwnYards);
        if (int32(dangerWhole) > int32(atSpot) + GetSettings().roadsPairLevels)
            danger = dangerWhole;
    }
    // Through a place the bot has set aside (it died or lost its fights there): the route goes around such places when
    // it can, and when it cannot the target waits (a level-24 rogue's hand-in route led through the camp it had died
    // at five times, and it died there again). Not while the bot stands in such a place itself: it has to get out.
    if (task.route.found && !rt.avoidAreas.empty() &&
        !rt.IsInAvoidedArea(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), uint8(level)))
    {
        bool through = false;
        for (std::size_t i = 0; i < task.route.points.size() && !through; i += 3)
            through = rt.IsInAvoidedArea(task.route.mapId, task.route.points[i].x, task.route.points[i].y, uint8(level));
        // (The destination itself in such a place is the planner's matter, not the way's.)
        if (through && !rt.IsInAvoidedArea(task.spot.mapId, task.spot.x, task.spot.y, uint8(level)) &&
            straightInstead("leads through a place it has set aside"))
            return false;
        if (through && !rt.IsInAvoidedArea(task.spot.mapId, task.spot.x, task.spot.y, uint8(level)))
        {
            LOG_INFO(ActivityLog, "activity: {} does not walk to {} ({}{}): the way ({}) leads through a place it has set aside (it "
                "died or lost its fights there); left alone for {} min", bot.name, task.targetName, ToString(task.kind),
                task.questId ? Acore::StringFormat(", quest {} \"{}\"", task.questId, QuestTitle(task.questId)) : std::string(),
                task.route.via, GetSettings().walksPostponeMinutes);
            std::string const why = Acore::StringFormat("The way to {} leads through a place it keeps away from: later", task.targetName);
            rt.Avoid(task.spot, GetSettings().walksPostponeMinutes * MINUTE * IN_MILLISECONDS);
            rt.gateRecheck = true;
            BotDriver::Rest(player);
            ClearTask(rt, IN_MILLISECONDS, "a place set aside on the way");
            rt.why = why;
            return true;
        }
    }
    // A group standing right on the way comes at once, whatever its level: the risk model says whether this bot can take
    // that many (a level-21 mage died to four level-10 gnolls on a route through their camp, a level-18 hunter to five
    // level-14 murlocs). Not past them; the target waits.
    {
        uint8 crowdLevel = 0;
        uint8 crowd = task.route.found ? task.route.crowd : sRoadMap.CrowdAlong(player->GetMapId(), player->GetPositionX(),
            player->GetPositionY(), task.spot.x, task.spot.y, player->GetTeamId(), crowdLevel, player->GetPositionZ(), task.spot.z,
            RoadMap::CrowdStartStretch);
        if (task.route.found)
            crowdLevel = task.route.crowdLevel;
        bool refused = RoadMap::GroupCounts(crowd, crowdLevel, level) &&
            !Risk::CanFight(player, rt, crowdLevel, crowd);
        // The straight walk of a task without a route (under the roads' distance) into the middle of a camp the bot
        // cannot take (Risk::EntersCamp): the same. Not out of such a camp (the way out is no reason to stay), and not
        // the last stretch before the spot, which whoever chose the spot judged.
        bool camp = task.route.found && task.route.camp;
        uint8 campCount = crowd, campLevel = crowdLevel;
        if (!refused && !task.route.found)
        {
            constexpr float CampStopBefore = 45.0f;
            Risk::CampAt at;
            if (!StandsAtCamp(player, rt) &&
                Risk::EntersCamp(player, rt, player->GetMapId(), player->GetPositionX(), player->GetPositionY(), task.spot.x, task.spot.y,
                    &at, nullptr, 10.0f, CampStopBefore))
            {
                refused = camp = true;
                campCount = uint8(std::min<uint16>(at.count, 255));
                campLevel = at.level;
            }
        }
        if (refused && straightInstead(Acore::StringFormat("passes {} level-{} creatures that {}", uint32(camp ? campCount : crowd),
            uint32(camp ? campLevel : crowdLevel), camp ? "have their camp there" : "stand together")))
            return false;
        if (refused)
        {
            char const* noFlight = nullptr;
            if (StartJourney(bot, player, rt, &noFlight))
                return true;
            LOG_INFO(ActivityLog, "activity: {} does not walk to {} ({}{}): {} level-{} creatures {} on the way{} (it is "
                "level {}); left alone for {} min{}", bot.name, task.targetName, ToString(task.kind),
                task.questId ? Acore::StringFormat(", quest {} \"{}\"", task.questId, QuestTitle(task.questId)) : std::string(),
                uint32(camp ? campCount : crowd), uint32(camp ? campLevel : crowdLevel),
                camp ? "have their camp" : "stand together", task.route.found ? " (" + task.route.via + ")" : "", level,
                GetSettings().walksPostponeMinutes, noFlight ? std::string("; no flight: ") + noFlight : std::string());
            if (camp)
            {
                crowd = campCount;
                crowdLevel = campLevel;
            }
            std::string const why = Acore::StringFormat("{} is behind a group of {} level-{} creatures: later", task.targetName,
                uint32(crowd), uint32(crowdLevel));
            rt.Avoid(task.spot, GetSettings().walksPostponeMinutes * MINUTE * IN_MILLISECONDS);
            rt.gateRecheck = true;
            BotDriver::Rest(player);
            ClearTask(rt, IN_MILLISECONDS, "a group on the way");
            rt.why = why;
            return true;
        }
    }
    // Where the walk leads: a quest giver or an ender among creatures that all start above the bot (the innkeeper of
    // Goldshire for a level-3 character with quests left in Northshire: around the village nothing is below level 5)
    // is for later. The bot would stand there with nothing it can fight.
    // (Not for a vendor or a trainer: the bot buys and leaves. The innkeeper of Menethil Harbor was refused to a
    // level-25 shaman for the level-28 murlocs on the shore 90 yd from the inn.)
    if (!beforeOnly && task.kind != TaskKind::Vendor && task.kind != TaskKind::Train &&
        sBotSettings.IsOn(player, Setting::WalksPlaceCheck) && distance > PlaceCheckDistance)
    {
        uint32 around = 0, aroundHere = 0;
        uint8 const weakest = sRoadMap.WeakestAround(task.spot.mapId, task.spot.x, task.spot.y, PlaceRadius, player->GetTeamId(), around);
        uint8 const weakestHere = sRoadMap.WeakestAround(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), PlaceRadius,
            player->GetTeamId(), aroundHere);
        bool const hereToo = aroundHere >= PlaceMinCreatures && int32(weakestHere) - level > margin;
        if (around >= PlaceMinCreatures && int32(weakest) - level > margin && !hereToo)
        {
            LOG_INFO(ActivityLog, "activity: {} does not walk to {} ({}{}): the creatures around it start at level {} (it is level {}); "
                "left alone for {} min", bot.name, task.targetName, ToString(task.kind),
                task.questId ? Acore::StringFormat(", quest {} \"{}\"", task.questId, QuestTitle(task.questId)) : std::string(),
                uint32(weakest), level, uint32(sBotSettings.Get(player, Setting::WalksPostponeMinutes)));
            std::string const why = Acore::StringFormat("{} is among creatures from level {} up: later, it is level {}", task.targetName,
                uint32(weakest), level);
            rt.Avoid(task.spot, uint32(sBotSettings.Get(player, Setting::WalksPostponeMinutes)) * MINUTE * IN_MILLISECONDS);
            rt.gateRecheck = true;
            BotDriver::Rest(player);
            ClearTask(rt, IN_MILLISECONDS, "place above its level");
            rt.why = why;
            return true;
        }
    }
    if (int32(danger) - level <= margin)
        return false;
    // Among such creatures already: a way that is no worse than where the bot stands is right (it has to get out). Not
    // any way: a level-31 bot among the level-33 creatures of Desolace walked past the level-50 guards of a Horde post.
    uint8 const here = sRoadMap.DangerAlong(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionX(),
        player->GetPositionY(), player->GetTeamId());
    if (int32(here) - level > margin && danger <= here)
        return false;
    if (straightInstead(Acore::StringFormat("passes {}", RoadMap::DangerName(danger))))
        return false;

    char const* noFlight = nullptr;
    if (StartJourney(bot, player, rt, &noFlight))
        return true;
    LOG_INFO(ActivityLog, "activity: {} does not walk to {} ({}{}): the way{} passes {} (it is level {}{}); left alone for {} "
        "min{}", bot.name, task.targetName, ToString(task.kind),
        task.questId ? Acore::StringFormat(", quest {} \"{}\"", task.questId, QuestTitle(task.questId)) : std::string(),
        task.route.found ? " (" + task.route.via + ")" : "", RoadMap::DangerName(danger), level,
        GetSettings().roadsPairLevels ? Acore::StringFormat("; a pair counts {} levels higher", GetSettings().roadsPairLevels) :
            std::string(),
        uint32(sBotSettings.Get(player, Setting::WalksPostponeMinutes)),
        noFlight ? std::string("; no flight: ") + noFlight : std::string());
    std::string const why = Acore::StringFormat("The way to {} is too dangerous at level {} ({} on it)", task.targetName,
        level, RoadMap::DangerName(danger));
    rt.Avoid(task.spot, uint32(sBotSettings.Get(player, Setting::WalksPostponeMinutes)) * MINUTE * IN_MILLISECONDS);
    rt.gateRecheck = true;
    BotDriver::Rest(player);
    ClearTask(rt, IN_MILLISECONDS, "way too dangerous");
    rt.why = why;
    return true;
}

// The sanctum of Stormwind's wizard tower lies under the tower, at height 30, and is entered through the portal at the
// tower's top, at height 152. A mage's class quests are handed in there: a bot walked to the spot above Jennea Cannon,
// stood "at" her by its distance on the map, found no way down and gave up, every half hour, each time after an
// errand's journey to Stormwind (one mage of the guild, six times in a day).
bool OrderService::ThroughLocalPortal(Player* player, BotRuntime& rt)
{
    constexpr float RoomYards = 60.0f;   // the NPC this near to where the teleporter puts the bot
    constexpr float RoomHeight = 12.0f;  // and at about that height
    constexpr uint32 PortalWalkMs = 4 * MINUTE * IN_MILLISECONDS;
    BotTask& task = rt.task;
    if (!task.hasSpot || task.spot.mapId != player->GetMapId() || !player->IsAlive() || player->IsInCombat())
        return false;
    if (!task.portalLooked)
    {
        task.portalLooked = true;
        task.viaPortal = sWorldIndex.FindLocalPortalInto(task.spot.mapId, task.spot.x, task.spot.y, task.spot.z, RoomYards, RoomHeight,
            task.portal);
    }
    if (!task.viaPortal)
        return false;
    LocalPortal const& portal = task.portal;
    // In the room: the visit goes on as any other.
    if (Dist2d(player, portal.targetX, portal.targetY) <= RoomYards && std::fabs(player->GetPositionZ() - portal.targetZ) <= RoomHeight)
        return false;

    uint32 const now = getMSTime();
    if (!task.portalSince)
    {
        task.portalSince = now ? now : 1;
        rt.stuckSince = 0; // the stuck tracker watches the way to the teleporter now
        LOG_INFO(ActivityLog, "activity: {} goes to the teleporter at ({:.0f}, {:.0f}, {:.0f}) first: {} stands in a room only it leads "
            "into", player->GetName(), portal.x, portal.y, portal.z, task.targetName);
    }
    float const dx = player->GetPositionX() - portal.x, dy = player->GetPositionY() - portal.y, dz = player->GetPositionZ() - portal.z;
    if (dx * dx + dy * dy + dz * dz <= portal.radius * portal.radius)
    {
        LOG_INFO(ActivityLog, "activity: {} steps into the teleporter at ({:.0f}, {:.0f}, {:.0f}) and stands at ({:.0f}, {:.0f}, {:.0f}), "
            "{:.0f} yd from {}", player->GetName(), portal.x, portal.y, portal.z, portal.targetX, portal.targetY, portal.targetZ,
            std::sqrt((portal.targetX - task.spot.x) * (portal.targetX - task.spot.x) + (portal.targetY - task.spot.y) * (portal.targetY - task.spot.y)),
            task.targetName);
        sGroundGuard.Forget(player->GetGUID());
        BotDriver::Rest(player);
        player->TeleportTo(player->GetMapId(), portal.targetX, portal.targetY, portal.targetZ, portal.targetO);
        rt.hasLastPosition = false; // no jump for the planner
        rt.stuckSince = 0;
        task.walkX = task.walkY = 0.0f;
        task.routePlanned = false;
        task.route.found = false;
        task.nearSince = 0;
        return true;
    }
    // The way to the teleporter did not work out: the visit as before, which gives the NPC up.
    if (now - task.portalSince > PortalWalkMs || UpdateStuck(player, rt, portal.x, portal.y))
    {
        LOG_INFO(ActivityLog, "activity: {} does not get to the teleporter at ({:.0f}, {:.0f}, {:.0f}) for {} ({:.0f} yd from it after {} s)",
            player->GetName(), portal.x, portal.y, portal.z, task.targetName, std::sqrt(dx * dx + dy * dy + dz * dz),
            (now - task.portalSince) / IN_MILLISECONDS);
        task.viaPortal = false;
        rt.stuckSince = 0;
        return false;
    }
    WalkTo(player, rt, player->GetMapId(), portal.x, portal.y, portal.z);
    rt.why = Acore::StringFormat("On the way to {}: to the teleporter that leads there", task.targetName);
    return true;
}

// The way back: a bot that handed its quest in in the sanctum had no way on the ground to its next task and was put
// back on a road by the rescue, 19 s later and 200 yd away. A walk that begins in a room a teleporter leads into, to
// a place outside it, with no road in reach of the path finder, leads to the teleporter out first.
// The road node a target on another level is reached from (SetAsideOtherLevel): the walk goes there first, then on.
bool OrderService::ViaRoadNode(Player* player, BotRuntime& rt, float& x, float& y, float& z)
{
    BotTask& task = rt.task;
    if (!task.viaActive)
        return false;
    if (Dist2d(player, task.viaX, task.viaY) <= 6.0f && std::fabs(player->GetPositionZ() - task.viaZ) <= 5.0f)
    {
        LOG_INFO(ActivityLog, "activity: {} is at {}: on to {}", player->GetName(), task.viaName,
            task.targetName.empty() ? std::string("its target") : task.targetName);
        task.viaActive = false;
        task.ownSteps = false;
        task.stallSince = 0;
        task.routePlanned = false;
        task.route.found = false;
        task.walkX = task.walkY = 0.0f;
        rt.stuckSince = 0;
        return false;
    }
    x = task.viaX;
    y = task.viaY;
    z = task.viaZ;
    return true;
}

bool OrderService::LeaveLocalRoom(Player* player, BotRuntime& rt, float& x, float& y, float& z)
{
    constexpr float RoomYards = 60.0f;
    constexpr float RoomHeight = 12.0f;
    constexpr float ExitYards = 100.0f;
    // Not again right after one: where the teleporter out puts the bot (the top of the tower) is "where a teleporter
    // leads to" as well.
    constexpr uint32 AgainMs = 3 * MINUTE * IN_MILLISECONDS;
    BotTask& task = rt.task;
    uint32 const now = getMSTime();
    if (!task.roomLooked)
    {
        task.roomLooked = true;
        LocalPortal in;
        if (!player->IsInCombat() && player->IsAlive() && (!rt.roomExitAt || getMSTimeDiff(rt.roomExitAt, now) > AgainMs) &&
            sWorldIndex.FindLocalPortalInto(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                RoomYards, RoomHeight, in) &&
            (std::sqrt((x - in.targetX) * (x - in.targetX) + (y - in.targetY) * (y - in.targetY)) > RoomYards ||
                std::fabs(z - in.targetZ) > RoomHeight) &&
            sWorldIndex.FindLocalPortalFrom(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                ExitYards, task.roomExit) &&
            IsCutOff(player))
        {
            task.leavesRoom = true;
            LOG_INFO(ActivityLog, "activity: {} goes to the teleporter at ({:.0f}, {:.0f}, {:.0f}) first: the room it stands in has no "
                "other way out", player->GetName(), task.roomExit.x, task.roomExit.y, task.roomExit.z);
        }
    }
    if (!task.leavesRoom)
        return false;
    LocalPortal const& exit = task.roomExit;
    float const dx = player->GetPositionX() - exit.x, dy = player->GetPositionY() - exit.y, dz = player->GetPositionZ() - exit.z;
    if (dx * dx + dy * dy + dz * dz <= exit.radius * exit.radius)
    {
        LOG_INFO(ActivityLog, "activity: {} steps into the teleporter at ({:.0f}, {:.0f}, {:.0f}) and stands at ({:.0f}, {:.0f}, {:.0f})",
            player->GetName(), exit.x, exit.y, exit.z, exit.targetX, exit.targetY, exit.targetZ);
        task.leavesRoom = false;
        rt.roomExitAt = now ? now : 1;
        sGroundGuard.Forget(player->GetGUID());
        BotDriver::Rest(player);
        player->TeleportTo(player->GetMapId(), exit.targetX, exit.targetY, exit.targetZ, exit.targetO);
        rt.hasLastPosition = false; // no jump for the planner
        rt.stuckSince = 0;
        task.walkX = task.walkY = 0.0f;
        task.routePlanned = false;
        task.route.found = false;
        return true;
    }
    x = exit.x;
    y = exit.y;
    z = exit.z;
    return false;
}

bool OrderService::StepNpcTask(Player* player, BotRuntime& rt, bool& done)
{
    uint32 const now = getMSTime();
    BotTask& task = rt.task;
    done = false;

    if (now - task.startedAt > NpcTaskMaxMs)
    {
        task.unreachable = !task.nearSince;
        return false;
    }
    if (ThroughLocalPortal(player, rt))
        return true;

    float const distance = Dist2d(player, task.spot);
    // (On the navmesh's way to another level (SetAsideOtherLevel) the walk goes on however near the NPC is on the map: a
    // warrior in Sarkoth's cave 25 yd above the vendor Huklah, 29 yd away, "walks that way" (456 yd) and stood 30 s,
    // NewRpg's straight visit refused, until the vendor was set aside; a player's log, 2026-11-28.)
    bool const openWalk = task.openWay && player->GetExactDist(task.spot.x, task.spot.y, task.spot.z) > 8.0f;
    if ((distance > NearDistance || openWalk) && !task.nearSince)
    {
        if (UpdateStuck(player, rt, task.spot.x, task.spot.y))
        {
            task.unreachable = true;
            return false;
        }
        WalkTo(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z);
        return true;
    }

    if (!task.nearSince)
        task.nearSince = now;
    if (now - task.nearSince > NpcTimeoutMs)
    {
        // Never in sight (dead, respawning, on patrol elsewhere): like an NPC the bot cannot get to, no failure.
        task.unreachable = !task.seenTarget;
        return false;
    }

    WorldObject* target = FindInSight(player, task.spot);
    if (GetSettings().debug)
        LOG_INFO("module", "mod-guild-adventure: {} {} near {}: in sight {}, distance {:.1f}, reached {}, rpg {}", player->GetName(),
            ToString(task.kind), task.targetName, target ? "yes" : "no", target ? player->GetDistance(target) : -1.0f,
            target && BotDriver::HasReachedVisitTarget(player, target->GetGUID()) ? "yes" : "no", ToString(BotDriver::GetRpgState(player)));
    if (!target)
        return true; // not in sight yet (respawning, around a corner): wait for the timeout
    task.seenTarget = true;

    if (task.kind == TaskKind::Train)
    {
        // Trainers often stand behind a counter the bot cannot step around; a few yards more than the client's
        // interaction distance are allowed.
        if (BotDriver::HasReachedVisitTarget(player, target->GetGUID()) || player->GetDistance(target) <= TeachDistance)
        {
            Creature* npc = target->ToCreature();
            Trainer::Trainer* trainer = npc ? sObjectMgr->GetTrainer(npc->GetEntry()) : nullptr;
            uint32 learned = 0;
            if (trainer)
                for (Trainer::Spell const* spell : AffordableSpells(trainer, player))
                {
                    trainer->TeachSpell(npc, player, spell->SpellId);
                    ++learned;
                }
            LOG_INFO("module", "mod-guild-adventure: {} learned {} spells at {}", player->GetName(), learned, task.targetName);
            done = true;
            return false;
        }
    }

    if (task.kind == TaskKind::Discover)
    {
        if (BotDriver::HasReachedVisitTarget(player, target->GetGUID()) || player->GetDistance(target) <= TeachDistance)
        {
            if (Creature* npc = target->ToCreature())
                player->GetSession()->SendLearnNewTaxiNode(npc);
            LOG_INFO("module", "mod-guild-adventure: {} learned the flight path at {}", player->GetName(), task.targetName);
            done = true;
            return false;
        }
    }

    // An NPC NewRpg does not get to within a quarter of a minute (a smith inside his house: NewRpg's own approach goes
    // for a spot around the NPC, which may lie behind a wall): the planner walks the bot to where the NPC stands, and
    // NewRpg's visit takes over from there.
    // The same for an NPC on another floor (the cellar of an inn, a shop's upper room): right above or below it the
    // bot is "at" the NPC by its distance and never gets to it (a warlock stood over Gakin the Darkbinder for 110 s,
    // "distance 0.0, reached no"). The walk down the stairs gets its time before the task gives up.
    constexpr uint32 DirectApproachMs = 15 * IN_MILLISECONDS;
    constexpr float DirectApproachDistance = 4.0f;
    constexpr float FloorHeight = 2.5f;
    constexpr uint32 FloorApproachMs = 90 * IN_MILLISECONDS;
    bool const otherFloor = std::fabs(player->GetPositionZ() - target->GetPositionZ()) > FloorHeight;
    bool const apart = player->GetDistance(target) > DirectApproachDistance;
    if (!BotDriver::HasReachedVisitTarget(player, target->GetGUID()) && (apart || otherFloor) && now - task.nearSince > DirectApproachMs)
    {
        if (!task.directSince)
            task.directSince = now;
        // (Another floor only for its time: after that NewRpg's own visit gets its turn again, as before.)
        bool const stairs = otherFloor && now - task.directSince < FloorApproachMs;
        if (stairs)
            task.nearSince = now - DirectApproachMs - IN_MILLISECONDS; // the task's own clock waits for the stairs
        if (apart || stairs)
        {
            // (With the planner's own steps once the task went over to them: they know the crossings, BridgeGap.)
            if (task.ownSteps && OwnStep(player, task, player->GetMapId(), target->GetPositionX(), target->GetPositionY(),
                target->GetPositionZ()))
                return true;
            if (now - task.lastAssert >= AssertGapMs)
            {
                BotDriver::GoTo(player, player->GetMapId(), target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), false);
                task.lastAssert = now;
            }
            return true;
        }
    }

    // Visit the NPC: NewRpg walks there and talks to it (quests, selling, repairing, buying). After a while at an NPC
    // NewRpg picks the next one by itself and stays in the same status: the visit is set again when it is not this
    // NPC's any more (hand-ins "failed" with the NPC 1.5 yd away while NewRpg was busy with its neighbour).
    RpgState const state = BotDriver::GetRpgState(player);
    if ((state != RpgState::WanderNpc || !BotDriver::IsVisiting(player, target->GetGUID())) && now - task.lastAssert >= AssertGapMs)
    {
        BotDriver::Visit(player, target->GetGUID());
        task.lastAssert = now;
    }

    // NewRpg sells, buys and repairs the moment it reaches the vendor; it buys only when it also sold something, so the
    // module asks for the purchase itself.
    bool const visited = BotDriver::HasReachedVisitTarget(player, target->GetGUID());
    bool const selfServe = !visited && player->GetDistance(target) <= INTERACTION_DISTANCE && !otherFloor &&
        now - task.nearSince >= VendorSelfServeMs;
    // The core sells only within its interaction distance (Player::GetNPCIfCanInteractWith, 5 yd), and NewRpg counts
    // the vendor as reached farther off: a quest item was "bought 0 of 1" twice (Flagon of Mead, "The Third Fleet", at
    // Innkeeper Helbrek and at Edris Barleybeard), and the bot set out for the next vendor of it. It steps in first, ten
    // seconds at most.
    constexpr uint32 BuyStepInMs = 10 * IN_MILLISECONDS;
    if (task.kind == TaskKind::Vendor && (visited || selfServe) && task.buyItem && target->ToCreature() &&
        !player->IsWithinDistInMap(target, INTERACTION_DISTANCE - 1.0f))
    {
        if (!task.buyStepInSince)
            task.buyStepInSince = now ? now : 1;
        if (now - task.buyStepInSince < BuyStepInMs)
        {
            BotDriver::PauseAI(player, 2 * IN_MILLISECONDS);
            if (!player->isMoving())
            {
                float x, y, z;
                target->GetNearPoint(player, x, y, z, player->GetCombatReach(), 1.5f, target->GetAngle(player));
                player->GetMotionMaster()->MovePoint(0, x, y, z);
            }
            return true;
        }
    }
    if (task.kind == TaskKind::Vendor && (visited || selfServe))
    {
        // Gear it has worn and will not wear again goes (LootService::HandleWornGear, WG1): an enchanter disenchants it,
        // anyone else sells it here. mod-playerbots' own sale skips such pieces for enchanters and for what it rates
        // "bad equip" (an armour kind its class should not wear any more).
        sLootService.HandleWornGear(player, target->ToCreature());
        // A trip for a repair repairs, with the bot's own money, as far as it goes (piece by piece). NewRpg repairs only
        // within what mod-playerbots' budget keeps free for it ("can repair"): a mage at 1 % durability and 9 gold went
        // to the armorer, was not repaired, and its next trip waited a quarter of an hour.
        if (Creature* npc = task.repair ? target->ToCreature() : nullptr; npc && npc->IsArmorer())
        {
            uint32 const before = MinDurabilityPercent(player);
            uint32 const money = player->GetMoney();
            player->DurabilityRepairAll(true, player->GetReputationPriceDiscount(npc), false);
            if (player->GetMoney() < money || MinDurabilityPercent(player) != before)
                LOG_INFO(ActivityLog, "activity: {} repaired its gear at {} for {}c: durability {}% -> {}%", player->GetName(),
                    task.targetName, money - player->GetMoney(), before, MinDurabilityPercent(player));
        }
        // playerbots' own buy comes first (ammo, reagents, and one kind of food or drink); the module fills up the rest.
        uint32 const foodBefore = CountConsumables(player, FoodSpellCategory);
        uint32 const drinkBefore = CountConsumables(player, DrinkSpellCategory);
        // A hunter's ammunition first, up to its stock (Ammunition.h; playerbots' purchase below is held to the same
        // stock by the vendor hook, as is the one stack of any other class with a bow or gun).
        if (std::string const ammo = Ammo::BuyAt(player, target->ToCreature()); !ammo.empty())
            LOG_INFO(ActivityLog, "activity: {} bought {} from {}", player->GetName(), ammo, task.targetName);
        // A rogue's poisons up to its stock, when this vendor sells them (RoguePoisons.h). The visit ends a trip for
        // them and starts the pause before the next one, whatever it could buy.
        if (std::string const poisons = Poisons::BuyAt(player, target->ToCreature()); !poisons.empty())
            LOG_INFO(ActivityLog, "activity: {} bought poisons for its weapons from {}: {}", player->GetName(), task.targetName, poisons);
        if (Poisons::Uses(player) && Poisons::VendorSells(target->GetEntry(), player))
        {
            rt.poisonTripSince = 0;
            rt.nextPoisonTopUp = getMSTime() + PoisonTopUpGapMs;
        }
        // Recipes of its professions this vendor sells and that it can learn now (Professions::BuyRecipesAt), before
        // mod-playerbots' own purchase: that buys recipes too, with the bot's own money, and leaves them in the bags.
        if (std::string const recipes = Professions::BuyRecipesAt(player, target->ToCreature()); !recipes.empty())
            LOG_INFO(ActivityLog, "activity: {} bought recipes from {} and learned them: {}", player->GetName(), task.targetName, recipes);
        // (Below the provisions level only a hunter has something to buy: its ammunition.)
        if (player->GetLevel() >= uint32(sBotSettings.Get(player, Setting::ProvisionsMinLevel)) || player->getClass() == CLASS_HUNTER)
            BotDriver::BuyFromVendor(player);
        // What it carries in recipes it can learn (bought by mod-playerbots just now or earlier) is learned.
        if (std::string const carried = Professions::LearnCarriedRecipes(player); !carried.empty())
            LOG_INFO(ActivityLog, "activity: {} learned recipes it carried: {}", player->GetName(), carried);
        BuyProvisions(player, target->ToCreature());
        // A bag for an empty bag slot, the largest this vendor sells (the guild storage had none for it).
        if (std::string const bag = BuyBagAt(player, target->ToCreature()); !bag.empty())
            LOG_INFO(ActivityLog, "activity: {} bought a bag at {}: {}", player->GetName(), task.targetName, bag);
        // Weapons and armour this vendor sells that are upgrades for the bot (GearUpkeep.h; the guild helps to pay).
        if (std::string const gear = Gear::BuyAt(player, target->ToCreature()); !gear.empty())
            LOG_INFO(ActivityLog, "activity: {} bought {} from {}", player->GetName(), gear, task.targetName);
        // A town trip: what a craft order of the guild lacks and this vendor sells, and then the order itself when
        // the bot knows a recipe and has what it takes (CraftOrders.h).
        if (AdventureGuild* ownGuild = sGuildAdventureMgr.FindGuildOfMember(player->GetGUID()))
        {
            if (std::string const materials = sCraftOrders.BuyAt(*ownGuild, player, target->ToCreature()); !materials.empty())
                LOG_INFO(ActivityLog, "activity: {} bought {} from {} for a craft order of the guild (the guild's budget)",
                    player->GetName(), materials, task.targetName);
            std::string orderText;
            if (sCraftOrders.Work(*ownGuild, player, orderText))
                BotDriver::Rest(player);
        }
        // The guild storage's shopping list: what it lacks and this vendor sells, paid from the guild bank, straight into
        // the guild storage (ShoppingList.h; no trip is made for the list).
        if (std::string const listed = sShoppingList.BuyAt(player, target->ToCreature()); !listed.empty())
            LOG_INFO(ActivityLog, "activity: {} bought for the guild's shopping list at {}: {}", player->GetName(), task.targetName, listed);
        // The ammo slot shoots the best it carries for its weapon now.
        Ammo::UseBest(player);
        uint32 const food = CountConsumables(player, FoodSpellCategory);
        uint32 const drink = CountConsumables(player, DrinkSpellCategory);
        if (task.restock || food != foodBefore || drink != drinkBefore)
            LOG_INFO("module", "mod-guild-adventure: {} restocked at {}{}: food {} -> {}, drink {} -> {}", player->GetName(),
                task.targetName, selfServe ? " (served itself)" : "", foodBefore, food, drinkBefore, drink);
        if (task.buyItem)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(task.buyItem);
            // The price is booked against the guild's budget, which hands the bot what it lacks; nothing bought, and
            // the money goes back.
            uint32 const bundle = proto ? std::max<uint32>(1, proto->BuyCount) : 1;
            uint32 const price = proto ? uint32(std::min<uint64>(uint64(proto->BuyPrice) * ((task.buyCount + bundle - 1) / bundle),
                std::numeric_limits<uint32>::max())) : 0;
            GuildBudget::Grant const grant = GuildBudget::Request(player, price);
            uint32 const got = grant.ok ? BuyQuestItem(player, target->ToCreature(), task.buyItem, task.buyCount) : 0;
            if (grant.ok && !got)
                GuildBudget::Cancel(player, price, grant);
            if (task.questId)
                LOG_INFO(ActivityLog, "activity: {} bought {} of {} x {} for quest {} \"{}\" from {}", player->GetName(), got,
                    task.buyCount, proto ? proto->Name1 : "?", task.questId, QuestTitle(task.questId), task.targetName);
            else
                LOG_INFO(ActivityLog, "activity: {} bought {} of {} x {} for a recipe of its profession from {}{}", player->GetName(), got,
                    task.buyCount, proto ? proto->Name1 : "?", task.targetName, grant.ok ? "" : " (the guild's budget does not allow it)");
            // A vendor that did not sell (all of) it is not asked again for a while; another one may have it.
            if (got < task.buyCount)
                rt.Avoid(task.spot, AvoidSpawnMs);
            // A limited stock it found sold out is so for the whole guild until it is restocked.
            if (Creature* seller = target->ToCreature(); seller && !got && grant.ok)
                if (VendorItemData const* items = seller->GetVendorItems())
                    if (VendorItem const* sold = items->FindItemCostPair(task.buyItem, 0); sold && sold->maxcount)
                        _soldOut[(uint64(task.spot.spawnId) << 32) | task.buyItem] =
                            std::max<uint32>(getMSTime(), 1) + std::max<uint32>(sold->incrtime, 1800) * IN_MILLISECONDS;
        }
        done = true;
        return false;
    }
    return true;
}

bool OrderService::FindFlight(Player* player, BotRuntime const& rt, uint32 destMap, float destX, float destY, float destZ,
    FlightPlan& plan, int32 maxWalkDanger, char const** whyNot, bool anyWalkShare) const
{
    Perf::Get().flights.fetch_add(1, std::memory_order_relaxed);
    auto no = [whyNot](char const* why)
    {
        if (whyNot)
            *whyNot = why;
        return false;
    };
    if (player->GetMapId() != destMap)
        return no("another map");
    float total = Dist2d(player, destX, destY);
    // (Short as the crow flies can be long on foot: from inside Ironforge the dock of Menethil Harbor is 1169 yd away
    // and 9282 yd by road; a shaman walked that for six minutes before a flight was looked for, in Loch Modan.)
    float walk = -2.0f; // not looked up yet
    auto walkLength = [&]()
    {
        if (walk == -2.0f)
            walk = WalkLength(player, destMap, destX, destY, destZ);
        return walk;
    };
    if (total < FlightMinDistance)
    {
        if (walkLength() >= 0.0f && walk < FlightMinDistance)
            return no("too near for a flight");
        total = std::max(total, walk);
    }

    // The nearest flight master the bot can walk to: the nearest on the map may stand behind a mountain range (Ratchet's
    // from the Valley of Trials). The walk counts with its real length.
    ServicePoint const* master = nullptr;
    float toMaster = 0.0f;
    std::unordered_set<ObjectGuid::LowType> tried;
    for (int attempt = 0; attempt < 3 && !master; ++attempt)
    {
        ServicePoint const* candidate = NearestService(ServiceType::FlightMaster, player, rt, 0,
            [&](ServicePoint const& service) { return !tried.count(service.spawn.spawnId); });
        if (!candidate)
            break;
        tried.insert(candidate->spawn.spawnId);
        float const walk = WalkLength(player, candidate->spawn.mapId, candidate->spawn.x, candidate->spawn.y, candidate->spawn.z);
        if (walk < 0.0f)
            continue;
        master = candidate;
        toMaster = walk;
    }
    // None by the rules for a place to go to: the flight master next to the bot is still its way out. Those rules
    // leave out a master in a place the bot has set aside (it ran from a fight there, it kept away from a creature
    // there), and so the places a bot most needs to leave were the ones it could not fly from: a level-45 warrior
    // 210 yd from Thorium Point's flight master, a level-33 mage in Booty Bay. A walk the path finder has, past
    // nothing far above the bot and through no town of the other side, is enough here.
    if (!master)
    {
        constexpr float WayOutMasterYards = 400.0f;
        float best = WayOutMasterYards;
        for (ServicePoint const& service : sWorldIndex.GetServices(ServiceType::FlightMaster))
        {
            if (service.spawn.mapId != player->GetMapId() || !WorldIndex::IsFriendlyFaction(service.factionTemplate, player))
                continue;
            float const distance = Dist2d(player, service.spawn);
            if (distance >= best)
                continue;
            float const walk = WalkLength(player, service.spawn.mapId, service.spawn.x, service.spawn.y, service.spawn.z);
            if (walk < 0.0f || int32(Risk::WayDanger(player, service.spawn.mapId, service.spawn.x, service.spawn.y, service.spawn.z)) -
                    int32(player->GetLevel()) > RouteMaxDanger)
                continue;
            best = distance;
            master = &service;
            toMaster = walk;
        }
    }
    if (!master)
        return no("no flight master it can walk to");
    TeamId const team = player->GetTeamId();
    uint32 const from = sObjectMgr->GetNearestTaxiNode(master->spawn.x, master->spawn.y, master->spawn.z, master->spawn.mapId, team);
    // The departure node need not be known yet: the bot learns it from the flight master before it takes off (NewRpg's
    // flight action does, like a player who talks to the flight master); every node after it must be known.
    if (!from || !sObjectMgr->GetTaxiMountDisplayId(from, team))
        return no("no flight path it knows goes there");

    // Where to land: the nodes of its side it knows, the nearest to the destination first. The nearest is not always a
    // place the bot can go on from (a level-25 warrior on its way to Stonetalon flew to Nijel's Point, was refused the
    // walk through the Charred Vale there and stood in Desolace, five levels above it, with every way out refused);
    // the next two are looked at as well.
    struct Landing
    {
        uint32 node = 0;
        float distance = 0.0f;
    };
    std::vector<Landing> landings;
    for (uint32 node = 1; node < sTaxiNodesStore.GetNumRows(); ++node)
    {
        TaxiNodesEntry const* entry = sTaxiNodesStore.LookupEntry(node);
        uint32 const field = (node - 1) / 32;
        if (!entry || entry->map_id != destMap || field >= TaxiMaskSize || !(sTaxiNodesMask[field] & (1u << ((node - 1) % 32))) ||
            !KnowsNode(player, node))
            continue;
        landings.push_back({ node, std::sqrt((entry->x - destX) * (entry->x - destX) + (entry->y - destY) * (entry->y - destY)) });
    }
    if (landings.empty())
        return no("no flight path it knows goes there");
    std::sort(landings.begin(), landings.end(), [](Landing const& a, Landing const& b) { return a.distance < b.distance; });
    if (landings.size() > FlightLandings)
        landings.resize(FlightLandings);
    if (landings.front().node == from)
        return no("the nearest flight master is the one of the destination");

    uint32 const destZone = sMapMgr->GetZoneId(PHASEMASK_NORMAL, destMap, destX, destY, destZ);
    bool const roads = sBotSettings.IsOn(player, Setting::RoadsEnable);
    float const roadsMin = sBotSettings.GetFloat(player, Setting::RoadsMinDistance);
    int32 const level = int32(player->GetLevel());
    int32 const walkLimit = maxWalkDanger >= 0 ? maxWalkDanger : RouteMaxDanger;
    float landingWalk = 0.0f; // the walk from the landing that was taken, on foot
    // Why a landing is not taken; null when it is.
    auto refuses = [&](Landing const& candidate) -> char const*
    {
        TaxiNodesEntry const* landing = sTaxiNodesStore.LookupEntry(candidate.node);
        // (As the crow flies until the route says how far it is on foot, below.)
        float fromLanding = candidate.distance;
        // Not into a zone far above the bot when that is not where it wants to go.
        uint32 const landingZone = sMapMgr->GetZoneId(PHASEMASK_NORMAL, landing->map_id, landing->x, landing->y, landing->z);
        if (landingZone != destZone)
            if (ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(landingZone))
                if (zone->minLevel > player->GetLevel() + FlightZoneMargin)
                    return "the landing lies in a zone far above it";
        // The walk from the landing as the travel judges it once the bot stands there: from the flight master on the
        // ground (the taxi node hangs in the air), past no creatures far above the bot and no group it cannot take;
        // without a route, along the straight line.
        if (fromLanding > roadsMin && roads)
        {
            float startX = landing->x, startY = landing->y, startZ = landing->z, nearest = LandingMasterReach;
            for (ServicePoint const& service : sWorldIndex.GetServices(ServiceType::FlightMaster))
            {
                if (service.spawn.mapId != landing->map_id)
                    continue;
                float const away = std::sqrt((service.spawn.x - landing->x) * (service.spawn.x - landing->x) +
                    (service.spawn.y - landing->y) * (service.spawn.y - landing->y));
                if (away >= nearest)
                    continue;
                nearest = away;
                startX = service.spawn.x;
                startY = service.spawn.y;
                startZ = service.spawn.z;
            }
            auto canTake = [&rt, player](uint8 count, uint8 crowdLevel) { return Risk::CanFight(player, rt, crowdLevel, count); };
            // (With the leg check and the set-aside places of the walk itself: without them a landing passed here whose
            // walk on was then planned another way and refused. A level-32 priest flew twice from Booty Bay to the Rebel
            // Camp for a quest area 910 yd on; its route there went around a place it had set aside, 5635 yd through
            // Westfall's nodes past level-37 creatures, and the journey ended "route too dangerous".)
            uint8 const walker = player->GetLevel();
            auto avoided = [&rt, destMap, walker](float px, float py) { return rt.IsInAvoidedArea(destMap, px, py, walker); };
            RoadMap::LegCheck legs;
            if (destMap == player->GetMapId())
                legs = [player](RoutePoint const& from, RoutePoint const& to)
                {
                    return WalkReaches(player, from.x, from.y, from.z, to.x, to.y, to.z);
                };
            RoadRoute const onward = sRoadMap.FindRoute(destMap, startX, startY, startZ, destX, destY, destZ, team, walker,
                legs, rt.avoidAreas.empty() ? RoadMap::AvoidCheck() : RoadMap::AvoidCheck(avoided), canTake);
            if (onward.found)
            {
                // The walk from the landing is the route's, not the line's. (Thunder Bluff, 2026-11-24: from Camp Taurajo
                // a level-25 tauren flew to Thunder Bluff for Malaka'jin, 1017 yd from it as the crow flies and 8749 yd
                // by road down the lift, through Mulgore and the Barrens; on that road it came back to Camp Taurajo, flew
                // again, and went round so six times in fifty minutes.)
                fromLanding = std::max(fromLanding, onward.length);
                if (int32(onward.danger) - level > walkLimit)
                    return "the walk from the landing passes creatures far above it";
                if (RoadMap::GroupCounts(onward.crowd, onward.crowdLevel, level) &&
                    !Risk::CanFight(player, rt, onward.crowdLevel, onward.crowd))
                    return "the walk from the landing passes a group it cannot take";
                // (The travel refuses such a way once the bot has landed: a level-38 hunter flew from Booty Bay to the
                // Rebel Camp for crocolisks on the coast 1246 yd on, and its walk on was refused there for the camp of
                // seven on the safest way.)
                if (onward.camp)
                    return "the walk from the landing leads through a camp it cannot take";
            }
            else if (int32(sRoadMap.DangerAlong(destMap, startX, startY, destX, destY, team)) - level > walkLimit)
                return "the walk from the landing passes creatures far above it";
        }
        // The walks to the master and from the landing against the way the flight saves: the road, not the line (from
        // Raven Hill to Redridge it is 2961 yd as the crow flies and 8559 yd by road; the flight from Darkshire was
        // turned down for its 1900 yd of walking, the road was refused for a pack on it, and the bot sailed to Kalimdor
        // instead). No way on foot at all: the flight is the way.
        if (!anyWalkShare && toMaster + fromLanding > total * FlightMaxWalkShare)
            if (walkLength() >= 0.0f && toMaster + fromLanding > std::max(total, walk) * FlightMaxWalkShare)
                return "the walks to the flight master and from the landing are most of the way";
        landingWalk = fromLanding;
        return nullptr;
    };

    char const* refused = nullptr; // what stood against the nearest landing
    for (Landing const& candidate : landings)
    {
        if (candidate.node == from)
            continue;
        char const* why = refuses(candidate);
        uint32 cost = 0;
        std::vector<uint32> route;
        if (!why)
        {
            route = FlightRoute(player, from, candidate.node, cost);
            if (route.size() < 2)
                why = "no chain of flight paths goes there";
            else if (player->GetMoney() < cost)
                why = "no money for the flight";
        }
        if (why)
        {
            if (!refused)
                refused = why;
            // (Only the nearest landing's reason goes into the refusal line; the others are named here for a test.)
            if (TaxiNodesEntry const* landing = sTaxiNodesStore.LookupEntry(candidate.node); landing && GetSettings().debug)
                LOG_INFO(ActivityLog, "activity: {} would not land at {} ({:.0f} yd from its destination): {}", player->GetName(),
                    landing->name[0], candidate.distance, why);
            continue;
        }
        plan.master = master;
        plan.route = std::move(route);
        plan.cost = cost;
        plan.toMaster = toMaster;
        plan.fromLanding = landingWalk;
        return true;
    }
    return no(refused ? refused : "no flight path it knows goes there");
}

bool OrderService::TryFlight(Player* player, BotRuntime& rt, uint32 destMap, float destX, float destY, float destZ, bool anyWalkShare)
{
    FlightPlan plan;
    rt.task.noFlightWhy = nullptr;
    // (An errand's or a journey's walk from the landing is judged with the limit of a walk to a quest NPC afterwards,
    // so its flight is too: an errand flew, and its walk from the landing was then refused.)
    int32 const walkLimit = (rt.task.errand || rt.task.journey) ? int32(sBotSettings.Get(player, Setting::WalksMaxLevelsAbove)) : -1;
    if (!FindFlight(player, rt, destMap, destX, destY, destZ, plan, walkLimit, &rt.task.noFlightWhy, anyWalkShare))
        return false;

    ServicePoint const* master = plan.master;
    // NewRpg walks to the flight master in one straight move. A master farther away than a short walk is reached by
    // the task's own walk first (WalkTo: a route); the flight is booked when the bot stands next to it.
    if (!IsAtFlightMaster(player, master->spawn))
    {
        if (!rt.task.hasFlightVia)
        {
            LOG_INFO("module", "mod-guild-adventure: {} walks to the flight master {} first ({:.0f} yd)", player->GetName(), master->name,
                plan.toMaster);
            rt.stuckSince = 0; // the stuck tracker now watches the way to the master
        }
        rt.task.hasFlightVia = true;
        rt.task.flightVia = master->spawn;
        return false;
    }
    rt.task.hasFlightVia = false;
    float const total = Dist2d(player, destX, destY);
    BotDriver::Fly(player, master->spawn.entry, master->spawn.mapId, master->spawn.x, master->spawn.y, master->spawn.z, plan.route);
    rt.flightBooked = true;
    rt.task.flew = true;
    // It stands at the master now (TryFlight books only there). With instant flight paths the core puts it at the last
    // node before the planner's next look, so the look would never see it at the master and took every such landing for
    // a jump the planner did not cause (914 of the 6354 flights of the live log by 2026-11-26).
    rt.flightAtMaster = true;
    rt.flightBookedAt = getMSTime();
    rt.flightMap = master->spawn.mapId;
    rt.flightX = master->spawn.x;
    rt.flightY = master->spawn.y;
    LOG_INFO("module", "mod-guild-adventure: {} flies from {} ({} hops, {}c), saves {:.0f} of {:.0f} yd on foot", player->GetName(),
        master->name, plan.route.size() - 1, plan.cost, total - plan.toMaster - plan.fromLanding, total);
    return true;
}

bool OrderService::PlanDiscovery(Player* player, BotRuntime& rt)
{
    uint32 const now = getMSTime();
    if (now < rt.nextDiscoverCheck)
        return false;
    rt.nextDiscoverCheck = now + DiscoverCheckMs;

    ServicePoint const* master = NearestService(ServiceType::FlightMaster, player, rt);
    if (!master || Dist2d(player, master->spawn) > DiscoverDistance)
        return false;
    uint32 const node = sObjectMgr->GetNearestTaxiNode(master->spawn.x, master->spawn.y, master->spawn.z, master->spawn.mapId,
        player->GetTeamId());
    if (!node || player->isTaxiCheater() || player->m_taxi.IsTaximaskNodeKnown(node))
        return false;

    BotTask task;
    task.kind = TaskKind::Discover;
    task.hasSpot = true;
    task.spot = master->spawn;
    task.targetName = master->name;
    StartTask(player, rt, task);
    rt.why = Acore::StringFormat("Learning the flight path at {}", master->name);
    return true;
}

uint32 OrderService::MakeBagSpace(GuildBot const& bot, Player* player)
{
    // As many free slots as a few quests' loot and rewards take.
    constexpr uint32 FreeTarget = 6;
    uint32 const before = player->GetFreeInventorySpace();
    if (!sStorageService.GetOwnerOf(player))
        return 0;
    // Nothing is sold here by the planner's own judgement (the owner's rule: only junk is ever sold automatically).
    // (1) The bags go through the guild's loot filter, as loot does: its rules apply, and what is junk by the junk
    // rule (grey items, white ones nobody in the guild would wear; never materials) is sold.
    sLootService.SweepBags(player);
    // (2) Crafting materials into the guild storage, not what a quest in its log asks for.
    uint32 stored = sStorageService.DepositMaterials(player, nullptr,
        [player](Item* item) { return QuestLogAsksFor(player, item->GetEntry()); }).first;
    // (3) Still full: whatever else the bot does not use itself and the storage takes goes into the guild storage
    // (gear for others, recipes, food and drink it does not keep). Never a quest item or what a quest in its log asks
    // for, what mod-playerbots says it uses, a tool, a key or a bag.
    std::vector<Item*> spare;
    auto needed = [&](uint32 entry)
    {
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(player->GetQuestSlotQuestId(slot));
                quest && !sQuestSupport.IsLeftAlone(player, quest->GetQuestId()))
                for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    if (quest->RequiredItemId[i] == entry)
                        return true;
        return false;
    };
    auto look = [&](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto || proto->Class == ITEM_CLASS_QUEST || proto->Class == ITEM_CLASS_KEY || proto->Class == ITEM_CLASS_CONTAINER ||
            proto->Class == ITEM_CLASS_QUIVER || proto->StartQuest || proto->TotemCategory || Professions::IsTool(proto->ItemId) ||
            (proto->Class == ITEM_CLASS_WEAPON && proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE))
            return;
        if (StorageService::RefuseReason(item) || needed(proto->ItemId) || BotDriver::WantsItem(player, item) ||
            sRoleService.Wants(player, item) || sLootService.IsToolOf(player, proto))
            return;
        spare.push_back(item);
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        look(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = player->GetBagByPos(bagSlot))
            for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                look(bag->GetItemByPos(uint8(i)));
    std::string names;
    uint32 others = 0;
    for (Item* item : spare)
    {
        if (player->GetFreeInventorySpace() >= FreeTarget)
            break;
        std::string const name = Acore::StringFormat("{} x {}", item->GetCount(), item->GetTemplate()->Name1);
        std::string error;
        if (!sStorageService.DepositItem(player, item, error))
            continue;
        if (others < 6)
            names += (names.empty() ? "" : ", ") + name;
        ++others;
    }
    uint32 const after = player->GetFreeInventorySpace();
    if (stored || others || after != before)
        LOG_INFO(ActivityLog, "activity: {} made room at the vendor ({} free bag slots, were {}): its bags went through the loot "
            "filter, {} stack{} of materials and {} other stack{} it does not use into the guild storage{}{}", bot.name, after, before,
            stored, stored == 1 ? "" : "s", others, others == 1 ? "" : "s", names.empty() ? "" : ": ", names);
    return stored + others;
}

bool OrderService::IsQuietToCraft(Player* player, uint32* hostile) const
{
    // What is alive around now, and what lives here by the spawn data (dead for the moment, or too far below the bot
    // for the fight rules to count it).
    uint32 const count = Escape::HostilesAround(player, CraftQuietYards) + sWorldIndex.CountHostileSpawns(player, player->GetMapId(),
        player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), CraftQuietYards);
    if (hostile)
        *hostile = count;
    return count == 0;
}

bool OrderService::PlanQuietPlace(GuildBot const& bot, Player* player, BotRuntime& rt)
{
    constexpr float QuietMin = 40.0f;
    constexpr float QuietRange = 800.0f;
    constexpr float QuietNodeYards = 45.0f;
    constexpr uint32 QuietGapMs = 3 * MINUTE * IN_MILLISECONDS;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    uint32 const mapId = player->GetMapId();
    if (now < rt.nextQuietWalk || player->GetMap()->IsDungeon())
        return false;
    rt.nextQuietWalk = now + QuietGapMs;
    // The road map's nodes around, nearest first: one with no hostile spawn around it, that the path finder walks to
    // and whose straight way the walk gate would let through.
    int32 const margin = int32(sBotSettings.Get(player, Setting::WalksMaxLevelsAbove));
    float minDistance = QuietMin;
    for (int asked = 0; asked < 8; ++asked)
    {
        RoutePoint node;
        std::string name;
        if (!sRoadMap.FindNearestNode(mapId, player->GetPositionX(), player->GetPositionY(), minDistance, QuietRange, node, name,
            player->GetTeamId(), player->GetLevel()))
            break;
        float const distance = Dist2d(player->GetPositionX(), player->GetPositionY(), node.x, node.y);
        minDistance = distance + 10.0f;
        if (rt.IsInAvoidedArea(mapId, node.x, node.y, player->GetLevel()) ||
            sWorldIndex.CountHostileSpawns(player, mapId, node.x, node.y, node.z, QuietNodeYards) ||
            int32(sRoadMap.DangerAlong(mapId, player->GetPositionX(), player->GetPositionY(), node.x, node.y, player->GetTeamId())) -
                int32(player->GetLevel()) > margin ||
            !WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), node.x, node.y, node.z))
            continue;
        BotTask task;
        task.kind = TaskKind::Travel;
        task.retreat = true;
        task.hasSpot = true;
        task.spot.mapId = mapId;
        task.spot.x = node.x;
        task.spot.y = node.y;
        task.spot.z = node.z;
        task.targetName = name;
        LOG_INFO(ActivityLog, "activity: {} has nothing else to do and does not craft among {} hostile creatures at ({:.0f}, {:.0f}): "
            "it walks {:.0f} yd to {} first", bot.name, rt.unquietHostiles, player->GetPositionX(), player->GetPositionY(), distance, name);
        StartTask(player, rt, task);
        rt.nextCraftCheck = 0;
        rt.why = Acore::StringFormat("Profession mode: walking to a quiet place to craft at ({})", name);
        return true;
    }
    return false;
}

namespace
{
    struct BotWait
    {
        uint8 kind = 0;      // 1 went ashore, 2 a zone that waits, 3 a hand-in given up (and 4 to 8, below)
        uint32 id = 0;
        uint32 until = 0;    // unix time
    };
    constexpr uint8 WaitCrossed = 1;
    constexpr uint8 WaitZone = 2;
    constexpr uint8 WaitErrand = 3;
    constexpr uint8 WaitDropped = 4;   // a quest dropped for the quest log (QuestSupport::NoteDropped)
    // 5 to 7: BotRuntime::lastingAvoids (LastingCreature, LastingObject, LastingArea at the top of the file); 8: a zone
    // found empty (WaitAfarEmpty, there too)
    std::unordered_map<uint32, std::vector<BotWait>> sBotWaits; // character (low guid) -> its waits
}

void OrderService::LoadBotWaits()
{
    sBotWaits.clear();
    uint32 const nowUnix = uint32(GameTime::GetGameTime().count());
    CharacterDatabase.Execute("DELETE FROM ga_bot_wait WHERE until_time <= {}", nowUnix);
    uint32 count = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT bot_guid, kind, id, until_time FROM ga_bot_wait WHERE until_time > {}", nowUnix))
        do
        {
            Field* fields = result->Fetch();
            sBotWaits[fields[0].Get<uint32>()].push_back({ fields[1].Get<uint8>(), fields[2].Get<uint32>(), fields[3].Get<uint32>() });
            ++count;
        } while (result->NextRow());
    LOG_INFO("module", "mod-guild-adventure: {} waits of {} characters from before the restart (their side of the sea, zones, "
        "hand-ins given up)", count, sBotWaits.size());
}

uint32 OrderService::RestoreBotWaits(ObjectGuid guid, BotRuntime& rt)
{
    auto known = sBotWaits.find(guid.GetCounter());
    if (known == sBotWaits.end())
        return 0;
    uint32 const nowUnix = uint32(GameTime::GetGameTime().count());
    uint32 const now = getMSTime();
    uint32 restored = 0;
    for (BotWait const& wait : known->second)
    {
        if (wait.until <= nowUnix)
            continue;
        uint32 const leftMs = uint32(std::min<uint64>(uint64(wait.until - nowUnix) * IN_MILLISECONDS, 12 * HOUR * IN_MILLISECONDS));
        switch (wait.kind)
        {
            case WaitCrossed:
            {
                // (A time before the server's start: the difference is what counts, getMSTimeDiff wraps.)
                uint32 const ago = BotRuntime::SeaCrossRestMs - std::min<uint32>(leftMs, BotRuntime::SeaCrossRestMs);
                rt.lastCrossingAt = now - ago;
                if (!rt.lastCrossingAt)
                    rt.lastCrossingAt = 1;
                rt.errandCrossing = wait.id;
                break;
            }
            case WaitZone:
                rt.unreachableZones[wait.id] = now + leftMs;
                break;
            case WaitErrand:
                rt.errandSkip[wait.id] = now + leftMs;
                break;
            case WaitDropped:
                sQuestSupport.NoteDropped(guid, wait.id, wait.until);
                break;
            case LastingCreature:
            case LastingObject:
            {
                SpawnPoint spawn;
                spawn.spawnId = wait.id;
                spawn.gameObject = wait.kind == LastingObject;
                rt.Avoid(spawn, leftMs);
                rt.lastingAvoids.push_back({ wait.kind, wait.id, now + leftMs });
                break;
            }
            case WaitAfarEmpty:
                rt.afarEmpty[wait.id & 0xFFFFFF] = { now + leftMs, uint8(wait.id >> 24) };
                break;
            case LastingArea:
            {
                uint32 areaMap = 0;
                float areaX = 0.0f, areaY = 0.0f;
                if (!LastingAreaSpot(wait.id, areaMap, areaX, areaY))
                    continue;
                rt.AddAvoidArea(areaMap, areaX, areaY, 255, leftMs, false);
                rt.lastingAvoids.push_back({ wait.kind, wait.id, now + leftMs });
                break;
            }
            default:
                continue;
        }
        ++restored;
    }
    return restored;
}

void OrderService::SaveBotWaits(ObjectGuid guid, BotRuntime& rt)
{
    constexpr uint32 WaitsLookMs = 10 * IN_MILLISECONDS;
    uint32 const now = getMSTime();
    if (now < rt.nextWaitsSave)
        return;
    rt.nextWaitsSave = now + WaitsLookMs;

    uint32 const nowUnix = uint32(GameTime::GetGameTime().count());
    std::vector<BotWait> waits;
    // (The sum is over what was set, in server time: it changes when a wait is added, changed or over.)
    uint64 sum = 0;
    auto add = [&](uint8 kind, uint32 id, uint32 setMs, uint32 leftMs)
    {
        waits.push_back({ kind, id, nowUnix + leftMs / IN_MILLISECONDS + 1 });
        sum += (uint64(kind) << 56) ^ (uint64(id) * 0x9E3779B97F4A7C15ull) ^ (uint64(setMs) << 8);
    };
    if (rt.CrossedLately())
        add(WaitCrossed, rt.errandCrossing, rt.lastCrossingAt, BotRuntime::SeaCrossRestMs - getMSTimeDiff(rt.lastCrossingAt, now));
    for (auto const& [zone, until] : rt.unreachableZones)
        if (until > now)
            add(WaitZone, zone, until, until - now);
    for (auto const& [quest, until] : rt.errandSkip)
        if (until > now)
            add(WaitErrand, quest, until, until - now);
    for (auto const& [zone, mark] : rt.afarEmpty)
        if (mark.first > now)
            add(WaitAfarEmpty, zone | (uint32(mark.second) << 24), mark.first, mark.first - now);
    std::erase_if(rt.lastingAvoids, [now](BotRuntime::LastingAvoid const& avoid) { return avoid.until <= now; });
    for (BotRuntime::LastingAvoid const& avoid : rt.lastingAvoids)
        add(avoid.kind, avoid.id, avoid.until, avoid.until - now);
    for (auto const& [quest, untilUnix] : sQuestSupport.DroppedBy(guid))
    {
        waits.push_back({ WaitDropped, quest, untilUnix });
        sum += (uint64(WaitDropped) << 56) ^ (uint64(quest) * 0x9E3779B97F4A7C15ull) ^ (uint64(untilUnix) << 8);
    }
    if (sum == rt.waitsSaved)
        return;
    rt.waitsSaved = sum;

    uint32 const low = guid.GetCounter();
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    trans->Append("DELETE FROM ga_bot_wait WHERE bot_guid = {}", low);
    for (BotWait const& wait : waits)
        trans->Append("INSERT INTO ga_bot_wait (bot_guid, kind, id, until_time) VALUES ({}, {}, {}, {})", low, uint32(wait.kind), wait.id,
            wait.until);
    CharacterDatabase.CommitTransaction(trans);
    if (waits.empty())
        sBotWaits.erase(low);
    else
        sBotWaits[low] = std::move(waits);
}

void OrderService::LoadGuildDeaths()
{
    sGuildDeaths.clear();
    uint32 const oldest = uint32(std::max<time_t>(GameTime::GetGameTime().count() - GuildDeathKeepSeconds, 0));
    CharacterDatabase.Execute("DELETE FROM ga_guild_death WHERE died_at < {}", oldest);
    uint32 count = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT owner_account, zone_id, quest_id, level, died_at FROM ga_guild_death "
        "WHERE died_at >= {} ORDER BY died_at", oldest))
        do
        {
            Field* fields = result->Fetch();
            sGuildDeaths[fields[0].Get<uint32>()].push_back({ fields[1].Get<uint32>(), fields[2].Get<uint32>(), fields[3].Get<uint8>(),
                time_t(fields[4].Get<uint32>()) });
            ++count;
        } while (result->NextRow());
    LOG_INFO("module", "mod-guild-adventure: {} deaths of guild characters in the last {} h (zones and quests that keep killing wait)",
        count, GuildDeathKeepSeconds / HOUR);

    // Quest NPCs the guilds could not get to (ga_guild_npc_block).
    sNpcBlocks.clear();
    uint32 const now = uint32(GameTime::GetGameTime().count());
    CharacterDatabase.Execute("DELETE FROM ga_guild_npc_block WHERE last_at < {} AND blocked_until < {}",
        uint32(std::max<time_t>(time_t(now) - NpcBlockSeconds, 0)), now);
    uint32 blocked = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT owner_account, spawn_id, is_gameobject, fails, last_at, blocked_until "
        "FROM ga_guild_npc_block"))
        do
        {
            Field* fields = result->Fetch();
            SpawnPoint spawn;
            spawn.spawnId = fields[1].Get<uint32>();
            spawn.gameObject = fields[2].Get<uint8>() != 0;
            NpcBlock& block = sNpcBlocks[fields[0].Get<uint32>()][NpcKey(spawn)];
            block.fails = fields[3].Get<uint8>();
            block.lastAt = time_t(fields[4].Get<uint32>());
            block.until = time_t(fields[5].Get<uint32>());
            if (block.until > time_t(now))
                ++blocked;
        } while (result->NextRow());
    LOG_INFO("module", "mod-guild-adventure: {} quest NPCs are left alone by a guild whose characters could not get to them", blocked);
}

bool BotRuntime::NoteRefusedStart(float x, float y, uint32 now)
{
    std::erase_if(refusedStarts, [now](RefusedStart const& start) { return now >= start.until; });
    for (RefusedStart& start : refusedStarts)
        if ((start.x - x) * (start.x - x) + (start.y - y) * (start.y - y) <= RefusedStartYards * RefusedStartYards)
        {
            start.until = now + RefusedStartMs;
            return ++start.count == RefusedStartTimes;
        }
    refusedStarts.push_back({ x, y, now + RefusedStartMs, 1 });
    return false;
}

bool BotRuntime::IsRefusedStart(float x, float y, uint32 now) const
{
    for (RefusedStart const& start : refusedStarts)
        if (start.count >= RefusedStartTimes && now < start.until &&
            (start.x - x) * (start.x - x) + (start.y - y) * (start.y - y) <= RefusedStartYards * RefusedStartYards)
            return true;
    return false;
}

// A travel's route is refused for a group that stands where the bot is (the Witherbark village in Arathi: every link
// out of its node passes more trolls, so the walk to the flight master and the travels to three zones were refused
// one after the other, and the guild leader sailed for Dustwallow Marsh): the bot is in that camp, staying is no
// safer than leaving, and the way out is planned once more without what stands within CampOutStretch of it. What
// stands farther off refuses the travel as before. True when the route is to be planned again.
// The legs of a way of the whole navmesh judged as a straight way is (StraightWayIsSafe): what lives on them, a group,
// a camp, a place set aside. `why` names what refuses it.
bool OrderService::NavWayIsSafe(Player* player, BotRuntime& rt, std::vector<NavPoint> const& corners, std::string& why) const
{
    int32 const myLevel = int32(player->GetLevel());
    int32 const margin = int32(sBotSettings.Get(player, Setting::WalksMaxLevelsAbove));
    TeamId const team = player->GetTeamId();
    bool const standsAtCamp = StandsAtCamp(player, rt);
    constexpr float CampExitYards = 60.0f;
    bool const inAvoided = rt.IsInAvoidedArea(player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
        uint8(myLevel));
    float walked = 0.0f;
    for (std::size_t i = 1; i < corners.size(); ++i)
    {
        NavPoint const& a = corners[i - 1];
        NavPoint const& b = corners[i];
        float const leg = std::hypot(b.x - a.x, b.y - a.y);
        bool const last = i + 1 == corners.size();
        uint8 const danger = sRoadMap.DangerAlong(player->GetMapId(), a.x, a.y, b.x, b.y, team,
            last ? std::min(leg, RoadMap::EndStretch) : 0.0f, a.z, b.z);
        if (int32(danger) - myLevel > margin)
        {
            why = Acore::StringFormat("it passes {}", RoadMap::DangerName(danger));
            return false;
        }
        if (walked + leg > RoadMap::CrowdStartStretch)
        {
            uint8 crowdLevel = 0;
            uint8 const crowd = sRoadMap.CrowdAlong(player->GetMapId(), a.x, a.y, b.x, b.y, team, crowdLevel, a.z,
                b.z, std::max(0.0f, RoadMap::CrowdStartStretch - walked));
            if (RoadMap::GroupCounts(crowd, crowdLevel, myLevel) && !Risk::CanFight(player, rt, crowdLevel, crowd))
            {
                why = Acore::StringFormat("{} level-{} creatures stand together on it", uint32(crowd), uint32(crowdLevel));
                return false;
            }
        }
        // (At a camp's edge only its first yards are let pass, the way out: the whole way was let pass before, and a
        // way of the navmesh from beside a camp may lead through its middle.)
        float const campSkip = standsAtCamp ? CampExitYards : 10.0f;
        if (walked + leg > campSkip && Risk::EntersCamp(player, rt, player->GetMapId(), a.x, a.y, b.x, b.y, nullptr, nullptr,
                walked < campSkip ? campSkip - walked : 0.0f, last ? 45.0f : 0.0f))
        {
            why = "it leads into the middle of a camp it cannot take";
            return false;
        }
        if (!inAvoided)
            for (float along = 0.0f; along < leg; along += 20.0f)
            {
                float const share = leg > 1.0f ? along / leg : 0.0f;
                if (rt.IsInAvoidedArea(player->GetMapId(), a.x + (b.x - a.x) * share, a.y + (b.y - a.y) * share,
                        uint8(myLevel)))
                {
                    why = "it leads through a place it has set aside";
                    return false;
                }
            }
        walked += leg;
    }
    return true;
}

bool OrderService::StandsAtCamp(Player const* player, BotRuntime const& rt) const
{
    constexpr float CampEdgeYards = 35.0f;
    uint32 const mapId = player->GetMapId();
    float const x = player->GetPositionX(), y = player->GetPositionY(), z = player->GetPositionZ();
    if (Risk::InCampItCannotTake(player, rt, mapId, x, y, z))
        return true;
    for (int direction = 0; direction < 8; ++direction)
    {
        float const angle = float(direction) * 0.785398f;
        if (Risk::InCampItCannotTake(player, rt, mapId, x + CampEdgeYards * std::cos(angle), y + CampEdgeYards * std::sin(angle), z))
            return true;
    }
    return false;
}

bool OrderService::ReachableFromLanding(Player* player, BotRuntime const& rt, TravelLink const& link, ZoneTemplate const& zone) const
{
    constexpr float LandingMasterYards = 600.0f; // a flight master this near to the landing
    constexpr float ZoneMasterYards = 1500.0f;   // one this near to the zone's hub counts as the zone's
    TeamId const team = player->GetTeamId();
    int32 const level = int32(player->GetLevel());
    uint32 const mapId = link.to.mapId;
    ZoneHub const* hub = sWorldIndex.GetHub(zone.zoneId, team);
    // (What the planner cannot judge it does not refuse.)
    if (!hub || hub->mapId != mapId || !sBotSettings.IsOn(player, Setting::RoadsEnable))
        return true;
    if (sMapMgr->GetZoneId(PHASEMASK_NORMAL, mapId, link.to.x, link.to.y, link.to.z) == zone.zoneId)
        return true;
    auto canTake = [&rt, player](uint8 count, uint8 crowdLevel) { return Risk::CanFight(player, rt, crowdLevel, count); };
    auto walkable = [&](float x1, float y1, float z1, float x2, float y2, float z2)
    {
        RoadRoute const route = sRoadMap.FindRoute(mapId, x1, y1, z1, x2, y2, z2, team, uint8(level), {}, {}, canTake);
        if (!route.found)
            return int32(sRoadMap.DangerAlong(mapId, x1, y1, x2, y2, team)) - level <= RouteMaxDanger;
        if (int32(route.danger) - level > RouteMaxDanger)
            return false;
        return !(RoadMap::GroupCounts(route.crowd, route.crowdLevel, level) &&
            !Risk::CanFight(player, rt, route.crowdLevel, route.crowd));
    };
    if (walkable(link.to.x, link.to.y, link.to.z, hub->x, hub->y, hub->z))
        return true;
    // By flight: a flight master at the landing ...
    std::vector<ServicePoint> const& masters = sWorldIndex.GetServices(ServiceType::FlightMaster);
    bool atLanding = false;
    for (ServicePoint const& master : masters)
    {
        if (master.spawn.mapId != mapId || !WorldIndex::IsFriendlyFaction(master.factionTemplate, team))
            continue;
        float const away = std::sqrt((master.spawn.x - link.to.x) * (master.spawn.x - link.to.x) +
            (master.spawn.y - link.to.y) * (master.spawn.y - link.to.y));
        if (away <= LandingMasterYards && walkable(link.to.x, link.to.y, link.to.z, master.spawn.x, master.spawn.y, master.spawn.z))
        {
            atLanding = true;
            break;
        }
    }
    if (!atLanding)
        return false;
    // ... and one at the zone, with a way on from it.
    for (ServicePoint const& master : masters)
    {
        if (master.spawn.mapId != mapId || !WorldIndex::IsFriendlyFaction(master.factionTemplate, team))
            continue;
        float const toHub = std::sqrt((master.spawn.x - hub->x) * (master.spawn.x - hub->x) + (master.spawn.y - hub->y) * (master.spawn.y - hub->y));
        if (sWorldIndex.GetSpawnZone(master.spawn) != zone.zoneId && toHub > ZoneMasterYards)
            continue;
        if (walkable(master.spawn.x, master.spawn.y, master.spawn.z, hub->x, hub->y, hub->z))
            return true;
    }
    return false;
}

bool OrderService::PauseForRoamer(Player* player, BotRuntime& rt, std::string const& who)
{
    // At most a minute per task (was four): after that the task goes on, and the look-out steps aside when the
    // creature really comes (the owner, 2026-11-14: bots must not wait at random). Only elites and creatures four or more
    // levels above the bot are waited for (Escape::StrengthOf).
    constexpr uint32 RoamerWaitMs = 20 * IN_MILLISECONDS;
    constexpr uint32 RoamerWaitMaxMs = 1 * MINUTE * IN_MILLISECONDS;
    BotTask& task = rt.task;
    if (task.roamerWaited >= RoamerWaitMaxMs)
        return false;
    uint32 const now = getMSTime();
    if (!who.empty())
        task.roamerName = who;
    // (One line per wait, not per look.)
    if (!task.roamerWaitUntil)
        LOG_INFO(ActivityLog, "activity: {} waits with its {} {} {}: {} is on its way ({} s waited for such creatures so far)",
            player->GetName(), ToString(task.kind), task.kind == TaskKind::Voyage || task.kind == TaskKind::Travel ? "to" : "at",
            task.targetName.empty() ? "-" : task.targetName,
            task.roamerName.empty() ? std::string("a creature far above its level") : task.roamerName,
            task.roamerWaited / IN_MILLISECONDS);
    task.roamerWaitUntil = now + RoamerWaitMs;
    task.roamerWaited += RoamerWaitMs;
    rt.why = Acore::StringFormat("Waiting for {} to pass", task.roamerName.empty() ? std::string("a creature far above its level") :
        task.roamerName);
    return true;
}

bool OrderService::StepRoamerWait(Player* player, BotRuntime& rt)
{
    constexpr uint32 RoamerLookMs = 3 * IN_MILLISECONDS;
    constexpr float RoamerAheadYards = 300.0f;
    constexpr float RoamerChunkYards = 60.0f;
    BotTask& task = rt.task;
    uint32 const now = getMSTime();
    if (!player->IsAlive() || player->IsInCombat() || player->GetTransport() || player->IsInFlight() ||
        BotDriver::GetRpgState(player) == RpgState::TravelFlight)
    {
        task.roamerWaitUntil = 0;
        return false;
    }
    bool const waiting = task.roamerWaitUntil != 0;
    // (Only a journey looks ahead for one by itself; any other task waits only after it kept away from one.)
    bool const journey = (task.kind == TaskKind::Travel || task.kind == TaskKind::Voyage) && !task.retreat;
    if (!waiting && !journey)
        return false;
    if (waiting && now < task.roamerWaitUntil)
    {
        // (The stuck clocks stand still meanwhile.)
        PauseStuck(rt);
        rt.why = Acore::StringFormat("Waiting for {} to pass", task.roamerName.empty() ? std::string("a creature far above its level") :
            task.roamerName);
        return true;
    }
    if (!waiting && now < task.nextRoamerLook)
        return false;
    task.nextRoamerLook = now + RoamerLookMs;
    // Not on a pier: what roams there swims below it (a level-33 bot waited two minutes on Theramore's pier for the
    // Coral Shark that patrols under it, with its ship at the pier's end).
    constexpr float PierYards = 250.0f;
    if (sRoadMap.IsNearTransportNode(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), PierYards))
    {
        if (waiting)
        {
            task.roamerWaitUntil = 0;
            task.lastAssert = 0;
            task.walkX = task.walkY = 0.0f;
        }
        return false;
    }

    // The next stretch of the way: along the route when the bot is on one, else towards where it walks.
    std::string who;
    bool onLeg = false;
    // (A creature that stays about the place, a town's guard or a short round, is not waited for: Escape::StaysAround.)
    bool stays = false;
    uint32 const mapId = player->GetMapId();
    uint8 const level = player->GetLevel();
    float fromX = player->GetPositionX(), fromY = player->GetPositionY();
    if (task.route.found && task.route.mapId == mapId && task.routeIndex < task.route.points.size())
    {
        float ahead = 0.0f, chunk = 0.0f;
        float chunkX = fromX, chunkY = fromY;
        for (std::size_t i = task.routeIndex; i < task.route.points.size() && ahead < RoamerAheadYards && !onLeg; ++i)
        {
            RoutePoint const& point = task.route.points[i];
            float const step = std::sqrt((point.x - fromX) * (point.x - fromX) + (point.y - fromY) * (point.y - fromY));
            ahead += step;
            chunk += step;
            fromX = point.x;
            fromY = point.y;
            if (chunk < RoamerChunkYards && i + 1 < task.route.points.size())
                continue;
            onLeg = Escape::RoamerOnLeg(mapId, chunkX, chunkY, point.x, point.y, level, &who, &stays);
            chunkX = point.x;
            chunkY = point.y;
            chunk = 0.0f;
        }
    }
    else
    {
        float toX = task.walkX, toY = task.walkY;
        bool known = toX != 0.0f || toY != 0.0f;
        if (!known && task.hasSpot && task.spot.mapId == mapId)
        {
            toX = task.spot.x;
            toY = task.spot.y;
            known = true;
        }
        if (known)
            onLeg = Escape::RoamerOnLeg(mapId, fromX, fromY, toX, toY, level, &who, &stays);
    }
    if (onLeg && !stays && PauseForRoamer(player, rt, who))
    {
        if (!waiting)
            BotDriver::Rest(player);
        PauseStuck(rt);
        return true;
    }
    if (waiting)
    {
        // On with the walk: it is set again from where the bot stands.
        task.roamerWaitUntil = 0;
        task.lastAssert = 0;
        task.walkX = task.walkY = 0.0f;
        task.stallSince = 0;
    }
    return false;
}

bool OrderService::WidenTravelStart(Player* player, BotTask& task, char const* what)
{
    if (task.wideStart || !task.route.found || task.route.crowdDistance > RoadMap::CampOutStretch)
        return false;
    LOG_INFO(ActivityLog, "activity: {} stands {:.0f} yd from the {} level-{} creatures on its way to {}: it is in that camp, and "
        "its way out is planned without what stands within {:.0f} yd of it", player->GetName(), task.route.crowdDistance,
        uint32(task.route.crowd), uint32(task.route.crowdLevel), what, RoadMap::CampOutStretch);
    task.wideStart = true;
    task.route = RoadRoute();
    task.routePlanned = false;
    task.nextRoutePlan = 0;
    return true;
}

// Stranglethorn Vale from Booty Bay: a level-35 bot whose inn is there has its quests 3000 yd to the north, behind the
// level-40 country of the Cape. Every walk was refused ("the way passes level-41 creatures", 46 such lines of one bot
// in an hour), then the zone had "nothing it can safely get to" and the bot left it. A player flies to Darkshire or
// Sentinel Hill and rides down from the north.
bool OrderService::StartJourney(GuildBot const& bot, Player* player, BotRuntime& rt, char const** whyNot, bool farAround, bool longWay)
{
    BotTask const& task = rt.task;
    uint32 const now = getMSTime();
    if (whyNot)
        *whyNot = nullptr;
    // (An objective at "the quest area" has no spawn for its spot, but the quest's own point on the map: from Booty Bay
    // the areas of Stranglethorn's hunting quests lie 2500 to 3000 yd north, with no spawn of their targets in the
    // planner's reach; their walks were refused one after the other and no flight took their place.)
    bool const area = task.kind == TaskKind::Objective && !task.hasSpot && (task.spot.x != 0.0f || task.spot.y != 0.0f);
    // (A spot near as the crow flies whose way on foot is far around counts by that way: from Silithus the targets of
    // "Savage Flora" lie 800 yd off, down in Un'Goro, and the road there is 5000 to 7000 yd, round 157.)
    if ((!task.hasSpot && !area) || task.kind == TaskKind::Travel || task.kind == TaskKind::Voyage ||
        task.spot.mapId != player->GetMapId() || player->GetTransport() ||
        (Dist2d(player, task.spot) < FlightMinDistance && !(farAround && task.route.found && task.route.length >= FlightMinDistance)))
        return false;
    if (now < rt.nextJourney)
    {
        if (whyNot)
            *whyNot = "it has just flown for another task";
        return false;
    }
    FlightPlan plan;
    if (!FindFlight(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z, plan,
        int32(sBotSettings.Get(player, Setting::WalksMaxLevelsAbove)), whyNot))
        return false;

    BotTask journey;
    journey.kind = TaskKind::Travel;
    journey.journey = true;
    journey.hasSpot = true;
    journey.spot = task.spot;
    journey.questId = task.questId;
    journey.targetName = task.targetName;
    LOG_INFO(ActivityLog, "activity: {} goes to {} ({}{}) by flight: the way on foot from here is {}, the flight from {} lands "
        "{:.0f} yd from it ({:.0f} yd away as the crow flies)", bot.name, task.targetName, ToString(task.kind),
        task.questId ? Acore::StringFormat(", quest {} \"{}\"", task.questId, QuestTitle(task.questId)) : std::string(),
        longWay ? "long" : "refused", plan.master ? plan.master->name : "?", plan.fromLanding, Dist2d(player, task.spot));
    rt.nextJourney = now + JourneyGapMs;
    ClearTask(rt, 0, "by flight instead");
    StartTask(player, rt, journey);
    rt.why = Acore::StringFormat("Flying towards {}: the way on foot is {}", journey.targetName, longWay ? "long" : "too dangerous");
    return true;
}

bool OrderService::IsNpcBlocked(Player const* player, SpawnPoint const& spawn)
{
    return IsBlockedIn(NpcBlocksOf(player), spawn);
}

void OrderService::NoteSpotUnreached(Player const* player, BotTask const& task)
{
    if (AdventureGuild const* guild = sGuildAdventureMgr.FindGuildOfMember(player->GetGUID()))
        NoteNpcUnreached(*guild, player->GetName(), task);
}

bool OrderService::PlanRetreat(GuildBot const& bot, Player* player, BotRuntime& rt)
{
    constexpr float RetreatMin = 60.0f;
    constexpr float RetreatRange = 800.0f;
    // One try in a while: a retreat that does not get out (its walk ended for what stood on it) must not keep the
    // planner from what comes after it, the look at the other zones and the travel there. (A level-32 bot in Booty
    // Bay began the same retreat every 27 s for as long as it was watched.)
    constexpr uint32 RetreatGapMs = 5 * MINUTE * IN_MILLISECONDS;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    uint32 const mapId = player->GetMapId();
    if (now < rt.nextRetreat || player->GetMap()->IsDungeon() ||
        IsPlaceUsable(player, rt, mapId, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ()))
        return false;
    // The road map's nodes around, nearest first: one that is a safe place and that the path finder walks to.
    float minDistance = RetreatMin;
    for (int asked = 0; asked < 6; ++asked)
    {
        RoutePoint node;
        std::string name;
        if (!sRoadMap.FindNearestNode(mapId, player->GetPositionX(), player->GetPositionY(), minDistance, RetreatRange, node, name,
            player->GetTeamId(), player->GetLevel()))
            break;
        float const distance = Dist2d(player->GetPositionX(), player->GetPositionY(), node.x, node.y);
        minDistance = distance + 10.0f;
        if (rt.IsInAvoidedArea(mapId, node.x, node.y, player->GetLevel()) || !IsPlaceUsable(player, rt, mapId, node.x, node.y, node.z) ||
            !WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), node.x, node.y, node.z))
            continue;
        // Not a node inside the camp it is to get out of: the guild leader was sent "75 yd out" to the node of the
        // troll village it stood in, and every way from there led past the village's groups.
        if (sRoadMap.IsNodeAmongGroups(mapId, node.x, node.y, player->GetTeamId(), player->GetLevel(),
            [&rt, player](uint8 count, uint8 crowdLevel) { return Risk::CanFight(player, rt, crowdLevel, count); }))
            continue;
        // Nor a node in the middle of a group it cannot take, with one link out of it free: Velmira (24) was sent "180 yd
        // out" to the node of Northwatch Hold, among 13 Theramore marines of level 16 and 17, and every travel from
        // there ended on a pack of them until the place held it and it used its hearthstone (2026-11-26).
        if (uint8 crowdLevel = 0, crowd = sRoadMap.GroupAtPoint(mapId, node.x, node.y, node.z, player->GetTeamId(), crowdLevel);
            RoadMap::GroupCounts(crowd, crowdLevel, int32(player->GetLevel())) && !Risk::CanFight(player, rt, crowdLevel, crowd))
            continue;
        BotTask task;
        task.kind = TaskKind::Travel;
        task.retreat = true;
        task.hasSpot = true;
        task.spot.mapId = mapId;
        task.spot.x = node.x;
        task.spot.y = node.y;
        task.spot.z = node.z;
        task.targetName = name;
        LOG_INFO(ActivityLog, "activity: {} has nothing to do at ({:.0f}, {:.0f}) and stands among creatures it cannot take: it "
            "walks {:.0f} yd out to {}", bot.name, player->GetPositionX(), player->GetPositionY(), distance, name);
        StartTask(player, rt, task);
        rt.nextRetreat = now + RetreatGapMs;
        rt.why = Acore::StringFormat("Leaving a place too dangerous for it: walking to {}", name);
        return true;
    }
    return false;
}

bool OrderService::TakeSteppingStone(Player* player, BotRuntime& rt, float towardX, float towardY)
{
    constexpr uint8 MaxStones = 3;
    constexpr float StoneAhead = 60.0f;
    constexpr uint32 StoneMs = 45 * IN_MILLISECONDS;
    BotTask& task = rt.task;
    if (task.stones >= MaxStones || task.stoneUntil)
        return false;

    // The nearest road map nodes, a few steps away at least: of the path finder's paths towards them the point a
    // stretch along. The one that ends nearest to where the refused step wanted to go.
    bool found = false;
    float bestLeft = 0.0f, bestX = 0.0f, bestY = 0.0f, bestZ = 0.0f;
    std::string bestName;
    float minDistance = 30.0f;
    for (int asked = 0; asked < 4; ++asked)
    {
        RoutePoint node;
        std::string name;
        if (!sRoadMap.FindNearestNode(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), minDistance, RescueRange, node,
            name))
            break;
        minDistance = Dist2d(player->GetPositionX(), player->GetPositionY(), node.x, node.y) + 10.0f;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (!PathPointAhead(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), node.x, node.y, node.z,
            StoneAhead, x, y, z))
            continue;
        float const left = Dist2d(x, y, towardX, towardY);
        if (!found || left < bestLeft)
        {
            found = true;
            bestLeft = left;
            bestX = x;
            bestY = y;
            bestZ = z;
            bestName = name;
        }
    }
    if (!found)
        return false;

    uint32 const now = std::max<uint32>(getMSTime(), 1);
    ++task.stones;
    task.stoneUntil = now + StoneMs;
    task.stoneX = bestX;
    task.stoneY = bestY;
    task.stoneZ = bestZ;
    LOG_INFO(ActivityLog, "activity: {} has no way from ({:.0f}, {:.0f}, {:.0f}) towards ({:.0f}, {:.0f}): it walks {:.0f} yd along "
        "the way to {} first (stepping stone {} of {})", player->GetName(), player->GetPositionX(), player->GetPositionY(),
        player->GetPositionZ(), towardX, towardY, Dist2d(player->GetPositionX(), player->GetPositionY(), bestX, bestY), bestName,
        uint32(task.stones), uint32(MaxStones));
    BotDriver::GoTo(player, player->GetMapId(), bestX, bestY, bestZ, false);
    task.lastAssert = now;
    return true;
}

bool OrderService::StepSteppingStone(Player* player, BotRuntime& rt)
{
    constexpr float StoneReached = 6.0f;
    constexpr uint32 StoneAssertMs = 5 * IN_MILLISECONDS;
    BotTask& task = rt.task;
    if (!task.stoneUntil)
        return false;
    uint32 const now = getMSTime();
    if (now < task.stoneUntil && !player->IsInCombat() &&
        Dist2d(player->GetPositionX(), player->GetPositionY(), task.stoneX, task.stoneY) > StoneReached)
    {
        if (BotDriver::GetRpgState(player) != RpgState::GoCamp && getMSTimeDiff(task.lastAssert, now) >= StoneAssertMs)
        {
            BotDriver::GoTo(player, player->GetMapId(), task.stoneX, task.stoneY, task.stoneZ, false);
            task.lastAssert = now;
        }
        return true;
    }
    // There (or the time is up, or a fight came): the task's walk is planned anew from where the bot stands now.
    task.stoneUntil = 0;
    task.walkX = task.walkY = 0.0f;
    task.lastAssert = 0;
    task.route.found = false;
    task.routePlanned = false;
    task.routeBlocked = false;
    task.shortProbed = false;
    task.nextRoutePlan = 0;
    return false;
}

bool OrderService::StepTask(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    uint32 const now = getMSTime();
    BotTask& task = rt.task;

    // On its way to a stepping stone: the task's own step waits.
    if (StepSteppingStone(player, rt))
        return true;
    // A journey waits while a strong creature walks on its next stretch; any task waits after it kept away from one
    // (until it has passed).
    if ((((task.kind == TaskKind::Travel || task.kind == TaskKind::Voyage) && !task.retreat) || task.roamerWaitUntil) &&
        StepRoamerWait(player, rt))
        return true;

    // A task whose place is set aside ends, and whatever chose it does not choose the same spawn again at once: the
    // spawn itself is skipped for a few minutes (a spot without a spawn: a short wait before the next plan).
    auto endForPlace = [&](char const* outcome)
    {
        constexpr uint32 PlaceRetryMs = 5 * MINUTE * IN_MILLISECONDS;
        bool const marked = task.hasSpot && task.spot.spawnId;
        if (marked)
            rt.Avoid(task.spot, PlaceRetryMs);
        ClearTask(rt, marked ? 0 : 5 * IN_MILLISECONDS, outcome);
    };

    // The quest giver or ender stands where the bot died or left a fight meanwhile: not back into it for now.
    if ((task.kind == TaskKind::Pickup || task.kind == TaskKind::TurnIn) && task.hasSpot &&
        rt.IsInAvoidedArea(task.spot.mapId, task.spot.x, task.spot.y, player->GetLevel()))
    {
        endForPlace("place set aside after deaths or a lost fight");
        return false;
    }

    // A far spot of a task: by flight when one is worth it (FindFlight weighs the walks to the flight master and from
    // the landing against the way on foot). An undead warlock walked 7646 yd from Dustwallow Marsh to Melor Stonehoof in
    // Thunder Bluff for the pickup of "Deadmire" in twelve minutes, and flew back for its travel, round 260.
    constexpr float LongWalkYards = 2500.0f;
    if (!task.journeyLooked && task.hasSpot)
    {
        switch (task.kind)
        {
            case TaskKind::Pickup:
            case TaskKind::TurnIn:
            case TaskKind::Vendor:
            case TaskKind::Train:
            case TaskKind::Objective:
            case TaskKind::Interact:
            case TaskKind::Event:
            case TaskKind::Discover:
                task.journeyLooked = true;
                if (task.spot.mapId == player->GetMapId() && Dist2d(player, task.spot) >= LongWalkYards &&
                    StartJourney(bot, player, rt, nullptr, false, true))
                    return true;
                break;
            default:
                break;
        }
    }

    switch (task.kind)
    {
        case TaskKind::Travel:
        {
            // To the anvil or forge next to the hub (the hub's crafting, PlanFocusTrip with a reach): there when the
            // bot stands at it; the hub's time goes on from then.
            if (task.hubFocus)
            {
                if (Professions::FocusNear(player, task.focus))
                {
                    ClearTask(rt, 0, "at the place to craft");
                    rt.nextCraftCheck = 0;
                    rt.nextGearCraft = 0;
                    rt.nextSmeltLook = 0;
                    rt.handInAt = now ? now : 1;
                    return false;
                }
                if (UpdateStuck(player, rt, task.spot.x, task.spot.y))
                {
                    rt.Avoid(task.spot, AvoidSpawnMs);
                    ClearTask(rt, 5 * IN_MILLISECONDS, "place to craft not reached");
                    return false;
                }
                WalkTo(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z);
                return true;
            }
            // A squad leader to a member waiting at its meeting point, the last stretch (UpdateSquad): beside it.
            if (task.fetchSquad && !task.journey)
            {
                constexpr float FetchReached = 15.0f;
                if (Dist2d(player, task.spot) <= FetchReached)
                {
                    ClearTask(rt, 0, "beside its squad member");
                    return false;
                }
                if (UpdateStuck(player, rt, task.spot.x, task.spot.y))
                {
                    ClearTask(rt, 5 * IN_MILLISECONDS, "its squad member not reached");
                    return false;
                }
                WalkTo(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z);
                rt.why = Acore::StringFormat("Squad: fetching {} ({:.0f} yd)", task.targetName, Dist2d(player, task.spot));
                return true;
            }
            // Out of a place too dangerous to stand in (PlanRetreat): to its safe place, whatever stands on the way.
            if (task.retreat)
            {
                constexpr float RetreatReached = 25.0f;
                if (Dist2d(player, task.spot) <= RetreatReached)
                {
                    ClearTask(rt, 10 * IN_MILLISECONDS, "back at a safe place");
                    return false;
                }
                if (UpdateStuck(player, rt, task.spot.x, task.spot.y))
                {
                    ClearTask(rt, 5 * IN_MILLISECONDS, "safe place not reached");
                    return false;
                }
                WalkTo(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z);
                return true;
            }
            // An errand travels to its ender, not to the target zone.
            // (Nor a journey: it travels to the spot of a task, which may lie in the zone the bot is in.)
            ZoneTemplate const* zone = (task.errand || task.journey) ? nullptr : sGuildAdventureMgr.GetZone(rt.targetZone);
            if ((task.errand && Dist2d(player, task.spot) <= ErrandArriveDistance) ||
                (task.journey && Dist2d(player, task.spot) <= JourneyArriveDistance))
            {
                ClearTask(rt, 0, "arrived");
                return false;
            }
            if (zone && player->GetZoneId() == zone->zoneId)
            {
                // One event per arrival in a new target zone, not for every return from a vendor next door.
                if (rt.arrivedZone != zone->zoneId)
                    RecordEvent(guild, "ZONE_CHANGE", bot.guid, Acore::StringFormat("{} arrived in {}.", bot.name, zone->name),
                        player->GetLevel(), zone->zoneId);
                rt.arrivedZone = zone->zoneId;
                rt.travelStuckZone = 0;
                rt.travelStuckCount = 0;
                ClearTask(rt, 0, "arrived");
                return false;
            }
            float const distance = Dist2d(player, task.spot);
            if (distance <= 20.0f)
            {
                // At the hub but not in the zone (hubs lie inside their zone, so this is a border case): no new travel
                // to the same spot every second, it counts as a stuck travel.
                LOG_INFO("module", "mod-guild-adventure: {} reached the hub of {} but is in {}", bot.name,
                    zone ? zone->name : "?", ZoneName(player->GetZoneId()));
                OnTravelStuck(bot, rt, distance);
                return false;
            }
            if (UpdateStuck(player, rt, task.hasFlightVia ? task.flightVia.x : task.spot.x, task.hasFlightVia ? task.flightVia.y : task.spot.y))
            {
                OnTravelStuck(bot, rt, distance);
                return false;
            }
            if (BotDriver::GetRpgState(player) == RpgState::TravelFlight)
            {
                rt.why = Acore::StringFormat("Travelling to {}: on the way to the flight master", task.targetName);
                return true;
            }
            // A flight booked a moment ago: the teleport comes (instant flight paths), no walk from the master meanwhile.
            // (Hordeboss booked the flight from Bloodvenom Post to Orgrimmar, and in the next second its step, seeing no
            // flight status, planned the way on foot, gave the errand up as too far and sent NewRpg off along the road.)
            if (rt.flightBooked && getMSTimeDiff(rt.flightBookedAt, now) <= FlightTeleportWaitMs)
            {
                rt.why = Acore::StringFormat("Travelling to {}: taking the flight", task.targetName);
                return true;
            }
            // A flight: looked for once a minute while walking. TryFlight books it when the flight master is near; a far
            // one becomes the walk's target first (task.hasFlightVia), and the flight is booked next to it.
            // (And when the travel begins, whatever the minute says: the clock is the bot's, not the task's, and a
            // travel that began within a minute of another one's look never looked. A level-28 bot's travel from
            // Duskwood to the Wetlands went straight to a route of 21 557 yd on foot, was refused for what lives on it,
            // and the zone was set aside; a level-34 one in Desolace took a ship of ten minutes for the same reason.)
            if (!task.hasFlightVia && !task.noFlight && (now >= rt.nextFlightCheck || !task.flightTried))
            {
                task.flightTried = true;
                rt.nextFlightCheck = now + FlightCheckMs;
                if (TryFlight(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z))
                {
                    rt.why = Acore::StringFormat("Travelling to {}: on the way to the flight master", task.targetName);
                    return true;
                }
            }
            if (task.hasFlightVia)
            {
                if (!IsAtFlightMaster(player, task.flightVia))
                {
                    WalkTo(player, rt, task.flightVia.mapId, task.flightVia.x, task.flightVia.y, task.flightVia.z);
                    // Not through a group the bot cannot take, not even for a flight (a level-20 mage died to three
                    // level-15 pillagers on its way to the flight master of Sentinel Hill): on foot then, by the checks
                    // below.
                    if (task.route.found && RoadMap::GroupCounts(task.route.crowd, task.route.crowdLevel, int32(player->GetLevel())) &&
                        !Risk::CanFight(player, rt, task.route.crowdLevel, task.route.crowd))
                    {
                        if (WidenTravelStart(player, task, "the flight master"))
                            return true;
                        LOG_INFO(ActivityLog, "activity: {} does not walk to the flight master for {}: {} level-{} creatures {} "
                            "on the way ({})", bot.name, task.targetName, uint32(task.route.crowd), uint32(task.route.crowdLevel),
                            task.route.camp ? "have their camp" : "stand together", task.route.via);
                        task.hasFlightVia = false;
                        task.noFlight = true;
                        task.route = RoadRoute();
                        task.routePlanned = false;
                        return true;
                    }
                    rt.why = Acore::StringFormat("Travelling to {}: walking to the flight master ({})", task.targetName,
                        RoughDistance(Dist2d(player, task.flightVia)));
                    return true;
                }
                rt.nextFlightCheck = now + FlightCheckMs;
                // (A flight taken after the way on foot was refused is judged at the master as it was then: the walks to
                // the master and from the landing do not count against it. Ashmire walked to Gringer in Booty Bay for
                // Grom'gol, 14 yd, was turned down there for "the walks ... are most of the way", and walked on foot
                // into the country its way had been refused for, until it was stuck three minutes later, 2026-11-26.)
                if (TryFlight(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z, task.flightAfterRefusal))
                {
                    rt.why = Acore::StringFormat("Travelling to {}: on the way to the flight master", task.targetName);
                    return true;
                }
                task.hasFlightVia = false; // no flight after all: on foot
            }
            WalkTo(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z);
            // An errand whose way on foot turns out to be a journey (no flight was to be had: no money, no flight
            // master in reach): from Stormwind to Ironforge it is 14 000 yd around the mountains. Not for one hand-in.
            // (From Auberdine to Astranaar it is 5 100 yd along the road, which is what the game asks for.)
            if (task.errand && task.route.found && task.route.length > ErrandMaxRoute)
            {
                LOG_INFO(ActivityLog, "activity: {} does not walk {:.0f} yd to {} for a hand-in ({} yd at most without a flight)", bot.name,
                    task.route.length, task.targetName, uint32(ErrandMaxRoute));
                BotDriver::Rest(player);
                ClearTask(rt, 5 * IN_MILLISECONDS, "errand too far on foot");
                DropErrand(bot, rt, "too far on foot");
                return false;
            }
            // The safest route still leads past creatures far above the bot (no flight got it there either): it does
            // not go. Unless it stands among such creatures already, then any way out is right.
            // (An errand is a walk to an ender: the tighter limit of such walks, so the bot does not travel for a
            // hand-in whose last stretch it then leaves alone.)
            int32 const maxDanger = (task.errand || task.journey) ? sBotSettings.Get(player, Setting::WalksMaxLevelsAbove) : RouteMaxDanger;
            // Nor past a group it cannot take, whatever its level (a level-18 priest died to three level-12 smugglers
            // on an errand's route; the route goes around such groups where it can, RoadMap::FindRoute).
            bool const group = task.route.found && RoadMap::GroupCounts(task.route.crowd, task.route.crowdLevel, int32(player->GetLevel())) &&
                !Risk::CanFight(player, rt, task.route.crowdLevel, task.route.crowd);
            if (group && WidenTravelStart(player, task, task.targetName.c_str()))
                return true;
            if (group || (task.route.found && int32(task.route.danger) - int32(player->GetLevel()) > maxDanger &&
                int32(sRoadMap.DangerAlong(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionX(),
                    player->GetPositionY(), player->GetTeamId())) - int32(player->GetLevel()) <= maxDanger))
            {
                std::string const flight = task.noFlightWhy ? Acore::StringFormat("; no flight: {}", task.noFlightWhy) :
                    task.noFlight ? std::string("; no flight: the way to the flight master was refused") : std::string();
                if (group)
                    LOG_INFO(ActivityLog, "activity: {} does not walk to {}: {} level-{} creatures {} on the safest way ({}){}",
                        bot.name, task.targetName, uint32(task.route.crowd), uint32(task.route.crowdLevel),
                        task.route.camp ? "have their camp" : "stand together", task.route.via, flight);
                else
                    LOG_INFO(ActivityLog, "activity: {} does not walk to {}: the safest way ({}) passes {}{}", bot.name,
                        task.targetName, task.route.via, RoadMap::DangerName(task.route.danger), flight);
                std::string const why = group ? Acore::StringFormat("The way to {} leads through a group of {} level-{} creatures",
                    task.targetName, uint32(task.route.crowd), uint32(task.route.crowdLevel)) :
                    Acore::StringFormat("The way to {} is too dangerous at level {} ({} on it)",
                    task.targetName, uint32(player->GetLevel()), RoadMap::DangerName(task.route.danger));
                // Its way on foot is refused: a flight after all, even where the walks to the master and from the landing are
                // most of the way (they are judged as walks of their own). (A shaman in Feralas was refused the walk to
                // Tanaris for a group on the way, turned the flight down for its walks, and used its hearthstone.)
                if (!task.flightAfterRefusal && !player->GetTransport())
                {
                    task.flightAfterRefusal = true;
                    if (TryFlight(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z, true) || task.hasFlightVia)
                    {
                        LOG_INFO(ActivityLog, "activity: {} takes a flight towards {} after all: its way on foot is refused", bot.name,
                            task.targetName);
                        task.route = RoadRoute();
                        return true;
                    }
                }
                // A travel into a zone whose way passes the zone before the group: it goes as far into the zone as the way is
                // free and works the zone from there. The zone was written off and the bot crossed the sea: a level-14 human
                // in Stormwind was refused Westfall for a camp of seven level-15 creatures at Demont's Place on the way to the
                // lighthouse, past Saldean's Farm and Sentinel Hill, and Loch Modan for a camp on its way, and sailed for
                // Darkshore (2026-11-28, the owner's test).
                if (group && task.kind == TaskKind::Travel && !task.errand && !task.journey && !task.partnerMeet && !task.zoneCut &&
                    rt.targetZone && task.targetName == ZoneName(rt.targetZone) && task.route.found && task.route.crowdDistance > 0.0f)
                {
                    constexpr float GroupMarginYards = 100.0f;
                    RoutePoint const* last = nullptr;
                    for (RoutePoint const& point : task.route.points)
                    {
                        if (Dist2d(player, point.x, point.y) >= task.route.crowdDistance - GroupMarginYards)
                            break;
                        if (player->GetMap()->GetZoneId(PHASEMASK_NORMAL, point.x, point.y, point.z) == rt.targetZone)
                            last = &point;
                    }
                    if (last && Dist2d(player, last->x, last->y) > NearDistance)
                    {
                        LOG_INFO(ActivityLog, "activity: {} travels only as far as ({:.0f}, {:.0f}) in {}: the way on leads through a "
                            "group of {} level-{} creatures", bot.name, last->x, last->y, task.targetName, uint32(task.route.crowd),
                            uint32(task.route.crowdLevel));
                        task.zoneCut = true;
                        task.spot.spawnId = 0;
                        task.spot.x = last->x;
                        task.spot.y = last->y;
                        task.spot.z = last->z;
                        task.route = RoadRoute();
                        return true;
                    }
                }
                bool const errand = task.errand;
                bool const journey = task.journey;
                bool const partnerMeet = task.partnerMeet;
                SpawnPoint const spot = task.spot;
                BotDriver::Rest(player);
                ClearTask(rt, 5 * IN_MILLISECONDS, "route too dangerous");
                if (partnerMeet)
                {
                    // A member travelling after its squad leader: its way failed, nothing else is set aside.
                    rt.squad.meetFailed = true;
                    rt.why = why;
                    return false;
                }
                if (errand)
                    DropErrand(bot, rt, "the way is too dangerous");
                else if (journey)
                {
                    // The spot it was going to waits, as after a refused walk.
                    if (spot.spawnId)
                        rt.Avoid(spot, uint32(sBotSettings.Get(player, Setting::WalksPostponeMinutes)) * MINUTE * IN_MILLISECONDS);
                    else
                        rt.AddAvoidArea(spot.mapId, spot.x, spot.y, player->GetLevel(),
                            uint32(sBotSettings.Get(player, Setting::WalksPostponeMinutes)) * MINUTE * IN_MILLISECONDS, false);
                }
                else if (rt.targetZone)
                {
                    rt.unreachableZones[rt.targetZone] = now + UnreachableZoneMs;
                    rt.zoneChosenAt = 0;
                }
                rt.why = why;
                // No safe way on foot and no flight from here: the zone waits and the autopilot picks another one (three
                // bots used their hearthstone at once in round 124, one of them 600 yd from its inn, while the zone pick
                // had others with as much work). The hearthstone only out of a zone that keeps killing it, or when no
                // zone is left at all (the no-zone hold, after its minutes).
                if (!errand && !journey && KeepsKillingHere(player, rt))
                    HearthHome(player, rt, "no safe way to its zone from where it stood", StrandedHearthYards);
                return false;
            }
            rt.why = Acore::StringFormat("Travelling to {} ({})", task.targetName, RoughDistance(distance));
            return true;
        }
        case TaskKind::Pickup:
        {
            if (player->GetQuestStatus(task.questId) != QUEST_STATUS_NONE)
            {
                ClearTask(rt, 0, "quest in the log");
                return false;
            }
            Quest const* quest = sObjectMgr->GetQuestTemplate(task.questId);
            if (!quest || !IsQuestPickable(bot, player, quest))
            {
                ClearTask(rt, 0, "no longer pickable");
                return false;
            }
            bool done = false;
            if (StepNpcTask(player, rt, done))
                return !LeaveDangerousWalk(bot, player, rt, sBotSettings.Get(player, Setting::WalksMaxLevelsAbove));
            if (!done && !task.unreachable)
            {
                // In sight, but the quest was not taken: a short break (not the 20 minutes of an unreachable NPC).
                OnQuestNpcFailed(guild, bot, player, rt, "not_offered");
                return false;
            }
            if (!done)
            {
                rt.Avoid(task.spot, AvoidSpawnMs);
                LOG_INFO("module", "mod-guild-adventure: {} could not get to {} for quest {} \"{}\"; skipped for {} min",
                    bot.name, task.targetName, task.questId, QuestTitle(task.questId), AvoidSpawnMs / (MINUTE * IN_MILLISECONDS));
                NoteNpcUnreached(guild, bot.name, task);
            }
            ClearTask(rt, done ? 0 : 5 * IN_MILLISECONDS, done ? nullptr : "giver not reached");
            return false;
        }
        case TaskKind::TurnIn:
        {
            if (player->GetQuestStatus(task.questId) != QUEST_STATUS_COMPLETE)
            {
                // At a hub now: what a vendor there can do is done next (GuildAdventure.Vendor.Trips).
                rt.handInAt = now ? now : 1;
                // ... and a hub with an inn becomes its home.
                BindAtHubInn(player, rt.targetZone);
                rt.craftPurchases = 0;
                rt.hubBatches = 0;
                rt.hubGearSteps = 0;
                rt.hubFocusTrips = 0;
                rt.nextCraftCheck = 0;
                rt.nextUpkeepCheck = 0;
                ClearTask(rt, 0, "turned in");
                return false;
            }
            // At the ender for a while and NewRpg's visit has not handed the quest in (a warlock stood at Gakin the
            // Darkbinder for 110 s, "distance 0.0, reached no", and the hand-in was given up): the module hands it in
            // itself, as it serves itself at a vendor. Only face to face: on the ender's floor and in its sight.
            constexpr uint32 TurnInSelfServeMs = 10 * IN_MILLISECONDS;
            if (task.nearSince && now - task.nearSince >= TurnInSelfServeMs)
                if (WorldObject* ender = FindInSight(player, task.spot))
                    if (player->GetDistance(ender) <= INTERACTION_DISTANCE && std::fabs(player->GetPositionZ() - ender->GetPositionZ()) <= 2.5f &&
                        player->IsWithinLOSInMap(ender))
                        if (Quest const* quest = sObjectMgr->GetQuestTemplate(task.questId))
                            if (HandInAt(player, quest, ender))
                            {
                                LOG_INFO(ActivityLog, "activity: {} hands in quest {} \"{}\" at {} itself (the visit had not done it after {} s)",
                                    bot.name, task.questId, QuestTitle(task.questId), task.targetName,
                                    (now - task.nearSince) / IN_MILLISECONDS);
                                return true; // the next step sees the quest gone and ends the task "turned in"
                            }
            bool done = false;
            if (StepNpcTask(player, rt, done))
                return !LeaveDangerousWalk(bot, player, rt, sBotSettings.Get(player, Setting::WalksMaxLevelsAbove));
            if (!done && !task.unreachable)
            {
                OnQuestNpcFailed(guild, bot, player, rt, "cannot_turn_in");
                return false;
            }
            if (!done)
            {
                rt.Avoid(task.spot, AvoidSpawnMs);
                LOG_INFO("module", "mod-guild-adventure: {} could not get to {} to turn in quest {} \"{}\"; skipped for {} min",
                    bot.name, task.targetName, task.questId, QuestTitle(task.questId), AvoidSpawnMs / (MINUTE * IN_MILLISECONDS));
                NoteNpcUnreached(guild, bot.name, task);
            }
            ClearTask(rt, done ? 0 : 5 * IN_MILLISECONDS, done ? nullptr : "ender not reached");
            return false;
        }
        case TaskKind::Discover:
        {
            bool done = false;
            if (StepNpcTask(player, rt, done))
                return !LeaveDangerousWalk(bot, player, rt, sBotSettings.Get(player, Setting::WalksMaxLevelsAbove));
            if (!done)
                rt.Avoid(task.spot, AvoidSpawnMs);
            ClearTask(rt, 0, done ? "learned the flight path" : "flight master not dealt with");
            return false;
        }
        case TaskKind::Vendor:
        case TaskKind::Train:
        {
            bool done = false;
            // Which vendor: the choice of the service point weighs the way there. The walk itself is judged like the
            // walk to a quest giver (a level-8 character died beside a Defias Dockmaster on its way to a vendor); a
            // vendor whose way is refused is left alone for a while, and the next plan takes another one.
            if (StepNpcTask(player, rt, done))
                return !LeaveDangerousWalk(bot, player, rt, sBotSettings.Get(player, Setting::WalksMaxLevelsAbove));
            if (!done)
            {
                LOG_INFO("module", "mod-guild-adventure: {} gave up on {} ({})", bot.name, task.targetName, ToString(task.kind));
                rt.Avoid(task.spot, AvoidSpawnMs);
                if (task.kind == TaskKind::Train)
                    rt.trainCheckedLevel = 0; // try again later, at another trainer
            }
            // A visit that could not pay the repair or found nothing to sell is not repeated every minute.
            if (done && task.repair && MinDurabilityPercent(player) < RepairBelowPercent)
            {
                LOG_INFO("module", "mod-guild-adventure: {} could not pay the repair at {} ({}c); next try in {} min", bot.name,
                    task.targetName, player->GetMoney(), UpkeepRetryMs / (MINUTE * IN_MILLISECONDS));
                rt.nextRepair = now + UpkeepRetryMs;
            }
            // Still full after what playerbots sold (it sells junk only): the module makes room.
            if (done && task.kind == TaskKind::Vendor && player->GetFreeInventorySpace() <= SellAtFreeSlots)
                MakeBagSpace(bot, player);
            // And the squad's members beside it: they follow and visit no vendor of their own, so the leader's visit is
            // theirs (a repair at an armorer, the loot filter's junk sold, room made in their bags). A trip made for a
            // member that frees nothing in its bags is not made again for half an hour (a druid went to a vendor for its
            // member every two minutes, eleven times: the member's bags held worn soulbound gear the visit did not
            // sell).
            if (done && task.kind == TaskKind::Vendor)
            {
                Creature* vendor = task.spot.gameObject ? nullptr : player->FindNearestCreature(task.spot.entry, 30.0f);
                for (ObjectGuid memberGuid : sSquadService.GetMembers(player->GetGUID()))
                {
                    Player* member = vendor ? ObjectAccessor::FindConnectedPlayer(memberGuid) : nullptr;
                    auto memberRt = member ? _runtime.find(memberGuid) : _runtime.end();
                    if (memberRt == _runtime.end() || !member->IsInWorld() || member->GetMapId() != player->GetMapId() ||
                        !member->IsWithinDist(player, 60.0f))
                        continue;
                    memberRt->second.vendorVisit = vendor->GetGUID();
                    memberRt->second.vendorVisitLeader = player->GetGUID();
                    memberRt->second.vendorVisitSince = now ? now : 1;
                    memberRt->second.vendorVisitFree = member->GetFreeInventorySpace();
                }
                // (A trip for its members that frees nothing pauses: StepMemberVendorVisit, when they are done.)
                rt.memberTripFree = std::numeric_limits<uint32>::max();
                // The squad waits for its members at the vendor (StepSquadWait waits for a member that lags behind).
            }
            if (done && task.sell && player->GetFreeInventorySpace() <= SellAtFreeSlots)
                rt.nextSell = now + UpkeepRetryMs;
            rt.nextUpkeepCheck = now + (done ? MINUTE * IN_MILLISECONDS : 5 * MINUTE * IN_MILLISECONDS);
            ClearTask(rt, 0, done ? "visited" : "gave up");
            return false;
        }
        case TaskKind::Objective:
        {
            QuestStatus const status = player->GetQuestStatus(task.questId);
            Quest const* questTemplate = sObjectMgr->GetQuestTemplate(task.questId);
            if (status != QUEST_STATUS_INCOMPLETE || !questTemplate || !NewRpgHelpers::IsQuestWorthDoing(player, questTemplate))
            {
                ClearTask(rt, 0, status == QUEST_STATUS_COMPLETE ? "quest complete" : "quest no longer open or worth doing");
                return false;
            }

            // The place was set aside meanwhile (deaths there, RiskModel): not back into it; another spawn or quest.
            if (task.hasSpot && rt.IsInAvoidedArea(task.spot.mapId, task.spot.x, task.spot.y, player->GetLevel()))
            {
                endForPlace("place set aside after deaths");
                return false;
            }

            // The item it fetched for the objective's interaction (a key): the interaction comes next.
            if (task.fetchItem && player->HasItemCount(task.fetchItem, 1))
            {
                ClearTask(rt, 0, "has the item for its interaction");
                return false;
            }

            if (BotDriver::HasAIGivenUp(player, task.questId))
            {
                FailQuest(guild, bot, player, task.questId, "no_progress");
                ClearTask(rt, 5 * IN_MILLISECONDS, "NewRpg gave up (no progress)");
                return false;
            }

            // Objective done?
            Quest const* quest = sObjectMgr->GetQuestTemplate(task.questId);
            QuestStatusMap& statusMap = player->getQuestStatusMap();
            auto statusItr = statusMap.find(task.questId);
            if (quest && statusItr != statusMap.end())
            {
                QuestStatusData const& progress = statusItr->second;
                bool complete = false;
                if (task.objective >= 0 && task.objective < QUEST_OBJECTIVES_COUNT)
                    complete = progress.CreatureOrGOCount[task.objective] >= quest->RequiredNpcOrGoCount[task.objective];
                else if (task.objective >= QUEST_OBJECTIVES_COUNT && task.objective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
                    complete = progress.ItemCount[task.objective - QUEST_OBJECTIVES_COUNT] >=
                        quest->RequiredItemCount[task.objective - QUEST_OBJECTIVES_COUNT];
                if (complete)
                {
                    ClearTask(rt, 0, "objective done");
                    return false;
                }
            }

            if (task.approach)
            {
                float const distance = Dist2d(player, task.spot);
                if (distance <= (task.approachDone > 0.0f ? task.approachDone : ObjectiveApproachDone))
                {
                    task.approach = false;
                    task.startedAt = now; // NewRpg's part starts now
                    BotDriver::DoQuest(player, quest, task.objective, task.hasSpot, task.spot.x, task.spot.y, task.spot.z);
                    return true;
                }
                if (UpdateStuck(player, rt, task.spot.x, task.spot.y))
                {
                    LOG_INFO("module", "mod-guild-adventure: {} cannot get to the spot of quest {} \"{}\" ({:.0f} yd left)", bot.name,
                        task.questId, QuestTitle(task.questId), distance);
                    rt.AddAvoidArea(task.spot.mapId, task.spot.x, task.spot.y, 255, AvoidSpawnMs, false);
                    rt.Avoid(task.spot, AvoidSpawnMs);
                    NoteSpotUnreached(player, task);
                    ClearTask(rt, 5 * IN_MILLISECONDS, "objective spot not reached");
                    return false;
                }
                WalkTo(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z);
                // A way far around (the spot lies behind a mountain: 8300 yd of route to the naga of the Zoram Strand,
                // 690 yd away from Stonetalon Peak), or past creatures far above the bot: the spot is set aside.
                // Unless the path finder has a straight way there, asked hop by hop: the road map knows the roads, and
                // a spot beside one is often a short walk over open ground. From Nijel's Point in Desolace the Kolkar
                // village lies 870 yd down the slope and 3973 yd by the roads the map knows: a level-33 priest and a
                // level-33 druid had their first quest of the zone set aside for it, again and again.
                constexpr float StraightMaxDetour = 1.6f;
                if (task.route.found && !task.dangerChecked && !task.straightTried &&
                    task.route.length > std::max(ObjectiveMaxRoute, task.route.direct * ObjectiveMaxDetour))
                {
                    task.straightTried = true;
                    float chain = 0.0f;
                    // (Only a way the walk gate lets through: what lives on the straight line, a group on it, a camp.
                    // Of the first three straight ways two ended a second later on "a group on the way".)
                    if (task.spot.mapId == player->GetMapId() && ChainReaches(player, task.spot.x, task.spot.y, task.spot.z, chain) &&
                        chain <= task.route.direct * StraightMaxDetour + 100.0f &&
                        StraightWayIsSafe(player, rt, task.spot.x, task.spot.y, task.spot.z, true))
                    {
                        LOG_INFO(ActivityLog, "activity: {} takes the straight way to {} for quest {} \"{}\": the path finder has one of "
                            "{:.0f} yd ({:.0f} yd straight), the road map's ({}) is {:.0f} yd", bot.name, task.targetName, task.questId,
                            QuestTitle(task.questId), chain, task.route.direct, task.route.via, task.route.length);
                        task.route.found = false;
                        task.routeBlocked = true; // the rest of the task walks without the road map
                        task.walkX = task.walkY = 0.0f;
                    }
                    // Else the whole navmesh, which the path finder's 148 polygons a hop and the grids loaded around the
                    // bot do not see: from Ghost Walker Post in Desolace the Hatefury satyrs lie 1260 yd off and 7000 yd
                    // by the roads; the navmesh walks there in 1375 yd, and a level-33 bot left "Befouled by Satyr"
                    // three times in a row as too far around and went home by hearthstone, round 244.
                    else if (task.spot.mapId == player->GetMapId() && !player->GetMap()->Instanceable() &&
                        task.OpenTryDue(player->GetPositionX(), player->GetPositionY()))
                    {
                        float length = 0.0f;
                        std::vector<NavPoint> corners;
                        // The way's own legs are judged as the straight way is (StraightWayIsSafe): what lives on them, a
                        // group, a camp, a place set aside; not the straight line, which may lead elsewhere.
                        std::string unsafe;
                        if (sDungeonNav.FindWay(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                                task.spot.x, task.spot.y, task.spot.z, &corners, &length, player->GetMap()) &&
                            length <= std::min(4000.0f, task.route.direct * StraightMaxDetour + 100.0f) && !NavWayIsSafe(player, rt, corners, unsafe))
                            LOG_INFO(ActivityLog, "activity: {} does not take the navmesh's way to {} for quest {} \"{}\" ({:.0f} yd, {:.0f} yd "
                                "straight): {}", bot.name, task.targetName, task.questId, QuestTitle(task.questId), length, task.route.direct,
                                unsafe);
                        else if (!corners.empty() && length <= std::min(4000.0f, task.route.direct * StraightMaxDetour + 100.0f))
                        {
                            LOG_INFO(ActivityLog, "activity: {} takes the navmesh's way to {} for quest {} \"{}\": {:.0f} yd ({:.0f} yd "
                                "straight), the road map's ({}) is {:.0f} yd", bot.name, task.targetName, task.questId,
                                QuestTitle(task.questId), length, task.route.direct, task.route.via, task.route.length);
                            task.openWay = true;
                            task.navActive = false;
                            task.ownSteps = false;
                            task.stallSince = 0;
                            task.routePlanned = false;
                            task.route.found = false;
                            task.walkX = task.walkY = 0.0f;
                            rt.stuckSince = 0;
                        }
                    }
                }
                if (task.route.found && !task.dangerChecked &&
                    task.route.length > std::max(ObjectiveMaxRoute, task.route.direct * ObjectiveMaxDetour))
                {
                    // By flight, when one leads near the spot: from Silithus the targets of "Savage Flora" lie 800 yd off,
                    // down in Un'Goro, the road 5000 to 7000 yd long, and the flight from Cenarion Hold lands in the crater.
                    char const* noFlight = nullptr;
                    if (StartJourney(bot, player, rt, &noFlight, true))
                        return true;
                    LOG_INFO(ActivityLog, "activity: {} does not walk to {} for quest {} \"{}\": the way ({}) is {:.0f} yd for {:.0f} yd "
                        "straight; set aside for {} min{}", bot.name, task.targetName, task.questId, QuestTitle(task.questId),
                        task.route.via, task.route.length, task.route.direct, AvoidSpawnMs / (MINUTE * IN_MILLISECONDS),
                        noFlight ? Acore::StringFormat("; no flight: {}", noFlight) : std::string());
                    rt.AddAvoidArea(task.spot.mapId, task.spot.x, task.spot.y, 255, AvoidSpawnMs, false);
                    rt.Avoid(task.spot, AvoidSpawnMs);
                    BotDriver::Rest(player);
                    // Another spawn at once: the quest is not held against it, a long way is no danger. But not one spawn
                    // after the other for ever: the targets beyond one cliff all lie at the end of the same long way (a
                    // priest in Silithus began and ended "Savage Flora" every 2 to 3 s, a spawn of Un'Goro farther each
                    // time, round 157). The third in a row within ten minutes sets the quest aside for a quarter of an hour.
                    constexpr uint8 FarAroundLimit = 3;
                    constexpr uint32 FarAroundWindowMs = 10 * MINUTE * IN_MILLISECONDS;
                    BotRuntime::FarAround& around = rt.farAroundQuests[task.questId];
                    if (!around.at || getMSTimeDiff(around.at, now) > FarAroundWindowMs)
                        around.count = 0;
                    around.at = now;
                    if (++around.count >= FarAroundLimit)
                    {
                        LOG_INFO(ActivityLog, "activity: {} leaves quest {} \"{}\" for a quarter of an hour: its targets lie {} times in a "
                            "row at the end of a far way around", bot.name, task.questId, QuestTitle(task.questId), uint32(around.count));
                        BotRuntime::Danger& danger = rt.dangerousQuests[task.questId];
                        danger.level = player->GetLevel();
                        danger.until = now + SpotRetryMs;
                        rt.farAroundQuests.erase(task.questId);
                    }
                    ClearTask(rt, IN_MILLISECONDS, "objective too far around");
                    return false;
                }
                // Past creatures two levels above the bot on the way there (what lives at the spot itself is the
                // objective's business): another objective first.
                SpawnPoint const spot = task.spot;
                if (LeaveDangerousWalk(bot, player, rt, sBotSettings.Get(player, Setting::WalksMaxLevelsAbove), true))
                {
                    rt.AddAvoidArea(spot.mapId, spot.x, spot.y, 255, uint32(sBotSettings.Get(player, Setting::WalksPostponeMinutes)) * MINUTE * IN_MILLISECONDS, false);
                    return false;
                }
                return true;
            }

            RpgState const state = BotDriver::GetRpgState(player);
            // Back from a trip for air (CheckBreath had NewRpg rest): on with the objective, it did not leave it.
            if (rt.resumeAfterAir)
            {
                rt.resumeAfterAir = false;
                if (state != RpgState::DoQuest || BotDriver::GetRpgQuest(player) != task.questId)
                {
                    BotDriver::DoQuest(player, quest, task.objective, task.hasSpot, task.spot.x, task.spot.y, task.spot.z);
                    return true;
                }
            }
            // (Not while it walks to the loot of its kill: that walk is a NewRpg status of its own, StepObjectiveLoot.)
            if (task.lootCorpse.IsEmpty() && (state != RpgState::DoQuest || BotDriver::GetRpgQuest(player) != task.questId))
            {
                // NewRpg left the quest (it found no spot, or ran into its own time limit). Re-plan; repeated empty
                // attempts end in NewRpg's no-progress rule or the planner's stuck rule.
                if (now - task.startedAt < AssertGapMs)
                {
                    BotDriver::DoQuest(player, quest, task.objective, task.hasSpot, task.spot.x, task.spot.y, task.spot.z);
                    return true;
                }
                // The same quest left again and again within seconds (NewRpg has no spot for it, or cannot start from
                // where the bot stands): the planner would give it the same task for ever. Its spot is set aside like
                // one the bot could not get to.
                constexpr uint32 LeftWindowMs = MINUTE * IN_MILLISECONDS;
                constexpr uint8 LeftLimit = 4;
                if (rt.leftQuest != task.questId || now - rt.leftAt > LeftWindowMs)
                {
                    rt.leftQuest = task.questId;
                    rt.leftCount = 0;
                }
                rt.leftAt = now;
                if (++rt.leftCount >= LeftLimit)
                {
                    rt.leftCount = 0;
                    LOG_INFO(ActivityLog, "activity: {} gets nowhere with quest {} \"{}\" at {} (NewRpg leaves it at once, {} times); "
                        "set aside for {} min", bot.name, task.questId, QuestTitle(task.questId), task.targetName, uint32(LeftLimit),
                        AvoidSpawnMs / (MINUTE * IN_MILLISECONDS));
                    rt.AddAvoidArea(task.spot.mapId, task.spot.x, task.spot.y, 255, AvoidSpawnMs, false);
                    if (task.hasSpot)
                        rt.Avoid(task.spot, AvoidSpawnMs);
                    ClearTask(rt, 5 * IN_MILLISECONDS, "NewRpg left the quest again and again");
                    return false;
                }
                ClearTask(rt, 0, "NewRpg left the quest");
                return false;
            }

            // On the way to the objective's spot the planner's stuck rules apply (NewRpg would teleport there instead).
            // (Also to "the quest area", which has no spawn and so no "spot" for the rest of the planner: NewRpg walks to
            // the quest's point itself. A priest stood five minutes 256 yd from the area of "Vyral the Vile" in Silithus,
            // NewRpg "doing" the quest without a step, and none of these rules looked at it, round 156.)
            if ((task.hasSpot || task.questArea) && !task.nearSince)
            {
                float const distance = Dist2d(player, task.spot);
                // The straight walk to the spot (under the routes' distance NewRpg walks it as the crow flies): not
                // through a group the bot cannot take. The walks of routes had this check, these had none: a level-17
                // warrior walked 211 yd to its Defias Pillagers through the Riverpaw camp of Westfall, four gnolls at
                // one fire, and died there, as others had before it. What stands at the spot itself is the pull's
                // matter, and the group the bot stands beside when it sets off is behind it.
                constexpr float DirectCrowdStart = 40.0f;
                constexpr float DirectCrowdEnd = 45.0f;
                if (!task.crowdChecked && !task.approach && distance > DirectCrowdStart + DirectCrowdEnd + 20.0f &&
                    sBotSettings.IsOn(player, Setting::RoadsEnable))
                {
                    task.crowdChecked = true;
                    float const t = (distance - DirectCrowdEnd) / distance;
                    uint8 crowdLevel = 0;
                    uint8 const crowd = sRoadMap.CrowdAlong(player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
                        player->GetPositionX() + (task.spot.x - player->GetPositionX()) * t,
                        player->GetPositionY() + (task.spot.y - player->GetPositionY()) * t, player->GetTeamId(), crowdLevel,
                        player->GetPositionZ(), player->GetPositionZ() + (task.spot.z - player->GetPositionZ()) * t, DirectCrowdStart);
                    int32 const level = player->GetLevel();
                    if (RoadMap::GroupCounts(crowd, crowdLevel, level) &&
                        !Risk::CanFight(player, rt, crowdLevel, crowd))
                    {
                        uint32 const minutes = uint32(sBotSettings.Get(player, Setting::WalksPostponeMinutes));
                        LOG_INFO(ActivityLog, "activity: {} does not walk to {} (objective, quest {} \"{}\"): {} level-{} creatures stand "
                            "together on the straight way of {:.0f} yd (it is level {}); left alone for {} min", bot.name, task.targetName,
                            task.questId, QuestTitle(task.questId), uint32(crowd), uint32(crowdLevel), distance, level, minutes);
                        std::string const why = Acore::StringFormat("{} is behind a group of {} level-{} creatures: later", task.targetName,
                            uint32(crowd), uint32(crowdLevel));
                        rt.Avoid(task.spot, minutes * MINUTE * IN_MILLISECONDS);
                        rt.gateRecheck = true;
                        BotDriver::Rest(player);
                        ClearTask(rt, IN_MILLISECONDS, "a group on the way");
                        rt.why = why;
                        return false;
                    }
                }
                // NewRpg holds the quest and does not set off (a warlock sat on its mount at Whelgar's Excavation for
                // five minutes, "movement 0; rpg do quest", through two objectives, until the rescue): after 12 s
                // within a few yards the planner walks the bot to the spot itself and hands the quest back in sight
                // of it. Once per task; after that the stuck rule ends the task as before. (It was 25 s: in an hour of
                // the integration server, 2026-11-26, 41 objectives waited 26 s on average before it, 18 minutes of
                // standing; a walk of the planner's own that NewRpg does not get on with is taken over after 10 s.)
                constexpr uint32 NoWalkMs = 12 * IN_MILLISECONDS;
                // (And at once when the spot lies on another level that no path of the path finder reaches: NewRpg walks
                // the mesa of Freewind Post round and round over its quest targets in the canyon 130 yd below, and the
                // lift down is the road map's; the guild leader stood three minutes there before each such task ended,
                // 2026-11-25.)
                constexpr uint32 LevelLookMs = 5 * IN_MILLISECONDS;
                constexpr float OtherLevelHeight = 20.0f;
                if (!task.levelLooked && !task.tookOver && distance > SightDistance && getMSTimeDiff(task.startedAt, now) >= LevelLookMs &&
                    std::fabs(task.spot.z - player->GetPositionZ()) > OtherLevelHeight && !player->GetMap()->IsDungeon())
                {
                    task.levelLooked = true;
                    task.otherLevel = !PathEndsAt(player, task.spot.x, task.spot.y, task.spot.z);
                }
                bool const otherLevel = task.otherLevel;
                bool const stood = rt.strandSince && getMSTimeDiff(rt.strandSince, now) >= NoWalkMs &&
                    getMSTimeDiff(task.startedAt, now) >= NoWalkMs;
                if (distance > SightDistance && !task.tookOver && (stood || otherLevel) && player->IsAlive() && !player->IsInCombat() &&
                    !player->IsNonMeleeSpellCast(false))
                {
                    if (otherLevel && !stood)
                        LOG_INFO(ActivityLog, "activity: {} has the spot of quest {} \"{}\" {:.0f} yd away and {:.0f} yd {}, where no path "
                            "leads: the planner walks it there itself", bot.name, task.questId, QuestTitle(task.questId), distance,
                            std::fabs(task.spot.z - player->GetPositionZ()), task.spot.z < player->GetPositionZ() ? "below it" : "above it");
                    else
                        LOG_INFO(ActivityLog, "activity: {} has not set off for {} s, {:.0f} yd from the spot of quest {} \"{}\": the planner "
                            "walks it there itself", bot.name, getMSTimeDiff(rt.strandSince, now) / IN_MILLISECONDS, distance, task.questId,
                            QuestTitle(task.questId));
                    task.tookOver = true;
                    task.ownSteps = true;
                    task.approach = true;
                    task.approachDone = SightDistance;
                    task.routePlanned = false;
                    rt.stuckSince = 0;
                    WalkTo(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z);
                    return true;
                }
                if (distance <= SightDistance)
                    task.nearSince = now;
                else if (UpdateStuck(player, rt, task.spot.x, task.spot.y))
                {
                    LOG_INFO("module", "mod-guild-adventure: {} cannot get to the spot of quest {} \"{}\" ({:.0f} yd left)", bot.name,
                        task.questId, QuestTitle(task.questId), distance);
                    rt.AddAvoidArea(task.spot.mapId, task.spot.x, task.spot.y, 255, AvoidSpawnMs, false);
                    rt.Avoid(task.spot, AvoidSpawnMs);
                    NoteSpotUnreached(player, task);
                    ClearTask(rt, 5 * IN_MILLISECONDS, "objective spot not reached");
                    return false;
                }
            }

            // Kills that bring nothing: an item objective worked at creatures for many fights without one piece. Every
            // fight is "progress" to the watchdog below, so such an objective never ended (a paladin held one for nine
            // minutes and 25 fights, and took no other quest meanwhile). The quest is set aside for half an hour. How
            // many fights: three times what one piece takes by the source's chance, ten at least, forty at most.
            if (quest && task.hasSpot && !task.spot.gameObject && task.objective >= QUEST_OBJECTIVES_COUNT && statusItr != statusMap.end())
            {
                int32 const index = std::min<int32>(task.objective - QUEST_OBJECTIVES_COUNT, QUEST_ITEM_OBJECTIVES_COUNT - 1);
                uint32 const have = statusItr->second.ItemCount[index];
                BotRuntime::DryKills& dry = rt.dryKills[task.questId];
                if (dry.objective != task.objective || dry.have != have)
                {
                    dry.objective = task.objective;
                    dry.have = have;
                    dry.fights = 0;
                }
                uint32 const fights = rt.total.fights + 1; // (+1: 0 is "not looked yet")
                if (task.fightsSeen && fights > task.fightsSeen)
                    dry.fights += fights - task.fightsSeen;
                task.fightsSeen = fights;
                float chance = sWorldIndex.GetItemSourceChance(quest->RequiredItemId[index], task.spot.entry, false);
                if (chance <= 0.0f)
                    chance = 15.0f;
                uint32 const limit = std::clamp<uint32>(uint32(300.0f / chance), 10, 40);
                if (dry.fights >= limit && !player->IsInCombat())
                {
                    constexpr uint32 DryAsideMs = 30 * MINUTE * IN_MILLISECONDS;
                    ItemTemplate const* wanted = sObjectMgr->GetItemTemplate(quest->RequiredItemId[index]);
                    LOG_INFO(ActivityLog, "activity: {} sets quest {} \"{}\" aside for {} min: {} fights at {} and its like without one "
                        "{} (the chance is about {:.0f}%)", bot.name, task.questId, QuestTitle(task.questId),
                        DryAsideMs / (MINUTE * IN_MILLISECONDS), dry.fights, task.targetName, wanted ? wanted->Name1 : "piece", chance);
                    BotRuntime::Danger& danger = rt.dangerousQuests[task.questId];
                    danger.level = uint8(player->GetLevel() + 1);
                    danger.until = now + DryAsideMs;
                    rt.dryKills.erase(task.questId);
                    BotDriver::Rest(player);
                    ClearTask(rt, 0, "kills without the item");
                    return false;
                }
            }

            // At the spawn with nothing of the objective in sight (its creature is dead, its object taken): on to the
            // next spawn, this one is left alone for about a respawn.
            // (Counted from close to the spawn: its creature roams, and from the edge of sight the far side of its round
            // is out of reach of the look.)
            // (The look at the targets also for a bot that stopped farther from the spawn, once it has stood half a minute
            // within sight of it: Velmira, a level-22 priest, stood three minutes 48 and 53 yd from a Bristleback
            // Thornweaver's spawn in the Barrens, twice, while its AI turned to the Bristleback Hunters around it that its
            // set-aside quest wanted and the check before a pull let them go each time; 15 of the 20 spawns a bot gave up
            // that day for "no progress" it had stood 36 to 60 yd from. The count of an empty spawn stays a matter of
            // standing close to it.) (5 s, was 30: a bot that had killed its target 35 yd or more from the spawn it was sent to
            // stood up to half a minute before it was turned to the next one in sight, which players took for waiting at the
            // dead one's spawn for its respawn: undead bots in Deathknell on "Rattling the Rattlecages", Duskbats 52-67 yd
            // off, 2026-11-28.)
            bool const closeToSpawn = task.hasSpot && Dist2d(player, task.spot) <= ObjectiveEmptyReach / 2.0f;
            if (task.hasSpot && task.nearSince && task.spot.entry && !player->IsInCombat() && !rt.rest.active && player->IsAlive() &&
                (closeToSpawn || now - task.nearSince >= 5 * IN_MILLISECONDS))
            {
                // A target the bot cannot see (a Shadowmaw Panther prowls in stealth): NewRpg finds nothing to attack, and
                // the bot stood beside it until the watchdog's three minutes were over (402 s and 212 s on "Panther
                // Mastery"; the fights in between were panthers that had found the bot). The planner walks the bot onto
                // the nearest one: a few yards in front of it the bot sees it, or the creature attacks first.
                constexpr uint32 StalkGapMs = 2 * IN_MILLISECONDS;
                // (10 s after the last progress, was 30: between two kills of "Identifying the Brood" a warlock of the
                // Horde test guild stood 40 s with Searing Hatchlings in sight, its AI picking none, 2026-11-27; 5 s since
                // a warlock of 12 stood 10 to 19 s before every Windfury Wind Witch 56 to 62 yd off, 217 s in five minutes,
                // the same night.)
                constexpr uint32 IdleTargetMs = 5 * IN_MILLISECONDS;
                if (!task.spot.gameObject && now >= task.nextStalk && !player->IsNonMeleeSpellCast(false))
                {
                    task.nextStalk = now + StalkGapMs;
                    std::list<Creature*> kind;
                    player->GetCreatureListWithEntryInGrid(kind, task.spot.entry, ObjectiveEmptyReach);
                    Creature* hidden = nullptr;
                    Creature* seen = nullptr;
                    std::vector<Creature*> hiding;
                    for (Creature* creature : kind)
                    {
                        if (!creature->IsAlive())
                            continue;
                        if (!player->CanSeeOrDetect(creature))
                        {
                            if (creature->HasStealthAura())
                                hiding.push_back(creature);
                            continue;
                        }
                        if (!seen || player->GetDistance(creature) < player->GetDistance(seen))
                            seen = creature;
                    }
                    // The walk onto a hidden one is a straight one, so only to one that stands where the bot may fight:
                    // the spot verdict at its place now and no camp on the way. (Frostvein, a level-35 mage, was walked
                    // onto a Syndicate Highwayman in stealth for "Foul Magics" in Arathi, into the Syndicate's camp; it
                    // won that fight, three Mercenaries beside it attacked it on its way out and it died, 2026-11-26.)
                    std::sort(hiding.begin(), hiding.end(), [player](Creature const* a, Creature const* b)
                    {
                        return player->GetDistance(a) < player->GetDistance(b);
                    });
                    for (Creature* creature : hiding)
                    {
                        if (!IsPlaceUsable(player, rt, creature->GetMapId(), creature->GetPositionX(), creature->GetPositionY(),
                                creature->GetPositionZ()) ||
                            Risk::EntersCamp(player, rt, creature->GetMapId(), player->GetPositionX(), player->GetPositionY(),
                                creature->GetPositionX(), creature->GetPositionY()))
                        {
                            if (!task.stalkRefused)
                            {
                                task.stalkRefused = true;
                                LOG_INFO(ActivityLog, "activity: {} does not walk onto {} in stealth ({:.0f} yd away): it stands where "
                                    "the bot may not fight", bot.name, creature->GetName(), player->GetDistance(creature));
                            }
                            continue;
                        }
                        hidden = creature;
                        break;
                    }
                    if (hidden && !seen)
                    {
                        if (!task.stalking)
                        {
                            task.stalking = true;
                            LOG_INFO(ActivityLog, "activity: {} cannot see {} (it is in stealth, {:.0f} yd away): the planner walks it onto "
                                "the creature", bot.name, hidden->GetName(), player->GetDistance(hidden));
                        }
                        BotDriver::PauseAI(player, StalkGapMs + 500);
                        player->GetMotionMaster()->MovePoint(0, hidden->GetPositionX(), hidden->GetPositionY(), hidden->GetPositionZ());
                        rt.why = Acore::StringFormat("Looking for {} (it hides) for \"{}\"", task.targetName, QuestTitle(task.questId));
                    }
                    else if (task.stalking && seen)
                    {
                        // In sight now: the bot's AI is turned to it, as when it picks a target itself.
                        task.stalking = false;
                        BotDriver::Attack(player, seen);
                    }
                    // In sight all along, and the bot does not take it: half a minute beside a living target without a
                    // fight (a warrior stood 317 s 25 yd from Sin'Dall on its mound for "Tiger Mastery", another 170 s
                    // beside a Snapjaw Crocolisk in the river, until NewRpg or the watchdog gave the spawn up). The
                    // planner turns the bot's AI to it, as the test aid `.ga admin attack` does; the check before a
                    // pull still judges that target.
                    // (Counted from the bot's first look at the spawn from close by, so its own choice comes first.)
                    else if (seen && !task.nextNudge)
                        task.nextNudge = now + IdleTargetMs;
                    else if (seen && task.progressAt && now - task.progressAt >= IdleTargetMs && now >= task.nextNudge &&
                        !player->GetVictim())
                    {
                        task.nextNudge = now + IdleTargetMs;
                        if (BotDriver::Attack(player, seen))
                            LOG_INFO(ActivityLog, "activity: {} has not fought for {} s beside {} ({:.0f} yd away): the planner turns "
                                "it to the creature", bot.name, (now - task.progressAt) / IN_MILLISECONDS, seen->GetName(),
                                player->GetDistance(seen));
                    }
                }
                bool const there = task.spot.gameObject ?
                    player->FindNearestGameObject(task.spot.entry, ObjectiveEmptyReach, true) != nullptr :
                    player->FindNearestCreature(task.spot.entry, ObjectiveEmptyReach, true) != nullptr;
                if (there || !closeToSpawn)
                    task.emptySince = 0;
                else if (!task.emptySince)
                    task.emptySince = now;
                else if (now - task.emptySince >= (IsSpawnDown(player, task.spot) ? ObjectiveDownMs : ObjectiveEmptyMs))
                {
                    uint32 const questId = task.questId;
                    rt.Avoid(task.spot, ObjectiveRespawnMs);
                    if (++rt.emptySpawns[questId] >= ObjectiveEmptySpawns)
                    {
                        LOG_INFO("module", "mod-guild-adventure: {} found no target at {} spawns of quest {} \"{}\" in a row", bot.name,
                            uint32(ObjectiveEmptySpawns), questId, QuestTitle(questId));
                        rt.emptySpawns.erase(questId);
                        FailQuest(guild, bot, player, questId, "no_progress");
                        ClearTask(rt, 5 * IN_MILLISECONDS, "no target at several spawns");
                        return false;
                    }
                    // What the map knows of the spawn (for the planner's look at spawns that are down).
                    if (GetSettings().debug)
                    {
                        std::string state = "not on the map";
                        if (task.spot.gameObject)
                        {
                            auto const bounds = player->GetMap()->GetGameObjectBySpawnIdStore().equal_range(task.spot.spawnId);
                            if (bounds.first != bounds.second)
                                state = Acore::StringFormat("object {}, respawn in {} s", bounds.first->second->isSpawned() ? "there" : "gone",
                                    int64(bounds.first->second->GetRespawnTime()) - int64(GameTime::GetGameTime().count()));
                        }
                        else
                        {
                            auto const bounds = player->GetMap()->GetCreatureBySpawnIdStore().equal_range(task.spot.spawnId);
                            if (bounds.first != bounds.second)
                                state = Acore::StringFormat("creature {} (entry {}), {:.0f} yd from its spawn, respawn in {} s",
                                    bounds.first->second->IsAlive() ? "alive" : "dead", bounds.first->second->GetEntry(),
                                    bounds.first->second->GetDistance2d(task.spot.x, task.spot.y),
                                    int64(bounds.first->second->GetRespawnTime()) - int64(GameTime::GetGameTime().count()));
                        }
                        LOG_INFO(ActivityLog, "activity: {} found no {} at spawn {} of quest {}: {}", bot.name, task.targetName, task.spot.spawnId,
                            questId, state);
                    }
                    ClearTask(rt, 0, "no target at this spawn");
                    return false;
                }
            }
            // The objective's item in a corpse of its kill first: the look at the spawn below moved the bot on 3 s
            // after a fight, and a caster or a hunter that killed at range left the item in the corpse beyond auto
            // loot's 10 yd (Durgash, a warlock, 2026-11-26: "10 fights at Bristleback Hunter and its like without one
            // Bristleback Quilboar Tusk", a drop of 80%; Vozjin, a hunter, and Mahiri, a shaman, the same at drops of
            // 80%). The walk is a NewRpg status of its own: the objective's status is set again after it.
            if (!task.spot.gameObject && quest && task.objective >= QUEST_OBJECTIVES_COUNT &&
                task.objective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT && !player->IsInCombat() && !rt.rest.active &&
                player->IsAlive() && !player->GetVictim())
            {
                bool const walking = !task.lootCorpse.IsEmpty();
                if (StepObjectiveLoot(player, task, quest->RequiredItemId[task.objective - QUEST_OBJECTIVES_COUNT]))
                {
                    task.busyAt = now;
                    return true;
                }
                if (walking)
                    BotDriver::DoQuest(player, quest, task.objective, task.hasSpot, task.spot.x, task.spot.y, task.spot.z);
            }
            // Watchdog: no kill, no fight, no item and no rest at this spawn for minutes (an object that is not there,
            // a spot the bot stands near but cannot get to): skip the spawn, the planner takes another one.
            if (player->IsInCombat() || rt.rest.active)
                task.busyAt = now;
            if (task.hasSpot)
            {
                // (At an object the fights around it are no progress: a bot stood beside a Wyrmtail it did not open and
                // fought what came, for nine minutes.)
                uint32 mark = task.spot.gameObject ? 0 : rt.total.fights + rt.total.rests + rt.total.deaths;
                if (statusItr != statusMap.end() && task.objective >= 0)
                    mark += task.objective < QUEST_OBJECTIVES_COUNT ? statusItr->second.CreatureOrGOCount[task.objective] :
                        statusItr->second.ItemCount[std::min<int32>(task.objective - QUEST_OBJECTIVES_COUNT, QUEST_ITEM_OBJECTIVES_COUNT - 1)];
                if (!task.progressAt)
                {
                    task.progressAt = now;
                    task.progressMark = mark;
                }
                else if (mark != task.progressMark)
                {
                    task.progressAt = now;
                    task.progressMark = mark;
                    rt.idleSpawns.erase(task.questId);
                    rt.emptySpawns.erase(task.questId);
                }
                // Nothing of the objective near: on to the next spawn at once, so the bot walks the objective's area
                // spawn by spawn. NewRpg looks for targets by walking to and fro around the place it was given, and once
                // that took the bot beyond the look at the spawn above (ObjectiveEmptyReach) only the watchdog's three
                // minutes ended it (the owner, 2026-11-14: [quote left out], and: [quote left out]). Looked at once it has been at the spawn, and
                // ObjectiveSettleMs after each fight and rest. A creature that is dead does not count; the spawn left
                // is not chosen again for ObjectiveRespawnMs.
                // (The look reaches as far down or up as the spot lies: a bot over a Drysnap Crawler on the sea floor 47 yd
                // below it saw "nothing within 45 yd" and left the spawn, Clam Bait on the j5 instance, 2026-11-26.)
                // (And at least as far as the spawn itself lies, with 15 yd beyond it: the look begins once the bot is within
                // SightDistance of the spawn, 60 yd, and Kodahorn (33), 55 yd before Khan Dez'hepah, saw "nothing of quest
                // 1365 within 45 yd" and turned to another quest, while its AI went on to Khan and his two guards killed it,
                // round 257.)
                else if (!task.spot.gameObject && task.nearSince && now - task.busyAt >= ObjectiveSettleMs && !player->IsInCombat() &&
                    !rt.rest.active && player->IsAlive() && !player->GetVictim() && !player->IsNonMeleeSpellCast(false) &&
                    !NeedNear(player, quest, task.objective, task.spot.entry, std::min(80.0f, std::max(ObjectiveNearYards,
                        Dist2d(player, task.spot) + 15.0f)) + std::min(60.0f, std::fabs(task.spot.z - player->GetPositionZ()))))
                {
                    rt.Avoid(task.spot, ObjectiveRespawnMs);
                    LOG_INFO(ActivityLog, "activity: {} sees nothing of quest {} \"{}\" within {:.0f} yd: on to the next spawn",
                        bot.name, task.questId, QuestTitle(task.questId), std::min(80.0f, std::max(ObjectiveNearYards,
                        Dist2d(player, task.spot) + 15.0f)) + std::min(60.0f, std::fabs(task.spot.z - player->GetPositionZ())));
                    ClearTask(rt, 0, "nothing near: next spawn");
                    return false;
                }
                else if (now - task.progressAt >= ObjectiveIdleMs)
                {
                    uint32 const questId = task.questId;
                    rt.Avoid(task.spot, AvoidSpawnMs);
                    if (++rt.idleSpawns[questId] >= ObjectiveIdleSpawns)
                    {
                        LOG_INFO("module", "mod-guild-adventure: {} made no progress at {} spawns of quest {} \"{}\" in a row", bot.name,
                            uint32(ObjectiveIdleSpawns), questId, QuestTitle(questId));
                        rt.idleSpawns.erase(questId);
                        FailQuest(guild, bot, player, questId, "no_progress");
                        ClearTask(rt, 5 * IN_MILLISECONDS, "no progress at several spawns");
                        return false;
                    }
                    LOG_INFO("module", "mod-guild-adventure: {} makes no progress at a spawn of quest {} \"{}\" ({}, {:.0f} yd away); "
                        "trying another one", bot.name, questId, QuestTitle(questId), task.targetName, Dist2d(player, task.spot));
                    ClearTask(rt, 0, "no progress at this spawn");
                    return false;
                }
            }
            return true;
        }
        case TaskKind::Interact:
            // The place was set aside meanwhile (deaths there, RiskModel): not back into it; another spawn or quest. Not for
            // a use where the bot stands (combine pages): its spot is the bot itself, so the planner chose it again at once
            // (Sunwhisper began and ended "The Lost Pages" every 6 s for half an hour, 2026-11-25).
            if (task.hasSpot && !task.plan.anywhere &&
                rt.IsInAvoidedArea(task.spot.mapId, task.spot.x, task.spot.y, player->GetLevel()))
            {
                endForPlace("place set aside after deaths");
                return false;
            }
            return StepInteraction(guild, bot, player, rt);
        case TaskKind::Fish:
            return StepFishing(guild, bot, player, rt);
        case TaskKind::Grind:
        {
            RpgState const state = BotDriver::GetRpgState(player);
            if (now - task.startedAt > GrindMaxMs)
            {
                ClearTask(rt, 0, "grind time over");
                return false;
            }
            // A hunt for a material ends when the bags and the guild storage hold what the recipe lacked.
            if (task.huntItem && !player->IsInCombat())
            {
                uint32 const owner = sStorageService.GetOwnerOf(player);
                if (player->GetItemCount(task.huntItem) + (owner ? sStorageService.GetAmount(owner, task.huntItem, 0) : 0) >= task.huntCount)
                {
                    ClearTask(rt, 0, "material gathered");
                    rt.nextCraftCheck = 0;
                    return false;
                }
            }
            // The place was set aside meanwhile (a death, a fight left): not on there.
            if (rt.IsInAvoidedArea(task.spot.mapId, task.spot.x, task.spot.y, player->GetLevel()))
            {
                ClearTask(rt, 0, "grind spot set aside");
                return false;
            }
            float const distance = Dist2d(player, task.spot);
            if (!task.nearSince)
            {
                if (distance <= SightDistance)
                    task.nearSince = now;
                else
                {
                    if (UpdateStuck(player, rt, task.spot.x, task.spot.y))
                    {
                        LOG_INFO("module", "mod-guild-adventure: {} cannot get to the grind spot at {} ({:.0f} yd left)", bot.name,
                            task.targetName, distance);
                        rt.AddAvoidArea(task.spot.mapId, task.spot.x, task.spot.y, 255, AvoidSpawnMs, false);
                        ClearTask(rt, 5 * IN_MILLISECONDS, "grind spot not reached");
                        return false;
                    }
                    WalkTo(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z);
                    return true;
                }
            }
            // At the spot the planner picks what is fought: nothing above the level the risk rules allow, nothing
            // far below the bot, nearest first.
            // (A hunt for skins names its levels itself: what the skinning skill is enough for.)
            int32 const level = player->GetLevel();
            int32 maxFoe = task.grindMax ? int32(task.grindMax) : level + sBotSettings.Get(player, Setting::GrindLevelsAbove);
            while (maxFoe > 1 && !Risk::CanFight(player, rt, uint8(std::min(maxFoe, 255))))
                --maxFoe;
            int32 const minFoe = task.grindMax ? int32(task.grindMin) : std::max(1, level - sBotSettings.Get(player, Setting::GrindLevelsBelow));
            if (Unit* target = PickGrindTarget(player, rt, task.spot, uint8(std::max(1, minFoe)), uint8(std::min(maxFoe, 255)), task.skinHunt,
                    task.farmItem, task.grindAround))
            {
                // Targets picked again and again with no fight coming of them: the spot counts as empty. (A warrior
                // farming the Loch's frenzies stood six minutes on the lake's floor below them, its walk to them never
                // began, until the rescue.)
                constexpr uint32 GrindNoFightMs = 60 * IN_MILLISECONDS;
                if (rt.fight.active || player->IsInCombat())
                    task.grindPickSince = 0;
                else if (!task.grindPickSince)
                    task.grindPickSince = now;
                else if (now - task.grindPickSince > GrindNoFightMs)
                {
                    LOG_INFO(ActivityLog, "activity: {} has picked targets at its grind spot for {} s and no fight came of them (the last: "
                        "{} level {}, {:.0f} yd away): the spot counts as empty", player->GetName(), (now - task.grindPickSince) / IN_MILLISECONDS,
                        target->GetName(), uint32(target->GetLevel()), player->GetDistance(target));
                    rt.Avoid(task.spot, GrindEmptyMs);
                    // The creature too: the next grind spot beside it chose it again. (A warrior chose a Putrid Gargoyle
                    // 18 yd below the hill it stood on three times, then its AI kept going down for it while the
                    // planner's own steps took it up again, five minutes in the Eastern Plaguelands until the rescue,
                    // round 156.) Left alone as a creature that does not die: no grind pick, and the AI lets go of it.
                    rt.undyingFoes[target->GetGUID()] = now + 15 * MINUTE * IN_MILLISECONDS;
                    BotDriver::DropTarget(player);
                    ClearTask(rt, 0, "no fight comes of its targets");
                    return false;
                }
                task.grindIdleSince = 0;
                if (!(player->GetDistance(target) <= GrindAttackDistance && BotDriver::Attack(player, target)) &&
                    now - task.lastAssert >= AssertGapMs)
                {
                    // In the water a target near is swum to straight (NewRpg's walk does not start there).
                    if ((IsSwimming(player) || IsDiving(player)) && player->GetDistance(target) < 60.0f)
                    {
                        BotDriver::Rest(player);
                        player->GetMotionMaster()->MovePoint(0, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(),
                            FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false);
                    }
                    else
                        BotDriver::GoTo(player, task.spot.mapId, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), false);
                    task.lastAssert = now;
                }
                return true;
            }
            if (!task.grindIdleSince)
                task.grindIdleSince = now;
            // An empty spot is left 8 s after its last fight once nothing is left to loot there (20 s at most): it stood
            // 20 s every time, the level-60 grinders of the Eastern Plaguelands 70 to 80 s of every five minutes, three or
            // four spots a window (Baumi: a fight, 20 s, "nothing to fight at the spot", 2026-11-27). A hunt for skins and
            // a farm order wait the whole 20 s: their corpses are walked to and skinned or looted after the fight.
            constexpr uint32 GrindIdleShortMs = 8 * IN_MILLISECONDS;
            bool const shortWait = !task.skinHunt && !task.farmItem && !BotDriver::HasLootTarget(player, 30.0f);
            if (now - task.grindIdleSince > (shortWait ? GrindIdleShortMs : GrindIdleMs))
            {
                rt.Avoid(task.spot, GrindEmptyMs);
                ClearTask(rt, 0, "nothing to fight at the spot");
                return false;
            }
            if (distance > NearDistance / 2.0f)
            {
                if (now - task.lastAssert >= AssertGapMs)
                {
                    BotDriver::GoTo(player, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z, false);
                    task.lastAssert = now;
                }
            }
            else if (state != RpgState::Rest)
                BotDriver::Rest(player);
            return true;
        }
        case TaskKind::Escort:
        {
            QuestStatus const status = player->GetQuestStatus(task.questId);
            if (status == QUEST_STATUS_COMPLETE)
            {
                ClearTask(rt, 0, "escort done");
                return false;
            }
            if (status != QUEST_STATUS_INCOMPLETE)
            {
                // The script failed the quest (the NPC died, the bot fell behind) or the quest is gone.
                if (status == QUEST_STATUS_FAILED)
                    FailQuest(guild, bot, player, task.questId, "escort_failed");
                ClearTask(rt, 5 * IN_MILLISECONDS, status == QUEST_STATUS_FAILED ? "escort failed" : "quest no longer in the log");
                return false;
            }
            if (now - task.startedAt > EscortMaxMs)
            {
                FailQuest(guild, bot, player, task.questId, "escort_timeout");
                ClearTask(rt, 5 * IN_MILLISECONDS, "escort took too long");
                return false;
            }

            Creature* escort = player->FindNearestCreature(task.escortEntry, EscortSight, true);
            if (!task.escortSeenAt)
                task.escortSeenAt = now;
            if (!escort)
            {
                // Not in sight: at its spot, if the bot is not there yet (the quest was in the log already); else lost.
                if (task.hasSpot && Dist2d(player, task.spot) > SightDistance && !task.escortMoved)
                {
                    WalkTo(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z);
                    task.escortSeenAt = now;
                    rt.why = Acore::StringFormat("Going to {} for the escort \"{}\"", task.targetName, QuestTitle(task.questId));
                    return true;
                }
                if (now - task.escortSeenAt > EscortLostMs)
                {
                    FailQuest(guild, bot, player, task.questId, "escort_lost");
                    ClearTask(rt, 5 * IN_MILLISECONDS, "escort lost");
                    return false;
                }
                return true;
            }
            task.escortSeenAt = now;

            // Has it set off? An NPC that still stands at its spot long after the quest was taken has not started (the
            // quest was in the log from an earlier try): the quest is given back and can be taken afresh.
            if (!task.escortMoved)
            {
                if (escort->isMoving() || (task.hasSpot && escort->GetExactDist2d(task.spot.x, task.spot.y) > 5.0f))
                    task.escortMoved = true;
                else if (now - task.startedAt > EscortStartMs)
                {
                    FailQuest(guild, bot, player, task.questId, "escort_not_started");
                    ClearTask(rt, 5 * IN_MILLISECONDS, "escort did not start");
                    return false;
                }
            }

            // Stay with it: the core's follow movement, behind the NPC at its speed. NewRpg's own walk would always go
            // to where the NPC was (playerbots finishes one move before it takes the next) and fall behind until the
            // script fails the quest (60 yd). NewRpg rests meanwhile; a fight is playerbots', and the follow is set
            // again when it is over.
            // What attacks the NPC is the bot's fight too (playerbots only defends the bot itself).
            if (DefendWard(player, escort))
            {
                rt.why = Acore::StringFormat("Fighting for {} (\"{}\")", task.targetName, QuestTitle(task.questId));
                return true;
            }
            if (BotDriver::GetRpgState(player) != RpgState::Rest)
                BotDriver::Rest(player);
            if (player->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE &&
                now - task.lastAssert >= IN_MILLISECONDS)
            {
                if (player->IsSitState())
                    player->SetStandState(UNIT_STAND_STATE_STAND);
                player->GetMotionMaster()->MoveFollow(escort, EscortFollowDistance / 2.0f, float(M_PI));
                task.lastAssert = now;
            }
            rt.why = Acore::StringFormat("Escorting {} for \"{}\"{}", task.targetName, QuestTitle(task.questId),
                task.escortMoved ? "" : " (waiting for it to set off)");
            return true;
        }
        case TaskKind::Hold:
        {
            if (now - task.startedAt < HoldReplanMs)
                return true;
            ClearTask(rt);
            return false;
        }
        case TaskKind::Voyage:
            return StepVoyage(guild, bot, player, rt);
        case TaskKind::Event:
            return StepEvent(guild, bot, player, rt);
        case TaskKind::SafeSpot:
        {
            // The walk to the inn that RunIdle began goes on here, for whoever calls the task step (the profession mode
            // with nothing to do, the wait at the level cap). It was ended at once before: every walk to an inn was
            // the one straight NewRpg move RunIdle began, "starts safe spot ... ends safe spot after 0 s: over" 768
            // times in a day, and a shaman stood 30 minutes in a pocket of Hive'Zora, its move refused every 30 s.
            float const distance = Dist2d(player, task.spot);
            if (distance <= 15.0f)
            {
                ClearTask(rt, 0, "at the inn");
                return false;
            }
            if (task.noWay || UpdateStuck(player, rt, task.spot.x, task.spot.y))
            {
                ClearTask(rt, 5 * IN_MILLISECONDS, "inn not reached");
                return false;
            }
            WalkTo(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z);
            return true;
        }
        case TaskKind::None:
            ClearTask(rt);
            return false;
    }
    return false;
}

void OrderService::StartVoyage(Player* player, BotRuntime& rt, TravelLink const& link)
{
    BotTask task;
    task.kind = TaskKind::Voyage;
    task.link = link;
    task.voyage = VoyagePhase::ToDock;
    task.hasSpot = true;
    task.spot.mapId = link.from.mapId;
    task.spot.x = link.from.x;
    task.spot.y = link.from.y;
    task.spot.z = link.from.z;
    task.targetName = link.toName;
    StartTask(player, rt, task);
    rt.task.phaseSince = getMSTime();
    LOG_INFO("module", "mod-guild-adventure: {} takes {} from {} to {}", player->GetName(), link.name, link.fromName, link.toName);
}

bool OrderService::StepVoyage(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    uint32 const now = getMSTime();
    BotTask& task = rt.task;
    TravelLink const& link = task.link;
    MotionTransport* ship = FindShip(player->GetMap(), link.transportEntry);

    auto setPhase = [&](VoyagePhase phase)
    {
        if (GetSettings().debug)
            LOG_INFO("module", "mod-guild-adventure: {} voyage on {}: {} -> {}", bot.name, link.name, ToString(task.voyage), ToString(phase));
        task.voyage = phase;
        task.phaseSince = now;
        task.lastAssert = 0;
    };

    // The way to the dock, portal or teleporter is judged like a travel's (a level-22 character on its way to the ship
    // in Ratchet walked from the east of Ashenvale through the Warsong camps towards the Horde's rampart, 14 053 yd past
    // level-31 creatures, and died twice): past creatures far above it or a group it cannot take it does not go, and
    // what lies across the sea waits. Unless it stands among such creatures already. Never into a town of the other
    // side, nor through one unless it stands in one and walks out (WalkTo's town gate leaves a voyage to this): the way
    // to a portal had no gate at all, and an Alliance death knight walked from Feralas up the Mesa Elevator into
    // Thunder Bluff to its Portal to Blasted Lands, died to the Bluffwatchers, took the same way from its spirit healer
    // and died again, round 242. 0: go on; 1: the caller returns true; 2: it returns false.
    auto gateWalk = [&](std::string const& toWhat, std::string const& whereName) -> int
    {
        int32 const level = int32(player->GetLevel());
        TeamId const team = player->GetTeamId();
        bool const group = task.route.found && RoadMap::GroupCounts(task.route.crowd, task.route.crowdLevel, level) &&
            !Risk::CanFight(player, rt, task.route.crowdLevel, task.route.crowd);
        bool const standsAmong = int32(sRoadMap.DangerAlong(player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
            player->GetPositionX(), player->GetPositionY(), team)) - level > RouteMaxDanger;
        bool const inTown = player->GetMapId() == link.from.mapId &&
            sRoadMap.InHostileTown(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), team);
        bool const toTown = sRoadMap.InHostileTown(link.from.mapId, link.from.x, link.from.y, team);
        bool const town = toTown || (!inTown && task.route.found && task.route.danger >= RoadMap::TownDanger);
        bool const danger = !town && task.route.found && int32(task.route.danger) - level > RouteMaxDanger && !standsAmong;
        if (group && WidenTravelStart(player, task, "the dock"))
            return 1;
        if (!group && !danger && !town)
            return 0;
        std::string const flight = task.noFlightWhy ? Acore::StringFormat("; no flight: {}", task.noFlightWhy) :
            task.noFlight ? std::string("; no flight: the way to the flight master was refused") : std::string();
        if (town)
            LOG_INFO(ActivityLog, "activity: {} does not walk to {} for {}: {}{}", bot.name, toWhat, link.name,
                toTown ? "it stands in a town of the other side" : Acore::StringFormat("the way ({}) leads through a town of the other "
                    "side", task.route.via), flight);
        else if (group)
            LOG_INFO(ActivityLog, "activity: {} does not walk to {} for {}: {} level-{} creatures stand together on "
                "the safest way ({}){}", bot.name, toWhat, link.name, uint32(task.route.crowd),
                uint32(task.route.crowdLevel), task.route.via, flight);
        else
            LOG_INFO(ActivityLog, "activity: {} does not walk to {} for {}: the safest way ({}, {:.0f} yd) passes "
                "{} (it is level {}){}", bot.name, toWhat, link.name, task.route.via, task.route.length,
                RoadMap::DangerName(task.route.danger), level, flight);
        std::string const why = Acore::StringFormat("The way to {} is too dangerous at level {}", whereName, level);
        // A member travelling after its squad leader: its way failed, no zone or sea is written off for it.
        if (task.partnerMeet)
        {
            BotDriver::Rest(player);
            rt.squad.meetFailed = true;
            ClearTask(rt, 5 * IN_MILLISECONDS, "route too dangerous");
            rt.why = why;
            return 2;
        }
        bool const errand = task.errand;
        BotDriver::Rest(player);
        ClearTask(rt, 5 * IN_MILLISECONDS, "route too dangerous");
        rt.CloseSea(now, player->GetMapId(), player->GetPositionX(), player->GetPositionY());
        if (errand)
            DropErrand(bot, rt, "the way to the ship is too dangerous");
        else if (rt.targetZone)
        {
            rt.unreachableZones[rt.targetZone] = now + UnreachableZoneMs;
            rt.zoneChosenAt = 0;
        }
        rt.why = why;
        // No safe way on foot and no flight from here: the hearthstone, and a new plan from the inn. The refusal was
        // this place's, not the zone's across the sea: from the inn the zone is open again at once (its flight master,
        // then the ship). Three level-60 characters in the south of Silithus were refused the walk to the ship past
        // level-61 to 64 creatures, hearthed to Cenarion Hold and there gave up Eastern Plaguelands for ten minutes,
        // round 145. From the inn a second refusal writes the zone off as before (the stone is not used where the bot
        // stands at its inn).
        // (Only where the stone is the way across, an inn on another continent or in a capital, or out of a zone that
        // keeps killing the bot, as for a refused way to a zone: the test guild's leader hearthed from Plaguewood to Light's
        // Hope on the same continent for the ship to Silithus, 2026-11-27; the owner: the hearthstone [quote left out]. Otherwise the zone waits,
        // the sea is closed for a while and the autopilot picks another zone or grinds where it is.)
        if (!errand && (TravelHearthAllowed(player) || KeepsKillingHere(player, rt)) &&
            HearthHome(player, rt, "no safe way to the ship from where it stood",
                IsStrandedHere(player, rt) ? StrandedHearthYards : DefaultHearthYards) && rt.targetZone)
            rt.unreachableZones.erase(rt.targetZone);
        return 2;
    };

    // Portals: walk into the trigger, then the same teleport the client's area trigger message causes. A teleporter
    // players click (an Orb of Translocation) the same way: walked up to, then its teleport.
    if (link.areaTrigger || link.gameObject)
    {
        if (player->GetMapId() == link.to.mapId && Dist2d(player, link.to.x, link.to.y) <= 50.0f)
        {
            RecordEvent(guild, "ZONE_CHANGE", bot.guid, Acore::StringFormat("{} went through {}.", bot.name, link.name),
                player->GetLevel(), player->GetZoneId());
            // Through to its zone's side, or for a hand-in: a crossing like a voyage, the bot stays on this side for the
            // hour (a level-15 undead took the Orb of Translocation and walked seven minutes to Ghostlands for 21
            // quests, and the moment it arrived an errand of three hand-ins took it back through the Orb to Silverpine,
            // twice). Not the first portal of a longer way to its zone (Darnassus to Rut'theran for Darkshore).
            bool toItsSide = task.errand;
            if (!toItsSide && rt.targetZone)
                if (ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(rt.targetZone))
                    toItsSide = GetRegion(player) == GetZoneRegion(zone->continent, zone->zoneId, guild.team);
            if (toItsSide && !task.shoreLeave)
            {
                rt.lastCrossingAt = std::max<uint32>(getMSTime(), 1);
                rt.errandCrossing = task.errand ? task.questId : 0;
            }
            ClearTask(rt, 0, "through the portal");
            return false;
        }
        float const distance = Dist2d(player, link.from.x, link.from.y);
        if (player->GetMapId() == link.from.mapId && distance <= std::max(4.0f, link.triggerRadius))
        {
            if (now - task.lastAssert >= 5 * IN_MILLISECONDS)
            {
                player->TeleportTo(link.to.mapId, link.to.x, link.to.y, link.to.z, player->GetOrientation());
                task.lastAssert = now;
            }
            rt.why = Acore::StringFormat("Travelling to {}: through {}", link.toName, link.name);
            return true;
        }
        if (UpdateStuck(player, rt, link.from.x, link.from.y))
        {
            OnTravelStuck(bot, rt, distance);
            return false;
        }
        WalkTo(player, rt, link.from.mapId, link.from.x, link.from.y, link.from.z);
        if (int const gate = gateWalk(Acore::StringFormat("{} in {}", link.name, link.fromName), link.name))
            return gate == 1;
        rt.why = Acore::StringFormat("Travelling to {}: walking to {} ({})", link.toName, link.name, RoughDistance(distance));
        return true;
    }

    switch (task.voyage)
    {
        case VoyagePhase::ToDock:
        {
            float const distance = Dist2d(player, link.from.x, link.from.y);
            if (player->GetMapId() == link.from.mapId && distance <= DockReachedDistance)
            {
                BotDriver::Rest(player);
                setPhase(VoyagePhase::Wait);
                return true;
            }
            if (UpdateStuck(player, rt, task.hasFlightVia ? task.flightVia.x : link.from.x, task.hasFlightVia ? task.flightVia.y : link.from.y))
            {
                OnTravelStuck(bot, rt, distance);
                return false;
            }
            if (BotDriver::GetRpgState(player) == RpgState::TravelFlight)
            {
                rt.why = Acore::StringFormat("Travelling to {}: flying towards the dock in {}", link.toName, link.fromName);
                return true;
            }
            // (As a travel: at its first step whatever the bot's minute says. Caelithra (33) was refused a travel to
            // Arathi, whose flight check set the minute, picked Desolace six seconds later, and its voyage's walk to the
            // zeppelin in Tirisfal looked for no flight from Tarren Mill: refused for a group on the way, the
            // hearthstone, round 256.)
            if (!task.hasFlightVia && !task.noFlight && (now >= rt.nextFlightCheck || !task.flightTried))
            {
                task.flightTried = true;
                rt.nextFlightCheck = now + FlightCheckMs;
                if (TryFlight(player, rt, link.from.mapId, link.from.x, link.from.y, link.from.z))
                    return true;
            }
            if (task.hasFlightVia)
            {
                if (!IsAtFlightMaster(player, task.flightVia))
                {
                    WalkTo(player, rt, task.flightVia.mapId, task.flightVia.x, task.flightVia.y, task.flightVia.z);
                    rt.why = Acore::StringFormat("Travelling to {}: walking to the flight master ({})", link.toName,
                        RoughDistance(Dist2d(player, task.flightVia)));
                    return true;
                }
                rt.nextFlightCheck = now + FlightCheckMs;
                if (TryFlight(player, rt, link.from.mapId, link.from.x, link.from.y, link.from.z))
                    return true;
                task.hasFlightVia = false; // no flight after all: on foot
            }
            WalkTo(player, rt, link.from.mapId, link.from.x, link.from.y, link.from.z);
            if (int const gate = gateWalk(Acore::StringFormat("the dock in {}", link.fromName),
                    Acore::StringFormat("the dock in {}", link.fromName)))
                return gate == 1;
            rt.why = Acore::StringFormat("Travelling to {}: walking to the dock in {} ({})", link.toName, link.fromName, RoughDistance(distance));
            return true;
        }
        case VoyagePhase::Wait:
        {
            if (now - task.startedAt > VoyageBoardTimeoutMs)
            {
                LOG_INFO("module", "mod-guild-adventure: {} gave up waiting for {} in {}", bot.name, link.name, link.fromName);
                if (task.partnerMeet)
                    rt.squad.meetFailed = true;
                ClearTask(rt, 5 * MINUTE * IN_MILLISECONDS, "gave up waiting for the ship");
                return false;
            }
            rt.why = Acore::StringFormat("Travelling to {}: waiting for {} in {}", link.toName, link.name, link.fromName);
            if (IsDockedAt(ship, link.from) && Dist2d(player, ship->GetPositionX(), ship->GetPositionY()) <= DockReachedDistance)
            {
                setPhase(VoyagePhase::Board);
                return true;
            }
            // Too far from the pier (pushed away by a fight): walk back.
            if (Dist2d(player, link.from.x, link.from.y) > DockReachedDistance && now - task.lastAssert >= AssertGapMs)
            {
                BotDriver::GoTo(player, link.from.mapId, link.from.x, link.from.y, link.from.z, false);
                task.lastAssert = now;
            }
            return true;
        }
        case VoyagePhase::Board:
        {
            if (ship && player->GetTransport() == ship)
            {
                task.ship = ship->GetGUID();
                setPhase(VoyagePhase::Ride);
                return true;
            }
            if (!IsDockedAt(ship, link.from))
            {
                setPhase(VoyagePhase::Wait); // it left without us: wait for the next one
                return true;
            }
            // On the deck already: become its passenger (the same as playerbots' own transport check does once a second).
            if (player->GetMap()->GetTransportForPos(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(),
                player->GetPositionZ(), player) == ship)
            {
                ship->AddPassenger(player, true);
                return true;
            }
            if (now - task.lastAssert >= 3 * IN_MILLISECONDS)
            {
                float x, y, z;
                DeckSpot(ship, x, y, z);
                if (GetSettings().debug)
                {
                    Transport* under = player->GetMap()->GetTransportForPos(player->GetPhaseMask(), player->GetPositionX(),
                        player->GetPositionY(), player->GetPositionZ(), player);
                    LOG_INFO("module", "mod-guild-adventure: {} boarding {}: ship at ({:.1f}, {:.1f}, {:.1f}) state {}, crew {}, deck spot "
                        "({:.1f}, {:.1f}, {:.1f}), bot at ({:.1f}, {:.1f}, {:.1f}), transport under the bot: {}", bot.name, link.name,
                        ship->GetPositionX(), ship->GetPositionY(), ship->GetPositionZ(), ship->GetGoState() == GO_STATE_READY ? "ready" : "active",
                        ship->GetStaticPassengers().size(), x, y, z, player->GetPositionX(), player->GetPositionY(),
                        player->GetPositionZ(), under ? under->GetName() : "none");
                }
                BotDriver::Hop(player, x, y, z);
                task.lastAssert = now;
            }
            rt.why = Acore::StringFormat("Travelling to {}: boarding {}", link.toName, link.name);
            return true;
        }
        case VoyagePhase::Ride:
        {
            if (!player->GetTransport())
            {
                // Off the ship before it arrived (it left the dock under us, or a fight knocked the bot off): try again.
                setPhase(player->GetMapId() == link.from.mapId ? VoyagePhase::Wait : VoyagePhase::ToDock);
                return true;
            }
            if (task.anyDock && ship && player->GetTransport() == ship)
            {
                // Found on board without a voyage: whichever dock of this ship comes first.
                for (TravelLink const& other : sWorldIndex.GetTravelLinks())
                    if (other.transportEntry == link.transportEntry && IsDockedAt(ship, other.to))
                    {
                        task.link = other;
                        task.anyDock = false;
                        setPhase(VoyagePhase::Disembark);
                        return true;
                    }
                rt.why = Acore::StringFormat("On board {}: going ashore at the next stop", link.name);
                return true;
            }
            rt.why = Acore::StringFormat("Travelling to {}: on board {}", link.toName, link.name);
            if (ship && player->GetTransport() == ship && IsDockedAt(ship, link.to))
                setPhase(VoyagePhase::Disembark);
            return true;
        }
        case VoyagePhase::Disembark:
        {
            if (!player->GetTransport())
            {
                RecordEvent(guild, "ZONE_CHANGE", bot.guid, Acore::StringFormat("{} arrived in {} on {}.", bot.name, link.toName, link.name),
                    player->GetLevel(), player->GetZoneId());
                // (Not ashore from a ship it was found on: the planner chose no crossing, and its own side's hour held the
                // guild leader three minutes in Rut'theran with no zone it could get to, after its owner had left it on
                // the Moonspray.)
                if (!task.shoreLeave)
                    rt.lastCrossingAt = std::max<uint32>(getMSTime(), 1);
                // It sailed for a hand-in and the zone it was heading for lies behind it now, across the sea: it looks
                // at the zones again from this side instead of sailing back. (Live round 51: a hunter in the Barrens
                // with Ashenvale as its zone sailed to Menethil for two hand-ins, 255 s, and straight back, 266 s.)
                // (Only after an errand's voyage: the first leg of a journey to its zone over two seas ends ashore too.)
                rt.errandCrossing = task.errand ? task.questId : 0;
                if (task.errand && bot.order.type == OrderType::Auto && rt.targetZone)
                    if (ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(rt.targetZone))
                        if (sWorldIndex.CountTravelLegs(GetRegion(player), GetZoneRegion(zone->continent, zone->zoneId, guild.team),
                                guild.team) != 0)
                        {
                            LOG_INFO(ActivityLog, "activity: {} went ashore across the sea from its zone {}: it looks at the zones on "
                                "this side", bot.name, ZoneName(rt.targetZone));
                            rt.zoneChosenAt = 0;
                        }
                ClearTask(rt, 0, "ashore");
                return false;
            }
            if (!ship || !IsDockedAt(ship, link.to))
            {
                setPhase(VoyagePhase::Ride); // it left again before the bot stepped off
                return true;
            }
            if (now - task.lastAssert >= 3 * IN_MILLISECONDS)
            {
                float x, y, z;
                if (!PierSpot(player, ship, rt, x, y, z))
                {
                    x = link.to.x;
                    y = link.to.y;
                    z = link.to.z + 2.0f;
                }
                // A pier the navmesh does not reach (the top of a zeppelin tower): onto the landing where it begins. (The
                // nearest ground of the navmesh first: at Orgrimmar's tower no service stands within 250 yd, the hop went to
                // the zeppelin's own stop in the air 8 yd above the platform, the path finder's search did not reach the
                // platform from there, and eleven bots in two days were rescued from that spot.)
                NavPoint landing;
                if (sDungeonNav.NearestOnMesh(player->GetMapId(), x, y, z, 25.0f, landing))
                {
                    x = landing.x;
                    y = landing.y;
                    z = landing.z;
                }
                else
                    SnapToNavmesh(player, player->GetMapId(), x, y, z);
                BotDriver::LeaveTransport(player);
                BotDriver::Hop(player, x, y, z + 0.5f);
                task.lastAssert = now;
            }
            rt.why = Acore::StringFormat("Travelling to {}: going ashore", link.toName);
            return true;
        }
    }
    return false;
}

void OrderService::CareForStaleQuests(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    // Looked at this often; a finished grey quest is kept while one of its enders is this close.
    constexpr uint32 CareMs = 20 * IN_MILLISECONDS;
    constexpr float GreyTurnInDistance = 1000.0f;

    uint32 const now = getMSTime();
    if (now < rt.nextStaleQuestCare)
        return;
    rt.nextStaleQuestCare = now + CareMs;
    if (!player->IsAlive() || player->IsInCombat())
        return;

    bool const abandonGrey = sBotSettings.IsOn(player, Setting::QuestsAbandonGrey);
    uint8 const level = player->GetLevel();
    // Grey as the client paints it: a quest at or below the level from which creatures give the character nothing.
    uint32 const greyLevel = Acore::XP::GetGrayLevel(level);

    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = player->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        // (Not the quest the bot is at work on or on its way to hand in: that task ends first.)
        if (!quest || questId == rt.task.questId || questId == rt.errandQuest)
            continue;
        QuestStatus const status = player->GetQuestStatus(questId);

        // A quest that failed (its timer ran out, its escort died while the bot was elsewhere) stays in the log as
        // failed for good: given back like a failure of the planner's own (once more at its giver, then blocked).
        if (status == QUEST_STATUS_FAILED)
        {
            LOG_INFO(ActivityLog, "activity: {} abandons quest {} \"{}\": it failed (a timer, an escort); it is taken again at its "
                "giver, once", bot.name, questId, QuestTitle(questId));
            FailQuest(guild, bot, player, questId, "failed");
            continue;
        }

        // (A quest without a level of its own scales with the character and is never grey.)
        if (!abandonGrey || quest->GetQuestLevel() <= 0 || uint32(quest->GetQuestLevel()) > greyLevel)
            continue;
        bool const complete = status == QUEST_STATUS_COMPLETE;
        if (!complete && status != QUEST_STATUS_INCOMPLETE)
            continue;
        // Finished: handed in when the ender is close by (the planner's turn-in, an errand next door), else given up.
        if (complete && NearestSpawn(sWorldIndex.GetQuestEnders(questId), player, 0, true, GreyTurnInDistance, rt))
            continue;
        LOG_INFO(ActivityLog, "activity: {} abandons quest {} \"{}\": grey at level {} (a level-{} quest){}", bot.name, questId,
            QuestTitle(questId), uint32(level), quest->GetQuestLevel(),
            complete ? Acore::StringFormat("; it was finished, but its hand-in is more than {:.0f} yd away", GreyTurnInDistance) :
                std::string());
        BotDriver::AbandonQuest(player, questId);
    }
}

void OrderService::FailQuest(AdventureGuild& /*guild*/, GuildBot& bot, Player* player, uint32 questId, std::string const& reason)
{
    // A hard quest no guild partner stands beside the bot for (QuestPartners.h): it stays in the log and waits for one,
    // instead of being given back for its deaths or for no progress (the owner, 2026-11-13).
    if (sQuestPartners.KeepsInsteadOfFailing(bot, player, questId, reason))
        return;
    QuestBlock& block = bot.questBlocks[questId];
    block.failures = uint8(std::min<uint32>(block.failures + 1, 255));
    block.reason = reason;
    block.updatedAt = Now();
    // wowbot335 A21: the first failure abandons the quest and allows one retry, the second blocks it for good.
    if (block.failures >= 2)
        block.blocked = true;
    // Enough bots at the same dead end, and no AI-played character takes the quest any more (QuestSupport).
    if (block.blocked)
        sQuestSupport.OnQuestBlocked(questId, bot.guid.GetCounter(), reason);

    // Only quests still in progress are abandoned: a completed one keeps its reward for a later turn-in (the planner skips
    // it once blocked; the owner can still turn it in when playing the bot).
    if (player->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE || player->GetQuestStatus(questId) == QUEST_STATUS_FAILED)
        BotDriver::AbandonQuest(player, questId);
    BotDriver::ForgetAIGivenUp(player, questId);
    sGuildAdventureMgr.SaveQuestBlock(bot, questId);

    LOG_INFO("module", "mod-guild-adventure: {} failed quest {} \"{}\" ({}), {}", bot.name, questId, QuestTitle(questId), reason,
        block.blocked ? "blocked" : "one retry left");
}

// ---------------------------------------------------------------------------------------------------------------
// Events and queries
// ---------------------------------------------------------------------------------------------------------------

void OrderService::OnOrderChanged(AdventureGuild& guild, GuildBot& bot)
{
    // A dungeon group of the run window whose leader got another order: its bots are what they were before again.
    if (bot.order.type != OrderType::Dungeon)
        DisbandDungeonGroup(guild, bot, false);

    auto itr = _runtime.find(bot.guid);
    if (itr == _runtime.end())
        return;

    BotRuntime& rt = itr->second;
    ClearTask(rt, 0, "order changed");
    rt.waitUntil = 0;
    rt.targetZone = 0;
    rt.zoneChosenAt = 0;
    rt.reportedOutleveled = 0;
    rt.capIdleUntil = 0;
    rt.flightBooked = false;
    rt.errandQuest = 0;
    rt.errandStuck = 0;
    rt.errandSkip.clear();
    rt.why.clear();
    rt.whyBeforeDeath.clear();
    rt.nextGroupCheck = 0;
    rt.catchUp = {};
    rt.nextCatchUpLook = 0;
    rt.gatherRefusals = 0;
    rt.gatherPausedUntil = 0;
    // A new order gives zones the bot got stuck on the way to a new chance.
    rt.unreachableZones.clear();
    rt.travelStuckZone = 0;
    rt.travelStuckCount = 0;
    rt.farm = BotRuntime::Farm();

    // Stop what NewRpg was doing for the old order; the next tick plans the new one.
    if (Player* player = sGuildBotHolder.GetPlannedPlayer(bot.guid))
        BotDriver::Rest(player);
}

bool OrderService::CanTakeGroup(Player const* player, uint8 level, uint8 count) const
{
    auto runtime = _runtime.find(player->GetGUID());
    return runtime == _runtime.end() || Risk::CanFight(player, runtime->second, level, count);
}

void OrderService::OnModuleTeleport(ObjectGuid bot)
{
    auto itr = _runtime.find(bot);
    if (itr != _runtime.end())
        itr->second.hasLastPosition = false; // the next tick takes the new place as its last position
}

void OrderService::OnLevelUp(AdventureGuild& /*guild*/, GuildBot& bot, uint8 level)
{
    auto itr = _runtime.find(bot.guid);
    if (itr == _runtime.end())
        return;
    BotRuntime& rt = itr->second;
    rt.lastLevel = level;
    rt.zoneChosenAt = 0;    // AUTO: look at the zones again
    rt.nextUpkeepCheck = 0; // trainer
    rt.nextClassAbilities = 0;
    rt.nextSpec = 0;
    rt.syncStrategies = true;

    // Class spells come at once, not with the next plan (a bot in the middle of a long task would wait minutes).
    Player* player = sBotSettings.IsOn(bot.guid, Setting::AutoLearnClassSpells) ? sGuildBotHolder.GetPlannedPlayer(bot.guid) : nullptr;
    if (!player || player->GetLevel() <= rt.trainCheckedLevel)
        return;
    rt.trainCheckedLevel = player->GetLevel();
    ServicePoint const* trainer = ClassTrainerForLearning(player);
    if (uint32 const learned = trainer ? LearnTrainerSpellsInPlace(TrainerOf(trainer), player) : 0)
        LOG_INFO("module", "mod-guild-adventure: {} learned {} class spell{} (level {}, no trainer trip)",
            player->GetName(), learned, learned == 1 ? "" : "s", player->GetLevel());
}

uint32 OrderService::LearnClassSpellsNow(Player* player)
{
    ServicePoint const* trainer = player ? ClassTrainerForLearning(player) : nullptr;
    return trainer ? LearnTrainerSpellsInPlace(TrainerOf(trainer), player) : 0;
}

bool OrderService::WantsMaster(ObjectGuid bot) const
{
    AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(bot);
    GuildBot const* guildBot = guild ? guild->FindPlanned(bot) : nullptr;
    // Characters without a roster row (test bots logged in with .ga bot login) have no order: they follow their owner,
    // as before M4 (docs/take-control.md section 5.7).
    if (!guildBot)
        return true;
    // A squad member's master is its squad leader, which the planner sets itself (OrderSquads.cpp), never the owner's
    // character.
    if (auto itr = _runtime.find(bot); itr != _runtime.end() && itr->second.squad.following)
        return false;
    return guildBot->order.type == OrderType::Follow;
}

bool OrderService::IsLeaderPlanned(ObjectGuid guid) const
{
    AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfLeader(guid);
    return guild && guild->leaderPlanned;
}

void OrderService::SetLeaderOrder(AdventureGuild& guild, std::optional<BotOrder> order)
{
    if (!order)
    {
        if (!guild.leaderPlanned)
            return;
        sGuildAdventureMgr.ClearLeaderOrder(guild);
        _runtime.erase(guild.leader);
        // An AI-held leader stops what NewRpg was doing and goes back to holding its position for its owner.
        if (Player* player = sGuildBotHolder.GetAIDrivenPlayer(guild.leader))
            BotDriver::Rest(player);
        sGuildBotHolder.ReapplyLeaderPolicy(guild.leader);
        return;
    }

    guild.leaderPlan.order = *order;
    guild.leaderPlan.order.setAt = Now();
    guild.leaderPlanned = true;
    sGuildAdventureMgr.SaveOrder(guild.leaderPlan);
    OnOrderChanged(guild, guild.leaderPlan);
}

std::string OrderService::GetWhy(ObjectGuid bot) const
{
    auto itr = _runtime.find(bot);
    return itr != _runtime.end() ? itr->second.why : std::string();
}

uint32 OrderService::GetXpPerHour(ObjectGuid bot) const
{
    auto itr = _runtime.find(bot);
    if (itr == _runtime.end() || itr->second.samples.size() < 2)
        return 0;
    auto const& first = itr->second.samples.front();
    auto const& last = itr->second.samples.back();
    uint32 const span = last.time - first.time;
    if (span < MinRateSpanSec || last.xp <= first.xp)
        return 0;
    return uint32((last.xp - first.xp) * HOUR / span);
}

uint32 OrderService::GetMoneyPerHour(ObjectGuid bot) const
{
    auto itr = _runtime.find(bot);
    if (itr == _runtime.end() || itr->second.samples.size() < 2)
        return 0;
    auto const& first = itr->second.samples.front();
    auto const& last = itr->second.samples.back();
    uint32 const span = last.time - first.time;
    if (span < MinRateSpanSec || last.money <= first.money)
        return 0;
    return uint32(uint64(last.money - first.money) * HOUR / span);
}

void OrderService::Forget(ObjectGuid bot)
{
    _runtime.erase(bot);
}

std::string OrderService::DescribeTask(GuildBot const& /*bot*/, Player* player, BotRuntime const& rt) const
{
    BotTask const& task = rt.task;
    std::string text = Acore::StringFormat("task {}", ToString(task.kind));
    if (task.questId)
        text += Acore::StringFormat(", quest {} \"{}\"", task.questId, QuestTitle(task.questId));
    if (task.objective >= 0)
        text += Acore::StringFormat(", objective {}", task.objective);
    if (!task.targetName.empty())
        text += ", target " + task.targetName;
    if (task.kind == TaskKind::Voyage)
        text += Acore::StringFormat(", {} {} for {} s", task.link.name, ToString(task.voyage), GetMSTimeDiffToNow(task.phaseSince) / IN_MILLISECONDS);
    if (task.kind != TaskKind::None && player)
    {
        if (task.spot.x != 0.0f || task.spot.y != 0.0f)
            text += Acore::StringFormat(", {:.0f} yd", Dist2d(player, task.spot));
        text += Acore::StringFormat(", {} s", GetMSTimeDiffToNow(task.startedAt) / IN_MILLISECONDS);
    }
    if (task.deaths)
        text += Acore::StringFormat(", {} death{}", task.deaths, task.deaths == 1 ? "" : "s");
    return text;
}

std::string OrderService::DescribeSpot(Player* player, float x, float y, float z, uint32 creatureEntry) const
{
    static BotRuntime const noRuntime;
    auto itr = _runtime.find(player->GetGUID());
    SpawnPoint spot;
    spot.mapId = uint16(player->GetMapId());
    spot.x = x;
    spot.y = y;
    spot.z = z;
    spot.entry = creatureEntry;
    spot.gameObject = !creatureEntry;
    // With a creature: its spawn nearest to the point (within 40 yards), judged as the planner judges it (the spawn
    // itself is no neighbour of its own).
    if (creatureEntry)
    {
        constexpr float SnapYards = 40.0f;
        SpawnPoint const* nearest = nullptr;
        float best = SnapYards;
        for (SpawnPoint const& spawn : sWorldIndex.GetCreatureSpawns(creatureEntry))
            if (spawn.mapId == spot.mapId)
                if (float const distance = std::hypot(spawn.x - x, spawn.y - y); distance < best)
                {
                    nearest = &spawn;
                    best = distance;
                }
        if (nearest)
            spot = *nearest;
    }
    return Risk::DescribeSpot(player, itr != _runtime.end() ? itr->second : noRuntime, spot);
}

std::vector<std::string> OrderService::Describe(ObjectGuid guid) const
{
    std::vector<std::string> lines;
    AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(guid);
    GuildBot const* bot = guild ? guild->FindPlanned(guid) : nullptr;
    if (!bot)
    {
        lines.push_back(guild ? "the guild leader has no order: it holds its position while the AI holds it" :
            "not a guild bot");
        return lines;
    }

    Player* player = sGuildBotHolder.GetPlannedPlayer(guid);
    lines.push_back(Acore::StringFormat("{}: order {}{}, {}", bot->name, ToToken(bot->order.type),
        !bot->order.zoneId ? std::string() :
        bot->order.type == OrderType::Farm ? Acore::StringFormat(" {} ({})", sObjectMgr->GetItemTemplate(bot->order.zoneId) ?
            sObjectMgr->GetItemTemplate(bot->order.zoneId)->Name1 : std::string("?"), bot->order.zoneId) :
        bot->order.type != OrderType::Dungeon ? " " + ZoneName(bot->order.zoneId) :
            " " + (sDungeons.GetPlan(bot->order.zoneId) ? sDungeons.GetPlan(bot->order.zoneId)->name : std::string("?")),
        !player ? "not AI-driven" :
        sGuildBotHolder.IsWatched(guid) ? "AI-driven in watch mode (its owner's client watches)" : "AI-driven"));

    auto itr = _runtime.find(guid);
    if (itr == _runtime.end())
    {
        lines.push_back("no planner state");
        return lines;
    }

    BotRuntime const& rt = itr->second;
    lines.push_back("why: " + rt.why);
    lines.push_back(DescribeTask(*bot, player, rt));
    if (SquadState const squad = SquadState(rt.squad.state); squad != SquadState::None)
    {
        std::string members;
        for (ObjectGuid const& member : sSquadService.GetMembers(guid))
            if (GuildBot const* memberBot = guild->FindBot(member))
                members += (members.empty() ? "" : ", ") + memberBot->name;
        GuildBot const* leaderBot = guild->FindBot(sSquadService.GetLeader(guid));
        Group const* group = player ? player->GetGroup() : nullptr;
        lines.push_back(Acore::StringFormat("squad: {}{}{}{}{}", GuildAdventure::Describe(squad),
            leaderBot ? " (leader " + leaderBot->name + ")" : "", members.empty() ? "" : " (members " + members + ")",
            rt.squad.waitFor.empty() ? "" : ", waits: " + rt.squad.waitFor,
            group ? Acore::StringFormat(", party of {}, loot {}", group->GetMembersCount(),
                group->GetLootMethod() == FREE_FOR_ALL ? "free for all" : "by the party's rules") : std::string()));
    }
    // The numbers of the dungeon run it leads, so far (the probe's dungeon-metrics waits for a fight counted here).
    if (rt.dungeon.id && rt.dungeon.phase == 2)
        lines.push_back("dungeon run so far: " + DescribeDungeonMetrics(rt.dungeon.metrics));
    // Its breath under water as the game counts it (CheckBreath).
    if (player && (rt.breathUsedMs || IsDiving(player)))
    {
        uint32 const breath = BreathMs(player);
        lines.push_back(breath ? Acore::StringFormat("breath: {} s of its {} s used{}", rt.breathUsedMs / IN_MILLISECONDS,
            breath / IN_MILLISECONDS, IsDiving(player) ? " (under water)" : "") : std::string("breath: it breathes under water"));
    }
    // Quest partners (QuestPartners.h): the quest of its partner squad, and the quests it keeps for a partner.
    if (player)
    {
        std::string kept;
        for (uint32 questId : sQuestPartners.Waiting(*bot, player))
            kept += Acore::StringFormat("{}{} \"{}\"", kept.empty() ? "" : ", ", questId, QuestTitle(questId));
        if (rt.squad.partnerQuest || !kept.empty())
            lines.push_back(Acore::StringFormat("quest partners: {}{}", rt.squad.partnerQuest ?
                Acore::StringFormat("teamed up for quest {} \"{}\"{}", rt.squad.partnerQuest, QuestTitle(rt.squad.partnerQuest),
                    rt.squad.meeting ? " (on its way to its partner)" : "") : std::string("in no partner squad"),
                kept.empty() ? std::string() : "; keeps for a partner: " + kept));
    }
    if (player)
    {
        lines.push_back(Acore::StringFormat("level {} in {} ({}), target zone {}, rpg {} for {} s, master {}, group {}",
            uint32(player->GetLevel()), ZoneName(player->GetZoneId()), ToString(GetRegion(player)),
            rt.targetZone ? ZoneName(rt.targetZone) : "-", ToString(BotDriver::GetRpgState(player)),
            BotDriver::GetRpgStateSeconds(player), BotDriver::GetMaster(player) ? BotDriver::GetMaster(player)->GetName() : "none",
            player->GetGroup() ? "yes" : "no"));
        // Where a travel to the target zone would end (the nearest quest giver the bot has there, else the zone's hub).
        if (ZoneTemplate const* zone = rt.targetZone && rt.targetZone != player->GetZoneId() ?
            sGuildAdventureMgr.GetZone(rt.targetZone) : nullptr)
        {
            SpawnPoint entry;
            if (ZoneEntry(*bot, player, rt, *zone, entry))
                lines.push_back(Acore::StringFormat("zone entry: {} at ({:.0f}, {:.0f}) map {}, {:.0f} yd", SpawnName(entry), entry.x,
                    entry.y, uint32(entry.mapId), entry.mapId == player->GetMapId() ? Dist2d(player, entry) : -1.0f));
            else
                lines.push_back("zone entry: the zone's hub (no quest to take there)");
        }
        lines.push_back(Acore::StringFormat("quest log {} free, bags {} free, durability {}%, money {}c",
            FreeQuestSlots(player), player->GetFreeInventorySpace(), MinDurabilityPercent(player), player->GetMoney()));
        lines.push_back(Acore::StringFormat("at ({:.1f}, {:.1f}, {:.1f}) map {}, {}{}{}{}{}{}{}hp {:.0f}%", player->GetPositionX(),
            player->GetPositionY(), player->GetPositionZ(), player->GetMapId(), player->IsInCombat() ? "in combat, " : "",
            player->isMoving() ? "moving, " : "", player->IsSitState() ? "sitting, " : "",
            player->GetTransport() ? Acore::StringFormat("on {}, ", player->GetTransport()->GetName()) : std::string(),
            !player->IsAlive() ? (player->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? "ghost, " : "dead, ") : "",
            player->IsBeingTeleportedNear() ? "teleporting (near), " : "", player->IsBeingTeleportedFar() ? "teleporting (far), " : "",
            player->GetHealthPct()));
        lines.push_back(BotDriver::DescribeAI(player));
    }

    uint32 blocked = 0, retry = 0;
    for (auto const& [questId, block] : bot->questBlocks)
        (block.blocked ? blocked : retry) += 1;
    lines.push_back(Acore::StringFormat("xp/h {}, copper/h {}, quests blocked {}, on their retry {}, jumps {}", GetXpPerHour(guid),
        GetMoneyPerHour(guid), blocked, retry, rt.jumps));
    lines.push_back(Acore::StringFormat("since login: {} fights ({} against 2+ foes, {} begun below half health, {} left in time), "
        "{} rests ({} s), {} deaths; a fight against one foe of its level costs it {:.0f}% (health and half the mana), such a foe "
        "takes {:.1f}% of its health per second, its runs cost {:.1f} times the estimate{}", rt.total.fights,
        rt.total.crowded, rt.total.weakStarts, rt.total.escapes, rt.total.rests, rt.total.restSeconds, rt.total.deaths,
        rt.fightCost * 100.0f, rt.fightHealthRate * 100.0f, rt.runFactor,
        rt.rest.active ? "; resting now" : (rt.escape.active ? "; running from a fight now" : "")));
    if (std::string const costly = Escape::DescribeFoeCosts(rt); !costly.empty())
        lines.push_back("kinds of creature that cost it more (against one of its level): " + costly);
    if (player)
        DescribeErrands(*bot, player, rt, lines);

    uint32 const now = getMSTime();
    uint8 const level = player ? player->GetLevel() : 0;
    for (auto const& [questId, danger] : rt.dangerousQuests)
        if (player && rt.IsDangerous(questId, level))
            lines.push_back(Acore::StringFormat("set aside: quest {} \"{}\" until level {} or {} s", questId, QuestTitle(questId),
                uint32(danger.level) + 1, (danger.until - now) / IN_MILLISECONDS));
    for (BotRuntime::AvoidArea const& area : rt.avoidAreas)
        if (player && level <= area.level && now < area.until)
            lines.push_back(Acore::StringFormat("set aside: area ({:.0f}, {:.0f}) map {} ({}) for {} s", area.x, area.y, area.mapId,
                area.deaths ? Acore::StringFormat("deaths, until level {}", uint32(area.level) + 1) : std::string("unreachable"),
                (area.until - now) / IN_MILLISECONDS));
    for (auto const& [zoneId, until] : rt.unreachableZones)
        if (now < until)
            lines.push_back(Acore::StringFormat("set aside: zone {} (stuck on the way) for {} s", ZoneName(zoneId),
                (until - now) / IN_MILLISECONDS));
    return lines;
}

std::vector<std::pair<std::string, std::string>> OrderService::DescribeNext(ObjectGuid guid) const
{
    std::vector<std::pair<std::string, std::string>> lines;
    AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(guid);
    GuildBot const* bot = guild ? guild->FindPlanned(guid) : nullptr;
    if (!bot)
    {
        lines.emplace_back("now", guild ? "It has no order: it holds its position." : "Not a guild character.");
        return lines;
    }
    // A character its owner plays by hand: no plan, but its quest log and upkeep all the same.
    Player* planned = sGuildBotHolder.GetPlannedPlayer(guid);
    Player* player = planned ? planned : ObjectAccessor::FindPlayer(guid);
    if (!player)
    {
        lines.emplace_back("now", "It is not in the world.");
        return lines;
    }
    static BotRuntime const noRuntime;
    auto itr = _runtime.find(guid);
    BotRuntime const& rt = itr != _runtime.end() ? itr->second : noRuntime;
    bool const byHand = !planned;
    uint32 const now = getMSTime();
    uint8 const level = player->GetLevel();
    auto minutes = [](uint32 ms) { return Acore::StringFormat("{}:{:02}", ms / MINUTE / IN_MILLISECONDS, ms / IN_MILLISECONDS % MINUTE); };

    lines.emplace_back("now", byHand ? std::string("You play it yourself: the AI plans nothing for it.") :
        itr == _runtime.end() ? std::string("The planner has not looked at it yet.") :
        rt.why.empty() ? std::string("Thinking about its next step.") : rt.why);

    // The task it works on.
    if (BotTask const& task = rt.task; !byHand && task.kind != TaskKind::None)
    {
        std::string text = ToString(task.kind);
        if (!text.empty())
            text[0] = char(std::toupper(static_cast<unsigned char>(text[0])));
        if (task.questId)
            text += Acore::StringFormat(": \"{}\"", QuestTitle(task.questId));
        if (!task.targetName.empty())
            text += (task.questId ? ", " : ": ") + task.targetName;
        if (task.kind == TaskKind::Voyage)
            text += Acore::StringFormat(" ({}, {})", task.link.name, ToString(task.voyage));
        if (task.hasSpot && task.spot.mapId == player->GetMapId())
            text += Acore::StringFormat(", {:.0f} yd away", Dist2d(player, task.spot));
        text += ", for " + minutes(GetMSTimeDiffToNow(task.startedAt));
        if (task.deaths)
            text += Acore::StringFormat(", {} death{}", task.deaths, task.deaths == 1 ? "" : "s");
        lines.emplace_back("task", text);
    }

    // Its squad and the dungeon run it is on.
    if (SquadState const squad = SquadState(rt.squad.state); !byHand && squad != SquadState::None)
    {
        std::string members;
        for (ObjectGuid const& member : sSquadService.GetMembers(guid))
            if (GuildBot const* memberBot = guild->FindBot(member))
                members += (members.empty() ? "" : ", ") + memberBot->name;
        GuildBot const* leaderBot = guild->FindBot(sSquadService.GetLeader(guid));
        lines.emplace_back("squad", Acore::StringFormat("Squad: {}{}{}{}", GuildAdventure::Describe(squad),
            leaderBot ? ", follows " + leaderBot->name : "", members.empty() ? "" : ", leads " + members,
            rt.squad.waitFor.empty() ? "" : ", waits for " + rt.squad.waitFor));
    }
    if (!byHand && rt.squad.partnerQuest)
        lines.emplace_back("squad", Acore::StringFormat("Quest partner for \"{}\"{}", QuestTitle(rt.squad.partnerQuest),
            rt.squad.meeting ? " (on its way to its partner)" : ""));
    if (!byHand && rt.dungeon.id)
        if (auto const* plan = sDungeons.GetPlan(rt.dungeon.id))
            lines.emplace_back("run", Acore::StringFormat("Dungeon run: {}{}", plan->name, rt.dungeon.phase == 2 ? " (inside)" : ""));
    // A member: what its leader does and why it waits (the owner, 2026-11-27, watching a member in a raid: [quote left out]), and the run it leads.
    if (ObjectGuid const leaderGuid = sSquadService.GetLeader(guid); !byHand && !leaderGuid.IsEmpty() && leaderGuid != guid)
        if (auto const leadItr = _runtime.find(leaderGuid); leadItr != _runtime.end())
        {
            BotRuntime const& lead = leadItr->second;
            std::string const leaderName = guild->FindBot(leaderGuid) ? guild->FindBot(leaderGuid)->name :
                leaderGuid == guild->leader ? guild->leaderPlan.name : std::string("its leader");
            if (std::string const what = LeaderWhyForMember(&lead); !what.empty())
                lines.emplace_back("wait", leaderName + ": " + what);
            if (!rt.dungeon.id && lead.dungeon.id)
                if (auto const* plan = sDungeons.GetPlan(lead.dungeon.id))
                    lines.emplace_back("run", Acore::StringFormat("Dungeon run: {}{}, led by {}", plan->name,
                        lead.dungeon.phase == 2 ? " (inside)" : "", leaderName));
        }
    if (!byHand && rt.errandQuest)
        lines.emplace_back("errand", Acore::StringFormat("Errand: hand in \"{}\" at {}", QuestTitle(rt.errandQuest), rt.errandName));

    // The quest log: what waits to be handed in first, then what is left to do, with what keeps a quest waiting.
    std::vector<uint32> const partnerWait = sQuestPartners.Waiting(*bot, player);
    std::vector<std::pair<std::string, std::string>> open;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = player->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest)
            continue;
        QuestStatus const status = player->GetQuestStatus(questId);
        std::string const title = Acore::StringFormat("\"{}\" (level {})", QuestTitle(questId), quest->GetQuestLevel());
        if (status == QUEST_STATUS_COMPLETE)
        {
            lines.emplace_back("hand", "Hand in " + title + (!byHand && questId == rt.task.questId && rt.task.kind == TaskKind::TurnIn ?
                " (now)" : ""));
            continue;
        }
        if (status == QUEST_STATUS_FAILED)
        {
            open.emplace_back("quest", title + ": failed, it gives it back");
            continue;
        }
        // Progress: creatures and objects, then items.
        std::string progress;
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredNpcOrGo[i] && quest->RequiredNpcOrGoCount[i])
                progress += Acore::StringFormat("{}{}/{}", progress.empty() ? "" : ", ",
                    std::min<uint32>(player->GetQuestSlotCounter(slot, i), quest->RequiredNpcOrGoCount[i]), quest->RequiredNpcOrGoCount[i]);
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredItemId[i] && quest->RequiredItemCount[i])
                progress += Acore::StringFormat("{}{}/{}", progress.empty() ? "" : ", ",
                    std::min<uint32>(player->GetItemCount(quest->RequiredItemId[i]), quest->RequiredItemCount[i]), quest->RequiredItemCount[i]);
        std::string note;
        if (rt.IsDangerous(questId, level))
        {
            auto danger = rt.dangerousQuests.find(questId);
            note = danger != rt.dangerousQuests.end() ? Acore::StringFormat("waits until level {} or {}", uint32(danger->second.level) + 1,
                minutes(danger->second.until > now ? danger->second.until - now : 0)) : std::string("waits");
        }
        else if (std::find(partnerWait.begin(), partnerWait.end(), questId) != partnerWait.end())
            note = "waits for a quest partner";
        else if (!byHand && questId == rt.task.questId)
            note = "now";
        open.emplace_back("quest", title + (progress.empty() ? "" : ": " + progress) + (note.empty() ? "" : " - " + note));
    }
    lines.insert(lines.end(), open.begin(), open.end());

    // Upkeep and what it keeps away from.
    lines.emplace_back("upkeep", Acore::StringFormat("Bags {} free, gear at {}%, quest log {} free, money {}g {}s", player->GetFreeInventorySpace(),
        MinDurabilityPercent(player), FreeQuestSlots(player), player->GetMoney() / GOLD, player->GetMoney() % GOLD / SILVER));
    for (auto const& [zoneId, until] : rt.unreachableZones)
        if (now < until)
            lines.emplace_back("aside", Acore::StringFormat("Zone {} waits for {}", ZoneName(zoneId), minutes(until - now)));
    uint32 places = 0;
    for (BotRuntime::AvoidArea const& area : rt.avoidAreas)
        if (level <= area.level && now < area.until)
            ++places;
    if (places)
        lines.emplace_back("aside", Acore::StringFormat("{} place{} set aside (deaths, packs, no way)", places, places == 1 ? "" : "s"));
    return lines;
}
