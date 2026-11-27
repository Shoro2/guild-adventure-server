/*
 * The order FARM in the order planner (docs/orders.md "Farm orders"). Part of OrderService; kept in a file of its own.
 *
 * The owner, 2026-11-13: the guild leader picks an item and the bot farms it on and on. The bot
 *
 *   - asks Farming::FindGrounds for a hunting ground: a group of creatures that drop the item, at least Farm.LevelsBelow
 *     levels below it, in an area it covers on foot, where the risk model lets it work;
 *   - travels there (a Travel task with flights when it is far, the hunt's own walk when it is near);
 *   - hunts there (a Grind task: the planner picks every target, only creatures that drop the item), walks to the
 *     corpse of each kill so that auto loot takes it, and the loot filter sends the item into the guild storage;
 *   - when the ground is empty it goes to the next best one and comes back once the creatures are back; with no other
 *     ground it waits there;
 *   - keeps itself supplied like under any order (PlanUpkeep: vendor trips for full bags, repairs, ammunition);
 *   - and when there is nothing it can farm the item from, the leader gets a notice (an event of the type FARM, once per
 *     cause) and the bot waits at an inn; it looks again every few minutes and after every level.
 *
 *   - with no target on its land mass it travels to the land mass that has the best ground for it (a Voyage task per
 *     leg, as any travel across the sea; the owner, 2026-11-13: [quote left out]).
 *
 * The order runs until the leader gives another one, or, when he set a target stock, until the guild storage holds
 * that many of the item (the owner, 2026-11-13: [quote left out]): then the leader is told
 * and the bot is on the autopilot again.
 */

#include "OrderService.h"
#include "BotDriver.h"
#include "BotSettings.h"
#include "Creature.h"
#include "Farming.h"
#include "Group.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureUtil.h"
#include "GuildEvents.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "StorageService.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldIndex.h"
#include <algorithm>
#include <cmath>

using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";

    // Beyond this the way to a hunting ground is a travel (flights, a route, and the checks of what lives on it);
    // nearer it is the hunt's own walk. (A ground 2400 yd away as the crow flies was 5100 yd by road, through a zone
    // of the bot's own level.)
    constexpr float FarmTravelDistance = 300.0f;
    // A ground the hunt found empty counts again after this long (the creatures are back, or the next look says no).
    constexpr uint32 FarmRespawnMs = 3 * MINUTE * IN_MILLISECONDS;
    // With nothing to farm the bot looks again this often; the leader is told again at most this often.
    constexpr uint32 FarmRetryMs = 5 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 FarmNoticeGapMs = 60 * MINUTE * IN_MILLISECONDS;
    // Waiting for a ground's creatures: a look this often.
    constexpr uint32 FarmWaitLookMs = 30 * IN_MILLISECONDS;
    // A ground the travel did not get to is set aside for this long.
    constexpr uint32 FarmUnreachedMs = 20 * MINUTE * IN_MILLISECONDS;
    // The corpse of a kill: looked for this far around the bot, walked to until this near, for at most this long.
    constexpr float CorpseLookYards = 45.0f;
    constexpr float CorpseReachYards = 3.0f;
    constexpr uint32 CorpseMaxMs = 12 * IN_MILLISECONDS;
    constexpr uint32 CorpseAssertMs = 2 * IN_MILLISECONDS;
    // The stock of the item is counted this often for a target stock.
    constexpr uint32 FarmStockLookMs = 5 * IN_MILLISECONDS;

    float Dist2d(Player const* player, SpawnPoint const& spot)
    {
        return player->GetExactDist2d(spot.x, spot.y);
    }

    std::string ItemName(uint32 item)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
        return proto ? proto->Name1 : Acore::StringFormat("item {}", item);
    }

    // What counts for a target stock: the item in the guild storage (every random property of it, its reserve included:
    // a reserve must not raise the target) and in the bot's bags.
    uint32 FarmStock(Player* player, uint32 item)
    {
        uint32 const owner = sStorageService.GetOwnerOf(player);
        return (owner ? sStorageService.GetStoredOfEntry(owner, item) : 0) + player->GetItemCount(item);
    }

    // "(143 of 200 in the guild storage)" with a target stock, else what the bot has looted under this order.
    std::string FarmProgress(Player* player, GuildBot const& bot, BotRuntime const& rt)
    {
        if (bot.order.farmTarget)
            return Acore::StringFormat("{} of {} in the guild storage", FarmStock(player, bot.order.zoneId), bot.order.farmTarget);
        return Acore::StringFormat("{} looted so far", rt.farm.looted);
    }

    // True for the problems that mean "nothing to farm" (the leader is told, the bot waits at an inn); an emptied
    // ground is no such problem.
    bool IsNothingToFarm(uint8 problem)
    {
        return problem != uint8(FarmProblem::None) && problem != uint8(FarmProblem::Waiting);
    }
}

