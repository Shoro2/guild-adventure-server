/*
 * Fishing: see Fishing.h and docs/orders.md "Fishing". The places, and the task of OrderService that walks to the
 * water, puts the pole into the bot's hand, casts, and takes the catch when the bobber splashes.
 */

#include "MapMgr.h"
#include "Fishing.h"
#include "Bag.h"
#include "BotDriver.h"
#include "BotSettings.h"
#include "GameObject.h"
#include "GuildAdventureMgr.h"
#include "Item.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "OrderService.h"
#include "MotionMaster.h"
#include "PathProbe.h"
#include "Player.h"
#include "Professions.h"
#include "QuestDef.h"
#include "RiskModel.h"
#include "Spell.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>

using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";

    // The ranks of the fishing spell, highest first.
    constexpr uint32 FishingSpells[] = { 51294, 33095, 18248, 7732, 7731, 7620 };
    // The bobber lands 10 to 20 yd ahead of the caster: water has to be there.
    constexpr float CastNear = 11.0f;
    constexpr float CastFar = 19.0f;
    // Water this deep is fished in; a place to stand is looked for this far around a place at the water.
    constexpr float MinDepth = 1.0f;
    constexpr float ShoreReach = 70.0f;
    // The bot stands this close to its spot; between two casts; a cast that does not start this often ends the task.
    constexpr float StandDistance = 2.5f;
    constexpr uint32 CastGapMs = 2 * IN_MILLISECONDS;
    constexpr uint32 MaxFailedCasts = 4;
    // A fishing task lasts this long at most; for skill it ends after this many casts.
    constexpr uint32 FishMaxMs = 8 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 SkillCasts = 25;
    // A place where no spot to stand was found, or that was not reached, is left alone for this long.
    constexpr uint32 PlaceGapMs = 20 * MINUTE * IN_MILLISECONDS;
    // Places for skill are looked for this far (walks), and the nearest few are looked at closely.
    constexpr float SkillPlaceDistance = 2500.0f;
    // The water next to the bot is looked for this far, and after a miss not again for this long.
    constexpr float NearShoreReach = 45.0f;
    // At a hub after a hand-in the water may be a short walk away (a town's canal, its pier).
    constexpr float HubShoreReach = 120.0f;
    constexpr uint32 NearShoreLookMs = 3 * MINUTE * IN_MILLISECONDS;
    constexpr std::size_t PlaceChecks = 12;

    float Dist2d(Player const* player, SpawnPoint const& spot)
    {
        float const dx = player->GetPositionX() - spot.x, dy = player->GetPositionY() - spot.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    bool IsPole(Item const* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        return proto && proto->Class == ITEM_CLASS_WEAPON && proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE;
    }

    // The water at a point: its surface when it is deep enough to fish in, INVALID_HEIGHT else.
    float SurfaceAt(Player const* player, float x, float y, float z)
    {
        LiquidData const liquid = player->GetMap()->GetLiquidData(player->GetPhaseMask(), x, y, z, player->GetCollisionHeight(), {});
        if (liquid.Level <= INVALID_HEIGHT || liquid.Status == LIQUID_MAP_NO_WATER)
            return INVALID_HEIGHT;
        // (Nothing is caught in lava or slime: the first journey to "water" went to the Great Forge of Ironforge.)
        if (liquid.Flags & (MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME))
            return INVALID_HEIGHT;
        float const ground = player->GetMap()->GetHeight(player->GetPhaseMask(), x, y, liquid.Level + 2.0f, true, 60.0f);
        if (ground > INVALID_HEIGHT && liquid.Level - ground < MinDepth)
            return INVALID_HEIGHT;
        return liquid.Level;
    }

    // From a standing point: a direction in which the bobber's whole landing stretch is water.
    bool WaterAhead(Player const* player, float x, float y, float z, float& orientation)
    {
        constexpr int Directions = 16;
        for (int i = 0; i < Directions; ++i)
        {
            float const angle = 2.0f * float(M_PI) * float(i) / float(Directions);
            bool water = true;
            float surface = INVALID_HEIGHT;
            for (float distance : { CastNear, (CastNear + CastFar) / 2.0f, CastFar })
            {
                surface = SurfaceAt(player, x + std::cos(angle) * distance, y + std::sin(angle) * distance, z);
                // (Not from a cliff: the water a few yards below the feet at most.)
                if (surface <= INVALID_HEIGHT || surface < z - 8.0f || surface > z + 2.0f)
                {
                    water = false;
                    break;
                }
            }
            if (water)
            {
                orientation = angle;
                return true;
            }
        }
        return false;
    }

    // Takes everything out of the loot the character has open; true when something was taken.
    bool TakeCatch(Player* player, GameObject* bobber)
    {
        Loot* loot = &bobber->loot;
        bool taken = false;
        uint32 const slots = loot->GetMaxSlotInLootFor(player);
        for (uint32 i = 0; i < slots; ++i)
        {
            if (!loot->LootItemInSlot(i, player))
                continue;
            InventoryResult msg = EQUIP_ERR_OK;
            player->StoreLootItem(uint8(i), loot, msg);
            taken = taken || msg == EQUIP_ERR_OK;
        }
        return taken;
    }
}

