/*
 * Errands of the order planner: completed quests whose ender stands outside the bot's target zone
 * (docs/orders.md "Errands"). Part of OrderService; kept in a file of its own.
 *
 * The quest planner works one zone: a turn-in is a task only when the ender is in the target zone, or within a few
 * hundred yards. Quests that end in a capital, in the next zone or across the sea ("Report to Ironforge", every
 * delivery) stayed in the log for good, and with them the chains behind them. An errand is the trip to such an ender:
 * the travel code the zone travel uses (flight paths, ships, zeppelins, portals), then an ordinary turn-in task.
 *
 * When: all reachable hand-ins when the zone has no quest work left or the bot is outside its zone anyway (about to
 * travel there); else only when a batch has gathered or the quest log runs short.
 *
 * What counts as reachable: an ender in the bot's region within walking distance, or with a flight that covers the way;
 * across regions at most two legs (ship, zeppelin, portal) with a short walk at both ends. Never into a zone whose level
 * range starts far above the bot.
 */

#include "OrderService.h"
#include "BotSettings.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Formulas.h"
#include "GameObject.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Perf.h"
#include "Player.h"
#include "QuestDef.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldIndex.h"
#include <algorithm>
#include <cmath>
#include <optional>

using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";

    // In the zone and with quest work left, the bot leaves for hand-ins only when this many wait outside ...
    constexpr uint32 ErrandBatch = 3;
    // ... or when the quest log has fewer free slots than this.
    constexpr uint32 ErrandLogPressure = 3;
    // An ender this far away is walked to; farther ones need a flight that covers the way.
    constexpr float ErrandWalkDistance = 3000.0f;
    // Across regions: at most this many legs (ship, zeppelin, portal); a leg counts as this many yards.
    constexpr int32 ErrandMaxLegs = 2;
    constexpr float ErrandLegCost = 6000.0f;
    // A flight counts as this many yards on top of the walks to and from it.
    constexpr float ErrandFlightCost = 500.0f;
    // Outside its zone, or after an errand, hand-ins follow without the batch only when their trip costs at most this
    // (no voyage: a leg costs ErrandLegCost); a single one only when it is close (the way back counts as well: a priest
    // walked 2,314 yd to one hand-in and back, eleven minutes).
    constexpr float ErrandChainCost = 2500.0f;
    constexpr float ErrandSingleCost = 1000.0f;
    constexpr float ErrandSingleFarCost = 1500.0f;
    // Not into a zone whose level range starts more than this above the bot.
    constexpr uint8 ErrandLevelMargin = 5;
    // An errand the bot gave up is not tried again for this long.
    constexpr uint32 ErrandSkipMs = 30 * MINUTE * IN_MILLISECONDS;
    // The same limits the quest planner uses for turn-ins it treats as zone work (OrderService.cpp).
    constexpr float ZoneTurnInDistance = 2500.0f;
    constexpr float NearbyTurnInDistance = 400.0f;

    float Dist2d(float x1, float y1, float x2, float y2)
    {
        return std::sqrt((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2));
    }

    float Dist2d(Player const* player, SpawnPoint const& spot)
    {
        return Dist2d(player->GetPositionX(), player->GetPositionY(), spot.x, spot.y);
    }

    std::string ZoneName(uint32 zoneId)
    {
        if (ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(zoneId))
            return zone->name;
        AreaTableEntry const* area = zoneId ? sAreaTableStore.LookupEntry(zoneId) : nullptr;
        return area ? area->area_name[LOCALE_enUS] : "the wilds";
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

    uint32 FreeQuestSlots(Player const* player)
    {
        uint32 free = 0;
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (!player->GetQuestSlotQuestId(slot))
                ++free;
        return free;
    }
}