bool OrderService::CheckFarmOrder(AdventureGuild const& guild, uint32 item, std::string& error) const
{
    return sFarming.CheckItem(item, guild.team, error);
}

std::vector<std::string> OrderService::DescribeFarm(Player* player, uint32 item) const
{
    static BotRuntime const noRuntime;
    auto itr = _runtime.find(player->GetGUID());
    return sFarming.Describe(player, itr != _runtime.end() ? itr->second : noRuntime, item);
}

void OrderService::OnFarmLoot(ObjectGuid bot, uint32 count)
{
    if (BotRuntime* rt = FindRuntime(bot))
        rt->farm.looted += count;
}

void OrderService::RunFarm(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    uint32 const item = bot.order.zoneId;
    rt.targetZone = 0; // the walks of this order end at hunting grounds, not in a zone
    if (rt.farm.item != item)
    {
        rt.farm = BotRuntime::Farm();
        rt.farm.item = item;
    }
    // The target stock is reached: the leader is told, and the bot is on the autopilot again (as after the profession
    // mode). Looked at every few seconds, not in a fight.
    if (bot.order.farmTarget && !player->IsInCombat() && now >= rt.farm.nextStockLook)
    {
        rt.farm.nextStockLook = now + FarmStockLookMs;
        uint32 const stock = FarmStock(player, item);
        if (stock >= bot.order.farmTarget)
        {
            std::string const itemName = ItemName(item);
            LOG_INFO(ActivityLog, "activity: {} has farmed {} up to the target stock ({} of {} in the guild storage, {} looted under "
                "this order): its order is the autopilot again", bot.name, itemName, stock, bot.order.farmTarget, rt.farm.looted);
            RecordEvent(guild, "FARM", bot.guid, Acore::StringFormat("{} has farmed {} up to the target stock ({} of {} in the guild "
                "storage) and is back on the autopilot.", bot.name, itemName, stock, bot.order.farmTarget), player->GetLevel(),
                player->GetZoneId());
            bot.order.type = OrderType::Auto;
            bot.order.zoneId = 0;
            bot.order.farmTarget = 0;
            sGuildAdventureMgr.SaveOrder(bot);
            OnOrderChanged(guild, bot);
            PushBot(guild, bot.guid, false);
            return;
        }
    }
    // A node of its profession plan beside the way is fetched, as under every order.
    if (GatherDetour(bot, player, rt))
        return;

    // The way to a far hunting ground: the travel step (it books flights) until the ground is near.
    if (rt.task.kind == TaskKind::Travel && rt.task.farmItem)
    {
        SpawnPoint const spot = rt.task.spot;
        if (Dist2d(player, spot) <= FarmTravelDistance)
        {
            ClearTask(rt, 0, "near the hunting ground");
            rt.farm.nextLook = 0;
            return;
        }
        if (!StepTask(guild, bot, player, rt) && rt.task.kind == TaskKind::None)
            rt.AddAvoidArea(spot.mapId, spot.x, spot.y, 255, FarmUnreachedMs, false);
        return;
    }

    // The hunt: the corpse of a kill first (auto loot takes what is in reach), then the next target.
    if (rt.task.kind == TaskKind::Grind && rt.task.farmItem)
    {
        if (!player->IsInCombat() && StepFarmLoot(player, rt))
            return;
        SpawnPoint const spot = rt.task.spot;
        std::string const name = rt.task.targetName;
        if (StepTask(guild, bot, player, rt))
        {
            if (rt.task.nearSince)
                rt.why = Acore::StringFormat("Farming {}: hunting {} in {} ({})", ItemName(item), name,
                    GetAreaName(player->GetZoneId()), FarmProgress(player, bot, rt));
            return;
        }
        // The hunt found nothing left to fight there (it set the spawn aside): the ground counts as emptied for a
        // while, so the next look takes another one, or waits.
        if (rt.task.kind == TaskKind::None && rt.IsAvoided(spot))
        {
            rt.farm.emptied.push_back({ spot.mapId, spot.x, spot.y, now + FarmRespawnMs });
            LOG_INFO(ActivityLog, "activity: {} has emptied the hunting ground at {} ({:.0f}, {:.0f}) for {}: it counts again in {} s",
                bot.name, name, spot.x, spot.y, ItemName(item), FarmRespawnMs / IN_MILLISECONDS);
        }
        rt.farm.nextLook = 0;
        return;
    }

    // Nothing to farm: at the inn until the next look (as under the order IDLE).
    if (IsNothingToFarm(rt.farm.problem) && now < rt.farm.nextLook &&
        (rt.task.kind == TaskKind::None || rt.task.kind == TaskKind::SafeSpot || rt.task.kind == TaskKind::Hold))
    {
        std::string const why = rt.why;
        RunIdle(bot, player, rt);
        rt.why = why.rfind("Farm order", 0) == 0 ? why : "Farm order for " + ItemName(item) + ": nothing to farm (" + rt.why + ")";
        return;
    }

    if (rt.task.kind != TaskKind::None && StepTask(guild, bot, player, rt))
        return;
    if (now < rt.waitUntil)
        return;
    if (PlanUpkeep(bot, player, rt))
        return;
    PlanFarm(guild, bot, player, rt);
}