uint32 Fishing::SpellOf(Player const* player)
{
    for (uint32 spell : FishingSpells)
        if (player->HasSpell(spell))
            return spell;
    return 0;
}

Item* Fishing::FindPole(Player const* player)
{
    if (Item* hand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND); IsPole(hand))
        return hand;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); IsPole(item))
            return item;
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = player->GetBagByPos(bagSlot))
            for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                if (Item* item = bag->GetItemByPos(uint8(i)); IsPole(item))
                    return item;
    return nullptr;
}

bool Fishing::CanFish(Player const* player)
{
    // (The pole comes with the trip: OrderService::StepFishing.)
    return SpellOf(player) != 0;
}

uint32 Fishing::CatchChance(Player const* player, uint32 zoneId, uint32 areaId)
{
    // GameObject::Use, GAMEOBJECT_TYPE_FISHINGNODE.
    int32 zoneSkill = areaId ? sObjectMgr->GetFishingBaseSkillLevel(areaId) : 0;
    if (!zoneSkill)
        zoneSkill = sObjectMgr->GetFishingBaseSkillLevel(zoneId);
    int32 const noMiss = zoneSkill + 95;
    int32 const skill = player->GetSkillValue(SKILL_FISHING);
    if (skill >= noMiss || noMiss <= 0)
        return 100;
    double const share = double(skill) / double(noMiss);
    return uint32(std::max(1.0, share * share * 100.0));
}

bool Fishing::NeedsFishing(Quest const* quest)
{
    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (uint32 const item = quest->RequiredItemId[i])
            if (!sProfessions.GetFishZones(item).empty() && sWorldIndex.GetItemSources(item).empty())
                return true;
    return false;
}

bool Fishing::IsQuietWater(Player const* player, SpawnPoint const& place)
{
    // (As for a fire or an anvil, OrderService::PlanFocusTrip: a level-27 priest was overrun by murlocs of level 12
    // to 15 at Westfall's coast, creatures the risk verdict leaves out as far below it.)
    constexpr float QuietYards = 45.0f;
    constexpr uint32 MostHostile = 1;
    return sWorldIndex.CountHostileSpawns(player, place.mapId, place.x, place.y, place.z, QuietYards) <= MostHostile;
}