void OrderService::DropErrand(GuildBot const& bot, BotRuntime& rt, char const* why)
{
    if (!rt.errandQuest)
        return;
    Quest const* quest = sObjectMgr->GetQuestTemplate(rt.errandQuest);
    LOG_INFO(ActivityLog, "activity: {} gives up the hand-in of quest {} \"{}\" at {}: {}; not tried again for {} min", bot.name,
        rt.errandQuest, quest ? quest->GetTitle() : "?", rt.errandName, why, ErrandSkipMs / (MINUTE * IN_MILLISECONDS));
    rt.errandSkip[rt.errandQuest] = getMSTime() + ErrandSkipMs;
    rt.errandQuest = 0;
    rt.errandStuck = 0;
}

std::optional<OrderService::ErrandOption> OrderService::FindErrand(GuildBot const& bot, Player* player, BotRuntime const& rt,
    uint32 zoneId, uint32 questId, std::string* whyNot) const
{
    Perf::Get().errands.fetch_add(1, std::memory_order_relaxed);
    auto no = [&](char const* why) -> std::optional<ErrandOption>
    {
        if (whyNot)
            *whyNot = why;
        return std::nullopt;
    };

    uint32 const now = getMSTime();
    TeamId const team = player->GetTeamId();
    uint8 const level = player->GetLevel();
    Region const here = GetRegion(player);
    // A quest far below the bot (grey: its hand-in gives next to nothing).
    Quest const* questTemplate = sObjectMgr->GetQuestTemplate(questId);
    bool const grey = questTemplate && uint32(player->GetQuestLevel(questTemplate)) <= Acore::XP::GetGrayLevel(level);

    auto block = bot.questBlocks.find(questId);
    if (block != bot.questBlocks.end() && block->second.blocked)
        return no("its turn-in failed twice");
    auto skip = rt.errandSkip.find(questId);
    if (skip != rt.errandSkip.end() && now < skip->second)
        return no("given up a moment ago");

    std::vector<SpawnPoint> enders;
    for (SpawnPoint const& ender : sWorldIndex.GetQuestEnders(questId))
        if (!IsNpcBlocked(player, ender))
            enders.push_back(ender);
    if (enders.empty())
        return no(sWorldIndex.GetQuestEnders(questId).empty() ? "no ender in the open world" :
            "the guild's characters could not get to its ender (left alone for a day)");

    // Zone work: in its target zone PlanQuests turns it into a turn-in task. Outside the zone the quest planner does
    // not run, so every hand-in is an errand there, the one next door too.
    bool const inZone = player->GetZoneId() == zoneId;
    for (SpawnPoint const& ender : enders)
    {
        if (!inZone || rt.IsAvoided(ender) || !CanTalkTo(ender, player))
            continue;
        if (ender.mapId == player->GetMapId() && sWorldIndex.GetSpawnRegion(ender) == here)
        {
            float const distance = Dist2d(player, ender);
            if (distance <= NearbyTurnInDistance || (distance <= ZoneTurnInDistance && sWorldIndex.GetSpawnZone(ender) == zoneId))
                return no("zone work");
        }
    }

    // What the trip to a spot costs (yards of walking, legs and flights counted in), or why the bot should not go.
    char const* reason = "no ender the bot can talk to";
    auto reach = [&](SpawnPoint const& spot) -> std::optional<float>
    {
        // A place set aside after deaths or a lost fight (RiskModel, FightEscape): no errand into it for the time.
        // (On any map: the verdict is the spawn data's. Mojowind, 50, sailed from Tanaris to the Bloodsail
        // Correspondence in the middle of the pirates' camp on the Wild Shore, round 259.)
        if (!IsPlaceUsable(player, rt, spot.mapId, spot.x, spot.y, spot.z))
        {
            reason = "the ender stands among creatures the bot cannot take";
            return std::nullopt;
        }
        if (rt.IsInAvoidedArea(spot.mapId, spot.x, spot.y, level))
        {
            reason = "the place is set aside after deaths or a lost fight";
            return std::nullopt;
        }

        if (ZoneTemplate const* zone = sGuildAdventureMgr.GetZone(sWorldIndex.GetSpawnZone(spot)))
            if (zone->minLevel > level + ErrandLevelMargin)
            {
                reason = "the zone of the ender is too high";
                return std::nullopt;
            }

        Region const there = sWorldIndex.GetSpawnRegion(spot);
        if (here == Region::None || there == Region::None)
        {
            reason = "unknown region";
            return std::nullopt;
        }

        if (here == there)
        {
            if (spot.mapId != player->GetMapId())
            {
                reason = "another map";
                return std::nullopt;
            }
            float const distance = Dist2d(player, spot);
            if (distance <= ErrandWalkDistance)
                return distance;
            FlightPlan flight;
            if (FindFlight(player, rt, spot.mapId, spot.x, spot.y, spot.z, flight, sBotSettings.Get(player, Setting::WalksMaxLevelsAbove)))
                return flight.toMaster + flight.fromLanding + ErrandFlightCost;
            reason = "too far to walk and no flight covers the way";
            return std::nullopt;
        }

        // Across the sea only for a quest that still gives something (a level-25 shaman sailed to Theramore for a
        // level-10 quest whose ender stands on Bloodmyst Isle, two ships and a continent's walk away).
        if (grey)
        {
            reason = "the quest is far below its level: its hand-in is not worth a voyage";
            return std::nullopt;
        }
        int32 const legs = sWorldIndex.CountTravelLegs(here, there, team);
        if (legs < 1 || legs > ErrandMaxLegs)
        {
            reason = legs < 1 ? "no ship, zeppelin or portal goes there" : "too many legs";
            return std::nullopt;
        }
        // Not back across the sea it has just crossed: the hand-ins over there wait the hour the zone choice waits.
        // (The errand that crossing was for may take its second leg.)
        if (rt.CrossedLately() && rt.errandCrossing != questId)
        {
            reason = "it went ashore less than an hour ago and stays on this side";
            return std::nullopt;
        }
        if (rt.SeaClosed(now, player->GetMapId(), player->GetPositionX(), player->GetPositionY()))
        {
            reason = "the way to the ship was refused from here a moment ago";
            return std::nullopt;
        }
        TravelLink const* link = sWorldIndex.NextTravelLink(here, there, team, player->GetMapId(), player->GetPositionX(),
            player->GetPositionY(), spot.mapId, spot.x, spot.y);
        if (!link || link->from.mapId != player->GetMapId())
        {
            reason = "no ship, zeppelin or portal goes there";
            return std::nullopt;
        }

        float toDock = Dist2d(player->GetPositionX(), player->GetPositionY(), link->from.x, link->from.y);
        if (toDock > ErrandWalkDistance)
        {
            FlightPlan flight;
            if (!FindFlight(player, rt, link->from.mapId, link->from.x, link->from.y, link->from.z, flight))
            {
                reason = "the dock is too far to walk and no flight covers the way";
                return std::nullopt;
            }
            toDock = flight.toMaster + flight.fromLanding + ErrandFlightCost;
        }
        // The walk at the far end is known when one leg gets there; after two legs the errand is looked at again.
        float fromDock = 0.0f;
        if (legs == 1)
        {
            if (link->to.mapId != spot.mapId)
            {
                reason = "another map";
                return std::nullopt;
            }
            fromDock = Dist2d(link->to.x, link->to.y, spot.x, spot.y);
            if (fromDock > ErrandWalkDistance)
            {
                reason = "too far from the dock";
                return std::nullopt;
            }
        }
        return toDock + float(legs) * ErrandLegCost + fromDock;
    };

    std::optional<ErrandOption> best;
    for (SpawnPoint const& ender : enders)
    {
        if (rt.IsAvoided(ender) || !CanTalkTo(ender, player))
            continue;
        if (std::optional<float> cost = reach(ender))
            if (!best || *cost < best->cost)
                best = ErrandOption{ questId, ender, *cost };
    }
    if (!best)
        return no(reason);
    return best;
}