bool OrderService::PlanFarm(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt)
{
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    uint32 const item = bot.order.zoneId;
    std::string const itemName = ItemName(item);
    uint8 const level = player->GetLevel();
    rt.farm.emptied.erase(std::remove_if(rt.farm.emptied.begin(), rt.farm.emptied.end(),
        [now](BotRuntime::Farm::Emptied const& ground) { return now >= ground.until; }), rt.farm.emptied.end());

    FarmSearch const search = sFarming.FindGrounds(player, rt, item, 1);
    rt.farm.problem = uint8(search.problem);

    if (search.problem == FarmProblem::None)
    {
        FarmGround const& ground = search.grounds.front();
        // (A later "nothing to farm" is news again.)
        rt.farm.noticeProblem = 0;
        std::string const zone = GetAreaName(sWorldIndex.GetSpawnZone(ground.spot));
        BotTask task;
        task.hasSpot = true;
        task.spot = ground.spot;
        task.targetName = ground.name;
        task.farmItem = item;
        bool const travel = ground.distance > FarmTravelDistance;
        if (travel)
            task.kind = TaskKind::Travel; // the hunt is planned when the ground is near
        else
        {
            task.kind = TaskKind::Grind;
            task.grindMin = 1;
            task.grindMax = std::max<uint8>(search.levelLimit, 1);
            task.grindAround = sBotSettings.GetFloat(player, Setting::FarmGroundRadius);
        }
        StartTask(player, rt, task);
        LOG_INFO(ActivityLog, "activity: {} (level {}) farms {}: {} {} in {} ({} creatures of {} kind{}, level {}-{}, about {:.2f} per round; "
            "{:.0f} yd away; {} of {} kinds that drop it are at or below level {})", bot.name, uint32(level), itemName,
            travel ? "travels to the hunting ground at" : "hunts", ground.name, zone.empty() ? "the open world" : zone, ground.spawns,
            ground.kinds, ground.kinds == 1 ? "" : "s", uint32(ground.minLevel), uint32(ground.maxLevel), ground.yield, ground.distance,
            search.validKinds, search.grindKinds, uint32(search.levelLimit));
        rt.why = Acore::StringFormat("Farming {}: {} {} in {} ({} creatures)", itemName, travel ? "on the way to" : "going to hunt",
            ground.name, zone.empty() ? "the open world" : zone, ground.spawns);
        return true;
    }

    if (search.problem == FarmProblem::Waiting)
    {
        // Every ground in reach is emptied: it waits where it is and looks again.
        BotTask hold;
        hold.kind = TaskKind::Hold;
        hold.targetName = "its hunting ground";
        StartTask(player, rt, hold);
        rt.farm.nextLook = now + FarmWaitLookMs;
        rt.why = Acore::StringFormat("Farming {}: waiting for the creatures to come back ({})", itemName, FarmProgress(player, bot, rt));
        return true;
    }

    // Nothing on its land mass, but on another one its side can travel to: the next leg of the way there (a ship, a
    // zeppelin, a portal). The ground is picked anew on the other side.
    if (search.problem == FarmProblem::Elsewhere && search.voyage)
    {
        Region const here = GetRegion(player);
        if (TravelLink const* link = sWorldIndex.NextTravelLink(here, search.voyageRegion, player->GetTeamId(), player->GetMapId(),
            player->GetPositionX(), player->GetPositionY(), search.voyageSpot.mapId, search.voyageSpot.x, search.voyageSpot.y))
        {
            rt.farm.noticeProblem = 0;
            rt.farm.problem = uint8(FarmProblem::None);
            LOG_INFO(ActivityLog, "activity: {} (level {}) farms {}: nothing it can farm it from lives in {}; it travels to {} ({} "
                "creatures at {} there, {} leg{} by ship, zeppelin or portal): {} from {} to {}", bot.name, uint32(level), itemName,
                ToString(here), search.elsewhere, search.voyageSpawns, search.voyageName, search.voyageLegs,
                search.voyageLegs == 1 ? "" : "s", link->name, link->fromName, link->toName);
            StartVoyage(player, rt, *link);
            rt.why = Acore::StringFormat("Farming {}: travelling to {} ({})", itemName, search.elsewhere, link->name);
            return true;
        }
    }

    // Nothing it can farm the item from: the leader is told (once per cause and level, and again after an hour), and
    // the bot waits at an inn.
    std::string const reason = search.problem == FarmProblem::Elsewhere && search.voyage ?
        Acore::StringFormat("the creatures it could farm {} from live in {}, and no ship, zeppelin or portal for its side leads "
            "there from here", itemName, search.elsewhere) : search.reason;
    if (rt.farm.noticeProblem != uint8(search.problem) || rt.farm.noticeLevel != level ||
        getMSTimeDiff(rt.farm.noticeAt, now) >= FarmNoticeGapMs)
    {
        rt.farm.noticeProblem = uint8(search.problem);
        rt.farm.noticeLevel = level;
        rt.farm.noticeAt = now;
        LOG_INFO(ActivityLog, "activity: {} (level {}) has nothing to farm {} from: {}", bot.name, uint32(level), itemName, reason);
        RecordEvent(guild, "FARM", bot.guid, Acore::StringFormat("{} cannot farm {}: {}.", bot.name, itemName, reason), level,
            player->GetZoneId());
    }
    rt.farm.nextLook = now + FarmRetryMs;
    RunIdle(bot, player, rt);
    rt.why = Acore::StringFormat("Farm order for {}: nothing to farm: {}", itemName, reason);
    return false;
}