std::optional<SpawnPoint> Fishing::PlaceFor(Player* player, BotRuntime const& rt, uint32 item)
{
    std::vector<uint32> const& zones = sProfessions.GetFishZones(item);
    if (zones.empty())
        return std::nullopt;
    auto fits = [&](uint32 zone, uint32 area)
    {
        return std::find(zones.begin(), zones.end(), zone) != zones.end() || std::find(zones.begin(), zones.end(), area) != zones.end();
    };
    // (With a skill that brings up junk nine times in ten the quest waits: the fish come with the skill.)
    constexpr uint32 MinQuestCatchChance = 25;
    // Where it stands, when that is such a zone: the water next to it.
    if (fits(player->GetZoneId(), player->GetAreaId()) && CatchChance(player, player->GetZoneId(), player->GetAreaId()) >= MinQuestCatchChance)
    {
        SpawnPoint here;
        here.mapId = uint16(player->GetMapId());
        here.x = player->GetPositionX();
        here.y = player->GetPositionY();
        here.z = player->GetPositionZ();
        if (!rt.IsInAvoidedArea(here.mapId, here.x, here.y, player->GetLevel()))
            return here;
    }
    // Else the nearest school of fish of such a zone (it marks water), else the zone's hub.
    std::optional<SpawnPoint> best;
    float bestDistance = 0.0f;
    for (SpawnPoint const& pool : sProfessions.GetFishingPools(player->GetMapId()))
    {
        float const distance = Dist2d(player, pool);
        if ((best && distance >= bestDistance) || rt.IsAvoided(pool))
            continue;
        uint32 const poolZone = sWorldIndex.GetSpawnZone(pool);
        if (!fits(poolZone, 0) || CatchChance(player, poolZone, 0) < MinQuestCatchChance)
            continue;
        best = pool;
        bestDistance = distance;
    }
    if (best)
        return best;
    for (uint32 zone : zones)
        if (ZoneHub const* hub = sWorldIndex.GetHub(zone, player->GetTeamId()); hub && hub->mapId == player->GetMapId() &&
            CatchChance(player, zone, 0) >= MinQuestCatchChance)
        {
            SpawnPoint spot;
            spot.mapId = uint16(hub->mapId);
            spot.x = hub->x;
            spot.y = hub->y;
            spot.z = hub->z;
            return spot;
        }
    return std::nullopt;
}

std::optional<SpawnPoint> Fishing::PlaceForSkill(Player* player, BotRuntime const& rt)
{
    int32 const skill = player->GetSkillValue(SKILL_FISHING);
    uint8 const level = player->GetLevel();
    std::vector<std::pair<float, SpawnPoint const*>> candidates;
    for (SpawnPoint const& pool : sProfessions.GetFishingPools(player->GetMapId()))
    {
        float const distance = Dist2d(player, pool);
        if (distance > SkillPlaceDistance || rt.IsAvoided(pool) || rt.IsInAvoidedArea(pool.mapId, pool.x, pool.y, level))
            continue;
        candidates.emplace_back(distance, &pool);
    }
    std::size_t const looked = std::min(candidates.size(), PlaceChecks);
    std::partial_sort(candidates.begin(), candidates.begin() + looked, candidates.end(),
        [](auto const& a, auto const& b) { return a.first < b.first; });
    for (std::size_t i = 0; i < looked; ++i)
    {
        SpawnPoint const& pool = *candidates[i].second;
        // Water its skill catches something in (the zone's skill, as the bobber asks), not in a zone above the bot, nor
        // at a place the risk rules keep it from.
        uint32 const zoneId = sWorldIndex.GetSpawnZone(pool);
        if (sObjectMgr->GetFishingBaseSkillLevel(zoneId) > skill)
            continue;
        ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(zoneId);
        if ((zone && zone->minLevel > level) || !Risk::JudgeSpot(player, rt, pool).usable || !IsQuietWater(player, pool))
            continue;
        return pool;
    }
    return std::nullopt;
}