void OrderService::DescribeErrands(GuildBot const& bot, Player* player, BotRuntime const& rt, std::vector<std::string>& lines) const
{
    if (rt.errandQuest)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(rt.errandQuest);
        lines.push_back(Acore::StringFormat("errand: hand in quest {} \"{}\" at {}", rt.errandQuest, quest ? quest->GetTitle() : "?",
            rt.errandName));
    }
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = player->GetQuestSlotQuestId(slot);
        if (!questId || questId == rt.errandQuest || player->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        std::string whyNot;
        if (std::optional<ErrandOption> option = FindErrand(bot, player, rt, rt.targetZone, questId, &whyNot))
            lines.push_back(Acore::StringFormat("hand-in outside the zone: quest {} \"{}\" at {} in {}, about {:.0f} yd", questId,
                quest ? quest->GetTitle() : "?", SpawnName(option->spot), ZoneName(sWorldIndex.GetSpawnZone(option->spot)), option->cost));
        else if (whyNot != "zone work")
            lines.push_back(Acore::StringFormat("hand-in outside the zone: quest {} \"{}\": not now ({})", questId,
                quest ? quest->GetTitle() : "?", whyNot));
    }
}

bool OrderService::PlanErrand(AdventureGuild& guild, GuildBot& bot, Player* player, BotRuntime& rt, uint32 zoneId, bool all, bool distant)
{
    uint32 const now = getMSTime();
    TeamId const team = player->GetTeamId();

    // A running errand: still to do?
    if (rt.errandQuest)
    {
        auto block = bot.questBlocks.find(rt.errandQuest);
        if (player->GetQuestStatus(rt.errandQuest) != QUEST_STATUS_COMPLETE)
        {
            rt.errandQuest = 0;
            rt.errandStuck = 0;
            all = true; // on the road already: the other hand-ins that can be reached from here follow
        }
        else if (rt.IsAvoided(rt.errandSpot) || (block != bot.questBlocks.end() && block->second.blocked))
        {
            DropErrand(bot, rt, "the ender could not be dealt with");
            all = true;
        }
    }

    if (!rt.errandQuest)
    {
        // Skips that ran out.
        for (auto itr = rt.errandSkip.begin(); itr != rt.errandSkip.end();)
            itr = now >= itr->second ? rt.errandSkip.erase(itr) : std::next(itr);

        std::vector<ErrandOption> errands;
        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = player->GetQuestSlotQuestId(slot);
            if (!questId || player->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
                continue;
            if (std::optional<ErrandOption> errand = FindErrand(bot, player, rt, zoneId, questId))
                errands.push_back(*errand);
        }
        if (errands.empty())
            return false;
        if (errands.size() < ErrandBatch && FreeQuestSlots(player) >= ErrandLogPressure)
        {
            if (!all && !distant)
                return false;
            // Outside its zone, or on the road already (an errand has just been done): the hand-ins near follow. One
            // that is a voyage or a long way off waits for the batch, or until the zone has nothing left (a shaman's
            // third hand-in in a row was a single quest two ships away, straight after two in Stormwind and Ironforge).
            if (!distant)
                std::erase_if(errands, [](ErrandOption const& errand) { return errand.cost > ErrandChainCost; });
            // A single hand-in only when it is close, a little farther when the zone has nothing left.
            if (errands.size() == 1 && errands.front().cost > (distant ? ErrandSingleFarCost : ErrandSingleCost))
                errands.clear();
            if (errands.empty())
                return false;
        }

        auto best = std::min_element(errands.begin(), errands.end(), [](ErrandOption const& a, ErrandOption const& b)
        {
            return a.cost < b.cost || (a.cost == b.cost && a.questId < b.questId);
        });
        rt.errandQuest = best->questId;
        rt.errandSpot = best->spot;
        rt.errandStuck = 0;
        rt.errandTravelStarts = 0;
        rt.errandName = Acore::StringFormat("{} in {}", SpawnName(best->spot), ZoneName(sWorldIndex.GetSpawnZone(best->spot)));
        Quest const* quest = sObjectMgr->GetQuestTemplate(best->questId);
        LOG_INFO(ActivityLog, "activity: {} starts an errand: hand in quest {} \"{}\" at {} (about {:.0f} yd; {} hand-in{} outside {})",
            bot.name, best->questId, quest ? quest->GetTitle() : "?", rt.errandName, best->cost, errands.size(),
            errands.size() == 1 ? " waits" : "s wait", ZoneName(zoneId));
    }

    Quest const* quest = sObjectMgr->GetQuestTemplate(rt.errandQuest);
    std::string const title = quest ? quest->GetTitle() : "?";

    // The ender stands next to the bot's inn, and the inn is far from here: the hearthstone is the way.
    if (HearthTowards(player, rt, rt.errandSpot.mapId, rt.errandSpot.x, rt.errandSpot.y,
        Acore::StringFormat("its inn stands next to {}, where it hands in quest {}", rt.errandName, rt.errandQuest)))
        return true;

    // Another region: the next ship, zeppelin or portal towards the ender.
    Region const here = GetRegion(player);
    Region const there = sWorldIndex.GetSpawnRegion(rt.errandSpot);
    if (here != there)
    {
        TravelLink const* link = sWorldIndex.NextTravelLink(here, there, team, player->GetMapId(), player->GetPositionX(),
            player->GetPositionY(), rt.errandSpot.mapId, rt.errandSpot.x, rt.errandSpot.y);
        if (!link)
        {
            DropErrand(bot, rt, "no ship, zeppelin or portal goes there from here");
            return false;
        }
        StartVoyage(player, rt, *link);
        // The voyage is the errand's: without the mark its refused walk to the dock set the bot's own zone aside and
        // sent it home by hearthstone, where the errand went on (a level-45 warrior at Thorium Point: its zone, the
        // Searing Gorge, written off for a hand-in at Theramore, a hearth to Booty Bay, the ship to Ratchet), and the
        // look at the zones after going ashore for a hand-in never came (it sailed back for a zone on the side it
        // had left, with twelve quests waiting on the side it stood on).
        rt.task.errand = true;
        rt.task.questId = rt.errandQuest;
        StepTask(guild, bot, player, rt);
        return true;
    }

    if (rt.errandSpot.mapId != player->GetMapId())
    {
        DropErrand(bot, rt, "the ender is on another map");
        return false;
    }

    BotTask task;
    task.hasSpot = true;
    task.spot = rt.errandSpot;
    if (Dist2d(player, rt.errandSpot) > ErrandArriveDistance)
    {
        // Begun again and again without getting nearer (a pack or a fight on the way ended it each time; a level-22
        // priest began one such travel ten times in four minutes, six of them within one): the hand-in waits.
        constexpr uint8 RestartLimit = 3;
        constexpr uint32 RestartWindowMs = 10 * MINUTE * IN_MILLISECONDS;
        constexpr float RestartProgress = 150.0f;
        float const left = Dist2d(player, rt.errandSpot);
        if (!rt.errandTravelStarts || getMSTimeDiff(rt.errandTravelAt, now) > RestartWindowMs || left < rt.errandTravelLeft - RestartProgress)
        {
            rt.errandTravelStarts = 0;
            rt.errandTravelAt = now;
            rt.errandTravelLeft = left;
        }
        if (++rt.errandTravelStarts > RestartLimit)
        {
            rt.errandTravelStarts = 0;
            DropErrand(bot, rt, "its travel was begun 4 times without getting nearer (a pack or a fight on the way each time)");
            return false;
        }
        task.kind = TaskKind::Travel;
        task.errand = true;
        task.questId = rt.errandQuest;
        task.targetName = rt.errandName;
        StartTask(player, rt, task);
        rt.why = Acore::StringFormat("Travelling to {} to turn in \"{}\"", rt.errandName, title);
        StepTask(guild, bot, player, rt);
        return true;
    }

    task.kind = TaskKind::TurnIn;
    task.questId = rt.errandQuest;
    task.targetName = SpawnName(rt.errandSpot);
    StartTask(player, rt, task);
    rt.why = Acore::StringFormat("Turning in \"{}\" at {}", title, rt.errandName);
    return true;
}