bool OrderService::StepFarmLoot(Player* player, BotRuntime& rt)
{
    uint32 const now = std::max<uint32>(getMSTime(), 1);
    // With no room in the bags nothing would be taken: the hunt goes on until its task ends and the upkeep sells.
    if (!player->GetFreeInventorySpace())
        return false;

    std::list<Creature*> dead;
    player->GetDeadCreatureListInGrid(dead, CorpseLookYards);
    Group const* group = player->GetGroup();
    Creature* corpse = nullptr;
    float best = 0.0f;
    for (Creature* creature : dead)
    {
        if (!creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE) || creature->loot.loot_type == LOOT_SKINNING || creature->loot.isLooted())
            continue;
        if (creature->GetLootRecipient() != player && !(group && creature->GetLootRecipientGroup() == group))
            continue;
        if (std::find(rt.farm.corpsesLeft.begin(), rt.farm.corpsesLeft.end(), creature->GetGUID()) != rt.farm.corpsesLeft.end())
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
        rt.farm.corpse.Clear();
        if (dead.empty())
            rt.farm.corpsesLeft.clear();
        return false;
    }
    if (corpse->GetGUID() != rt.farm.corpse)
    {
        rt.farm.corpse = corpse->GetGUID();
        rt.farm.corpseSince = now;
        rt.farm.corpseAssert = 0;
    }
    if (getMSTimeDiff(rt.farm.corpseSince, now) > CorpseMaxMs)
    {
        // Not reached, or nothing takes it (auto loot is off and the AI does not loot it): on with the hunt.
        rt.farm.corpsesLeft.push_back(corpse->GetGUID());
        rt.farm.corpse.Clear();
        return false;
    }
    if (best > CorpseReachYards && (!rt.farm.corpseAssert || getMSTimeDiff(rt.farm.corpseAssert, now) >= CorpseAssertMs))
    {
        BotDriver::GoTo(player, player->GetMapId(), corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ(), false);
        rt.farm.corpseAssert = now;
    }
    return true;
}