std::optional<SpawnPoint> Fishing::WaterForSkill(Player* player, BotRuntime const& rt, std::string& name)
{
    constexpr std::size_t Places = 10;     // how many inns and flight masters are looked at (terrain lookups)
    // (Water right by the inn or the flight master: 150 yd found a lake of Ironforge behind a wall of the Great Forge,
    // 134 yd away and a walk around the city's ring.)
    constexpr float ShoreReach = 70.0f;
    int32 const skill = player->GetSkillValue(SKILL_FISHING);
    uint8 const level = player->GetLevel();
    Region const here = GetRegion(player);
    // A school of fish marks open water that is fished: the nearest one on its land mass in a zone its skill reaches,
    // however far (the place on the shore is found when the bot gets there, as for the schools near it).
    {
        SpawnPoint const* best = nullptr;
        float bestDistance = 0.0f;
        std::vector<std::pair<float, SpawnPoint const*>> pools;
        for (SpawnPoint const& pool : sProfessions.GetFishingPools(player->GetMapId()))
            if (!rt.IsAvoided(pool) && !rt.IsInAvoidedArea(pool.mapId, pool.x, pool.y, level))
                pools.emplace_back(player->GetExactDist2d(pool.x, pool.y), &pool);
        std::size_t const lookedPools = std::min<std::size_t>(pools.size(), 60);
        std::partial_sort(pools.begin(), pools.begin() + lookedPools, pools.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        for (std::size_t i = 0; i < lookedPools && !best; ++i)
        {
            SpawnPoint const& pool = *pools[i].second;
            uint32 const zoneId = sWorldIndex.GetSpawnZone(pool);
            int32 const base = sObjectMgr->GetFishingBaseSkillLevel(zoneId);
            if (!base || base > skill || sWorldIndex.GetSpawnRegion(pool) != here)
                continue;
            ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(zoneId);
            if ((zone && zone->minLevel > level) || !Risk::JudgeSpot(player, rt, pool).usable || !IsQuietWater(player, pool))
                continue;
            best = &pool;
            bestDistance = pools[i].first;
            name = zone ? "a school of fish in " + std::string(zone->name) : std::string("a school of fish");
        }
        if (best)
        {
            (void)bestDistance;
            return *best;
        }
    }
    std::vector<std::pair<float, ServicePoint const*>> candidates;
    for (ServiceType type : { ServiceType::Innkeeper, ServiceType::FlightMaster })
        for (ServicePoint const& service : sWorldIndex.GetServices(type))
        {
            SpawnPoint const& spawn = service.spawn;
            if (spawn.mapId != player->GetMapId() || rt.IsAvoided(spawn) || rt.IsInAvoidedArea(spawn.mapId, spawn.x, spawn.y, level))
                continue;
            // Water its skill catches something in (the area's level, else the zone's, as the bobber asks).
            uint32 const zoneId = sWorldIndex.GetSpawnZone(spawn);
            uint32 const areaId = sMapMgr->GetAreaId(PHASEMASK_NORMAL, spawn.mapId, spawn.x, spawn.y, spawn.z);
            int32 base = sObjectMgr->GetFishingBaseSkillLevel(areaId);
            if (!base)
                base = sObjectMgr->GetFishingBaseSkillLevel(zoneId);
            if (!base || base > skill)
                continue;
            ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(zoneId);
            if (zone && zone->minLevel > level)
                continue;
            candidates.emplace_back(player->GetExactDist2d(spawn.x, spawn.y), &service);
        }
    std::size_t const looked = std::min(candidates.size(), Places);
    std::partial_sort(candidates.begin(), candidates.begin() + looked, candidates.end(),
        [](auto const& a, auto const& b) { return a.first < b.first; });
    for (std::size_t i = 0; i < looked; ++i)
    {
        ServicePoint const& service = *candidates[i].second;
        if (sWorldIndex.GetSpawnRegion(service.spawn) != here)
            continue;
        SpawnPoint stand;
        float orientation = 0.0f;
        if (!FindShore(player, service.spawn.x, service.spawn.y, service.spawn.z, ShoreReach, stand, orientation, true) ||
            !Risk::JudgeSpot(player, rt, stand).usable || !IsQuietWater(player, stand))
            continue;
        name = service.name;
        return stand;
    }
    return std::nullopt;
}

bool Fishing::FindShore(Player* player, float x, float y, float z, float reach, SpawnPoint& stand, float& orientation, bool fromPlace)
{
    constexpr int Directions = 12;
    constexpr float Ring = 6.0f;
    Map* map = player->GetMap();
    bool found = false;
    float best = 0.0f;
    for (float radius = 0.0f; radius <= reach; radius += Ring)
    {
        for (int i = 0; i < (radius > 0.0f ? Directions : 1); ++i)
        {
            float const angle = 2.0f * float(M_PI) * float(i) / float(Directions);
            float const px = x + std::cos(angle) * radius, py = y + std::sin(angle) * radius;
            float const ground = map->GetHeight(player->GetPhaseMask(), px, py, z + 15.0f, true, 60.0f);
            if (ground <= INVALID_HEIGHT)
                continue;
            // Dry ground (ankle-deep at most).
            LiquidData const liquid = map->GetLiquidData(player->GetPhaseMask(), px, py, ground, player->GetCollisionHeight(), {});
            if (liquid.Level > INVALID_HEIGHT && liquid.Status != LIQUID_MAP_NO_WATER && liquid.Level - ground > 0.5f)
                continue;
            float const walk = fromPlace ? std::sqrt((px - x) * (px - x) + (py - y) * (py - y)) : player->GetExactDist(px, py, ground);
            if (found && walk >= best)
                continue;
            float ahead = 0.0f;
            if (!WaterAhead(player, px, py, ground, ahead))
                continue;
            if (walk > StandDistance && !(fromPlace ? WalkReaches(player, x, y, z, px, py, ground) :
                    WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), px, py, ground)))
                continue;
            found = true;
            best = walk;
            stand.mapId = uint16(player->GetMapId());
            stand.x = px;
            stand.y = py;
            stand.z = ground;
            orientation = ahead;
        }
        // (The first ring with a spot is near enough to the place; farther rings only when it has none.)
        if (found)
            break;
    }
    return found;
}

// ---------------------------------------------------------------------------------------------------------------
// The task
// ---------------------------------------------------------------------------------------------------------------

void OrderService::RestoreWeapon(Player* player, BotRuntime& rt)
{
    if (!rt.fishWeapon)
        return;
    Item* weapon = player->GetItemByGuid(rt.fishWeapon);
    Item* hand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (weapon && !weapon->IsEquipped() && (!hand || IsPole(hand)))
        player->SwapItem(weapon->GetPos(), uint16(INVENTORY_SLOT_BAG_0) << 8 | EQUIPMENT_SLOT_MAINHAND);
    rt.fishWeapon.Clear();
}

bool OrderService::PlanFishing(GuildBot const& bot, Player* player, BotRuntime& rt, uint32 nearCasts)
{
    // `nearCasts` (AUTO, at a hub after a hand-in): only at water next to the bot, and only that many casts.
    if (!Professions::HasWork(bot, player, SKILL_FISHING) || !Fishing::CanFish(player))
        return false;
    BotTask task;
    task.kind = TaskKind::Fish;
    task.hasSpot = true;
    task.targetName = "the water";
    // The water next to where it stands (a canal, a pond by the inn), when its skill catches something there; looked
    // for once in a while (many height and water lookups).
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    if (now >= rt.nextShoreLook)
    {
        rt.nextShoreLook = now + NearShoreLookMs;
        int32 baseSkill = sObjectMgr->GetFishingBaseSkillLevel(player->GetAreaId());
        if (!baseSkill)
            baseSkill = sObjectMgr->GetFishingBaseSkillLevel(player->GetZoneId());
        float orientation = 0.0f;
        if (baseSkill <= int32(player->GetSkillValue(SKILL_FISHING)) &&
            Fishing::FindShore(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                nearCasts ? HubShoreReach : NearShoreReach, task.spot, orientation) &&
            !rt.IsInAvoidedArea(task.spot.mapId, task.spot.x, task.spot.y, player->GetLevel()) && Fishing::IsQuietWater(player, task.spot))
        {
            task.fishStand = true;
            task.fishOrientation = orientation;
            rt.nextShoreLook = 0;
        }
    }
    if (!task.fishStand)
    {
        if (nearCasts)
            return false;
        std::optional<SpawnPoint> place = Fishing::PlaceForSkill(player, rt);
        if (!place)
        {
            // No water its skill reaches within a walk: a journey to some (looked for every few minutes: terrain
            // lookups around ten places). The profession mode's travel takes it there; the casts begin when it stands
            // at the water.
            constexpr uint32 WaterLookMs = 5 * MINUTE * IN_MILLISECONDS;
            if (now < rt.nextWaterLook)
                return false;
            rt.nextWaterLook = now + WaterLookMs;
            std::string name;
            std::optional<SpawnPoint> water = Fishing::WaterForSkill(player, rt, name);
            if (!water)
                return false;
            BotTask trip;
            trip.kind = TaskKind::Travel;
            trip.gatherRound = true;
            trip.fishTrip = true;
            trip.hasSpot = true;
            trip.spot = *water;
            trip.targetName = "the water by " + name;
            trip.fishNear = water->gameObject; // a school of fish: the place on the shore is found from near it
            StartTask(player, rt, trip);
            LOG_INFO(ActivityLog, "activity: {} travels to the water by {} to fish for its profession ({:.0f} yd; Fishing {} of {}: no water "
                "its skill reaches is nearer)", bot.name, name, player->GetExactDist2d(water->x, water->y),
                player->GetPureSkillValue(SKILL_FISHING), player->GetPureMaxSkillValue(SKILL_FISHING));
            rt.why = Acore::StringFormat("Profession mode: travelling to the water by {} to fish (Fishing {} of {})", name,
                player->GetPureSkillValue(SKILL_FISHING), player->GetPureMaxSkillValue(SKILL_FISHING));
            return true;
        }
        task.spot = *place;
    }
    task.fishCastLimit = nearCasts;
    StartTask(player, rt, task);
    if (nearCasts)
    {
        LOG_INFO(ActivityLog, "activity: {} fishes at the water by the hub for its profession: {} casts (Fishing {} of {})", bot.name,
            nearCasts, player->GetPureSkillValue(SKILL_FISHING), player->GetPureMaxSkillValue(SKILL_FISHING));
        rt.why = Acore::StringFormat("Fishing at the water here for its profession (Fishing {} of {})",
            player->GetPureSkillValue(SKILL_FISHING), player->GetPureMaxSkillValue(SKILL_FISHING));
    }
    else
        rt.why = Acore::StringFormat("Profession mode: going fishing (Fishing {} of {})", player->GetPureSkillValue(SKILL_FISHING),
            player->GetPureMaxSkillValue(SKILL_FISHING));
    return true;
}

bool OrderService::StepFishing(GuildAdventure::AdventureGuild& /*guild*/, GuildBot& bot, Player* player, BotRuntime& rt)
{
    BotTask& task = rt.task;
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    uint32 const spell = Fishing::SpellOf(player);
    Item* pole = spell ? Fishing::FindPole(player) : nullptr;
    // Its pole comes with the trip, not with the profession: mod-playerbots sells a pole at the next vendor (a weapon
    // the bot does not fight with), and a bot that got one with its profession got a new one after every vendor visit.
    if (spell && !pole)
    {
        constexpr uint32 FishingPole = 6256;
        if (player->AddItem(FishingPole, 1))
            LOG_INFO(ActivityLog, "activity: {} got a fishing pole for its trip", bot.name);
        pole = Fishing::FindPole(player);
    }
    if (!spell || !pole)
    {
        ClearTask(rt, 0, "no fishing pole");
        return false;
    }
    // What it is here for: the fish of a quest, or its skill.
    Quest const* quest = task.questId ? sObjectMgr->GetQuestTemplate(task.questId) : nullptr;
    if (task.questId)
    {
        bool open = false;
        if (quest && player->GetQuestStatus(task.questId) == QUEST_STATUS_INCOMPLETE)
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i] == task.fishItem && player->GetItemCount(task.fishItem) < quest->RequiredItemCount[i])
                    open = true;
        if (!open)
        {
            ClearTask(rt, 0, "objective done");
            return false;
        }
    }
    else if (task.fishCasts >= (task.fishCastLimit ? task.fishCastLimit : SkillCasts) || !Professions::HasWork(bot, player, SKILL_FISHING))
    {
        ClearTask(rt, 0, "fished");
        return false;
    }
    if (now - task.startedAt > FishMaxMs || player->GetFreeInventorySpace() < 2)
    {
        if (task.questId && task.hasSpot)
            rt.Avoid(task.spot, PlaceGapMs);
        ClearTask(rt, 0, player->GetFreeInventorySpace() < 2 ? "bags full" : "fishing time over");
        return false;
    }
    // Attacked: its weapon back into its hand; the fight is playerbots'.
    if (player->IsInCombat())
    {
        RestoreWeapon(player, rt);
        return true;
    }

    // The spot on the shore, once the bot is near the place.
    float const distance = Dist2d(player, task.spot);
    if (!task.fishStand)
    {
        if (distance > ShoreReach / 2.0f)
        {
            if (UpdateStuck(player, rt, task.spot.x, task.spot.y))
            {
                rt.Avoid(task.spot, PlaceGapMs);
                rt.AddAvoidArea(task.spot.mapId, task.spot.x, task.spot.y, 255, PlaceGapMs, false);
                ClearTask(rt, 5 * IN_MILLISECONDS, "water not reached");
                return false;
            }
            WalkTo(player, rt, task.spot.mapId, task.spot.x, task.spot.y, task.spot.z);
            SpawnPoint const place = task.spot;
            if (LeaveDangerousWalk(bot, player, rt, sBotSettings.Get(player, Setting::WalksMaxLevelsAbove), true))
            {
                // (The place, not only its spawn: several schools of fish share one spot.)
                rt.AddAvoidArea(place.mapId, place.x, place.y, 255, PlaceGapMs, false);
                return false;
            }
            return true;
        }
        SpawnPoint stand;
        float orientation = 0.0f;
        SpawnPoint const place = task.spot;
        if (!Fishing::FindShore(player, place.x, place.y, player->GetPositionZ(), ShoreReach, stand, orientation))
        {
            LOG_INFO(ActivityLog, "activity: {} finds no spot to fish from at ({:.0f}, {:.0f}) map {}", bot.name, place.x, place.y,
                uint32(place.mapId));
            rt.Avoid(place, PlaceGapMs);
            rt.AddAvoidArea(place.mapId, place.x, place.y, 255, PlaceGapMs, false);
            ClearTask(rt, 5 * IN_MILLISECONDS, "no spot at the water");
            return false;
        }
        // Not among hostile creatures, whatever their level: it stands there for minutes.
        if (!Fishing::IsQuietWater(player, stand))
        {
            LOG_INFO(ActivityLog, "activity: {} does not fish at ({:.0f}, {:.0f}) map {}: hostile creatures live at that water", bot.name,
                stand.x, stand.y, uint32(stand.mapId));
            rt.Avoid(place, PlaceGapMs);
            rt.AddAvoidArea(place.mapId, place.x, place.y, 255, PlaceGapMs, false);
            ClearTask(rt, 5 * IN_MILLISECONDS, "hostile creatures at the water");
            return false;
        }
        task.spot = stand;
        task.fishOrientation = orientation;
        task.fishStand = true;
        task.walkX = task.walkY = 0.0f;
        rt.stuckSince = 0;
        return true;
    }
    if (distance > StandDistance && !player->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
    {
        if (UpdateStuck(player, rt, task.spot.x, task.spot.y))
        {
            rt.AddAvoidArea(task.spot.mapId, task.spot.x, task.spot.y, 255, PlaceGapMs, false);
            ClearTask(rt, 5 * IN_MILLISECONDS, "spot at the water not reached");
            return false;
        }
        // The last yards to the spot on the shore are the planner's own point movement along the core's path, and a spot
        // the path finder has no whole way to from here is no spot: set aside at once. (NewRpg's walk did not set off from
        // the water below a bank: a priest stood more than two minutes 12 yd from its spot, in the water, until the stuck
        // rule ended the task.)
        if (!task.standChecked)
        {
            task.standChecked = true;
            if (!WalkReaches(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), task.spot.x, task.spot.y,
                task.spot.z))
            {
                LOG_INFO(ActivityLog, "activity: {} finds no way from ({:.0f}, {:.0f}, {:.0f}) to its spot at the water at ({:.0f}, {:.0f}, "
                    "{:.0f}): the place is set aside", bot.name, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                    task.spot.x, task.spot.y, task.spot.z);
                rt.AddAvoidArea(task.spot.mapId, task.spot.x, task.spot.y, 255, PlaceGapMs, false);
                ClearTask(rt, 5 * IN_MILLISECONDS, "no way to the spot at the water");
                return false;
            }
        }
        if (now - task.lastAssert >= 3 * IN_MILLISECONDS)
        {
            BotDriver::Rest(player);
            player->GetMotionMaster()->MovePoint(0, task.spot.x, task.spot.y, task.spot.z);
            task.lastAssert = now;
        }
        return true;
    }

    // At the water: stand, the pole in hand, face the water, cast; take the catch when the bobber splashes.
    if (!task.nearSince)
        task.nearSince = now;
    PauseStuck(rt);
    if (BotDriver::GetRpgState(player) != RpgState::Rest)
        BotDriver::Rest(player);
    Item* hand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (!IsPole(hand))
    {
        if (hand && !rt.fishWeapon)
            rt.fishWeapon = hand->GetGUID();
        player->SwapItem(pole->GetPos(), uint16(INVENTORY_SLOT_BAG_0) << 8 | EQUIPMENT_SLOT_MAINHAND);
        return true;
    }
    if (player->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
    {
        GameObject* bobber = player->GetGameObject(spell);
        if (bobber && bobber->getLootState() == GO_READY)
        {
            ObjectGuid const guid = bobber->GetGUID();
            bobber->Use(player);
            if (player->GetLootGUID() == guid)
            {
                if (TakeCatch(player, bobber))
                    ++task.fishCaught;
                player->GetSession()->DoLootRelease(guid);
            }
        }
        return true;
    }
    if (now - task.fishCastAt < CastGapMs)
        return true;
    if (player->isMoving())
    {
        player->GetMotionMaster()->Clear(false);
        player->StopMoving();
        return true;
    }
    // Nobody fishes from the saddle, nor in an animal form: a bot of level 20 and more rides to the water, and every
    // cast failed "not while mounted" (cast result 64, on the live server and for both test anglers).
    if (player->IsMounted())
    {
        player->Dismount();
        player->RemoveAurasByType(SPELL_AURA_MOUNTED);
    }
    if (player->GetShapeshiftForm() != FORM_NONE && (player->getClass() == CLASS_DRUID || player->GetShapeshiftForm() == FORM_GHOSTWOLF))
        player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
    player->SetFacingTo(task.fishOrientation);
    task.fishCastAt = now;
    if (SpellCastResult const result = player->CastSpell(player, spell, false); result != SPELL_CAST_OK)
    {
        if (++task.fishFailed >= MaxFailedCasts)
        {
            LOG_INFO(ActivityLog, "activity: {} cannot cast its line at ({:.0f}, {:.0f}) map {} (spell {}, cast result {}, pole in hand {})",
                bot.name, task.spot.x, task.spot.y, uint32(task.spot.mapId), spell, uint32(result),
                IsPole(player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND)) ? "yes" : "no");
            rt.AddAvoidArea(task.spot.mapId, task.spot.x, task.spot.y, 255, PlaceGapMs, false);
            ClearTask(rt, 5 * IN_MILLISECONDS, "cannot fish here");
            return false;
        }
        return true;
    }
    task.fishFailed = 0;
    ++task.fishCasts;
    rt.why = task.questId ? Acore::StringFormat("Fishing for \"{}\" ({} of {} casts caught something)", quest ? quest->GetTitle() : "a quest",
        task.fishCaught, task.fishCasts) : Acore::StringFormat("Profession mode: fishing (Fishing {} of {}, cast {} of {})",
        player->GetPureSkillValue(SKILL_FISHING), player->GetPureMaxSkillValue(SKILL_FISHING), task.fishCasts, SkillCasts);
    return true;
}
