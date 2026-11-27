/*
 * QuestPartnerService implementation. See QuestPartners.h and docs/orders.md "Quest partners".
 */

#include "QuestPartners.h"
#include "GameTime.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildBotHolder.h"
#include "GuildEvents.h"
#include "BotSettings.h"
#include "FightEscape.h"
#include "Log.h"
#include "NewRpgHelpers.h"
#include "ObjectMgr.h"
#include "OrderService.h"
#include "Player.h"
#include "QuestDef.h"
#include "QuestSupport.h"
#include "Squads.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldIndex.h"
#include <algorithm>
#include <map>

using namespace GuildAdventure;

namespace
{
    constexpr char const* ActivityLog = "module.ga.activity";
    // Pairing and splitting are looked at this often per guild.
    constexpr uint32 UpdateMs = 20 * IN_MILLISECONDS;
    // Partners stand together within this distance (as the risk model counts a squad: RiskModel's SquadRiskYards).
    constexpr float TogetherYards = 120.0f;
    // A bot this near a quest giver picks a hard quest up when a guild character that could be its partner keeps it.
    constexpr float CheapPickupYards = 150.0f;
    // After partners parted without the quest done (too much even for both, no meeting, too long), neither pairs up again
    // for this long: else one pair goes through every hard quest of the two, one plan each.
    constexpr uint32 PairRestSeconds = 5 * MINUTE;
    // A quest a pair could not work at its level waits at most this long for a holder of that level (QuestPartners::_held).
    constexpr uint32 HoldSeconds = 2 * HOUR;
    // After they met, a leader that works no task of the pair's quest for this long (out of a fight and a rest) parts the
    // pair; the quest waits HoldSeconds before partners pair up for it again.
    constexpr uint32 NoWorkSeconds = 4 * MINUTE;

    uint32 GameNow()
    {
        return uint32(GameTime::GetGameTime().count());
    }

    std::string QuestTitle(uint32 questId)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        return quest ? quest->GetTitle() : std::to_string(questId);
    }

    AdventureGuild* GuildOf(ObjectGuid guid)
    {
        if (AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfBot(guid))
            return guild;
        return sGuildAdventureMgr.FindGuildOfLeader(guid);
    }

    // The quest is in the log, worked on or finished.
    bool InLog(Player const* player, uint32 questId)
    {
        if (!player)
            return false;
        QuestStatus const status = player->GetQuestStatus(questId);
        return status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE;
    }

    bool PlaysOnItsOwn(GuildBot const& bot)
    {
        return bot.state == BotState::Active && (bot.order.type == OrderType::Auto || bot.order.type == OrderType::LevelZone);
    }

    Player* InWorld(ObjectGuid guid)
    {
        Player* player = sGuildBotHolder.GetPlannedPlayer(guid);
        return player && player->IsInWorld() && !player->IsDuringRemoveFromWorld() ? player : nullptr;
    }
}

QuestPartnerService& QuestPartnerService::instance()
{
    static QuestPartnerService instance;
    return instance;
}

bool QuestPartnerService::IsEnabled(Player const* player) const
{
    if (!player || !GetSettings().squadsEnable)
        return false;
    // Roster bots only: the guild leader (no squad can take it in) keeps the planner's old ways.
    if (!sGuildAdventureMgr.FindGuildOfBot(player->GetGUID()))
        return false;
    return sBotSettings.IsOn(player, Setting::SquadsQuestPartners);
}

bool QuestPartnerService::IsGroupQuest(uint32 questId) const
{
    if (auto itr = _groupQuests.find(questId); itr != _groupQuests.end())
        return itr->second;

    bool group = false;
    if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
    {
        switch (quest->GetType())
        {
            // Dungeon and raid quests are the dungeon runs' (OrderDungeons.cpp).
            case QUEST_TYPE_DUNGEON:
            case QUEST_TYPE_RAID:
            case QUEST_TYPE_HEROIC:
            case QUEST_TYPE_RAID_10:
            case QUEST_TYPE_RAID_25:
                break;
            case QUEST_TYPE_ELITE:
                group = true;
                break;
            default:
                group = quest->GetSuggestedPlayers() > 1;
                for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT && !group; ++i)
                {
                    if (quest->RequiredNpcOrGo[i] <= 0 || !quest->RequiredNpcOrGoCount[i])
                        continue;
                    CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(uint32(quest->RequiredNpcOrGo[i]));
                    group = creature && (creature->rank == CREATURE_ELITE_ELITE || creature->rank == CREATURE_ELITE_RAREELITE ||
                        creature->rank == CREATURE_ELITE_WORLDBOSS);
                }
                break;
        }
    }
    _groupQuests[questId] = group;
    return group;
}

void QuestPartnerService::Prune(HardMarks& marks) const
{
    uint32 const oldest = GameNow() - std::min<uint32>(GameNow(), GetSettings().squadsPartnerHours * HOUR);
    std::erase_if(marks.deaths, [oldest](Mark const& mark) { return mark.at < oldest; });
    std::erase_if(marks.tooHard, [oldest](Mark const& mark) { return mark.at < oldest; });
}

std::string QuestPartnerService::WhyHard(uint32 ownerAccount, uint32 questId, uint8 level) const
{
    if (IsGroupQuest(questId))
        return "an elite or group quest";

    Settings const& settings = GetSettings();
    auto owner = _hard.find(ownerAccount);
    if (owner == _hard.end())
        return "";
    auto quest = owner->second.find(questId);
    if (quest == owner->second.end())
        return "";
    uint32 const oldest = GameNow() - std::min<uint32>(GameNow(), settings.squadsPartnerHours * HOUR);
    if (settings.squadsPartnerDeaths)
    {
        uint32 deaths = 0;
        for (Mark const& mark : quest->second.deaths)
            if (mark.at >= oldest && level <= mark.level)
                ++deaths;
        if (deaths >= settings.squadsPartnerDeaths)
            return Acore::StringFormat("{} deaths of the guild on it", deaths);
    }
    if (settings.squadsPartnerTooHard)
        for (Mark const& mark : quest->second.tooHard)
            if (mark.at >= oldest && level <= mark.level)
                return Acore::StringFormat("too much for {} at level {}", mark.who, uint32(mark.whoLevel));
    return "";
}

bool QuestPartnerService::IsSkipped(GuildBot const& bot, Player* player, uint32 questId) const
{
    if (!IsEnabled(player) || player->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
        return false;
    AdventureGuild const* guild = sGuildAdventureMgr.FindGuildOfBot(bot.guid);
    if (!guild || WhyHard(guild->ownerAccount, questId, player->GetLevel()).empty())
        return false;
    // Its partners stand beside it (a quest partner squad, or the owner's squad): it works the quest with them.
    return sSquadService.CountTogether(player, TogetherYards) < 2;
}

bool QuestPartnerService::KeepsInsteadOfFailing(GuildBot const& bot, Player* player, uint32 questId, std::string const& reason)
{
    if (reason != "deaths" && reason != "no_progress")
        return false;
    if (!IsSkipped(bot, player, questId))
        return false;
    LOG_INFO(ActivityLog, "activity: {} keeps quest {} \"{}\" instead of giving it back ({}): it waits for a guild partner", bot.name,
        questId, QuestTitle(questId), reason == "deaths" ? "it died on it" : "no progress");
    return true;
}

bool QuestPartnerService::AllowsPickup(GuildBot const& bot, Player* player, uint32 questId, float giverDistance) const
{
    if (!IsEnabled(player))
        return true;
    AdventureGuild const* guild = sGuildAdventureMgr.FindGuildOfBot(bot.guid);
    if (!guild || WhyHard(guild->ownerAccount, questId, player->GetLevel()).empty())
        return true;
    if (sSquadService.CountTogether(player, TogetherYards) >= 2)
        return true;
    if (giverDistance > CheapPickupYards)
        return false;

    // Another guild character keeps it and could be its partner: the bot is at the giver anyway.
    Settings const& settings = GetSettings();
    for (auto const& [guid, other] : guild->bots)
    {
        if (guid == bot.guid || !PlaysOnItsOwn(other))
            continue;
        Player* holder = InWorld(guid);
        if (!holder || holder->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE || holder->GetMapId() != player->GetMapId() ||
            holder->GetZoneId() != player->GetZoneId() ||
            std::abs(int32(holder->GetLevel()) - int32(player->GetLevel())) > int32(settings.squadsMaxLevelGap) ||
            !holder->IsWithinDistInMap(player, settings.squadsPartnerYards) || !IsEnabled(holder))
            continue;
        return true;
    }
    return false;
}

std::vector<uint32> QuestPartnerService::Waiting(GuildBot const& bot, Player* player) const
{
    std::vector<uint32> waiting;
    if (!IsEnabled(player))
        return waiting;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = player->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest || player->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE || !NewRpgHelpers::IsQuestWorthDoing(player, quest))
            continue;
        auto block = bot.questBlocks.find(questId);
        if ((block != bot.questBlocks.end() && block->second.blocked) || sQuestSupport.IsLeftAlone(player, questId))
            continue;
        if (IsSkipped(bot, player, questId))
            waiting.push_back(questId);
    }
    return waiting;
}

void QuestPartnerService::OnQuestDeath(Player* player, uint32 questId)
{
    AdventureGuild* guild = player ? GuildOf(player->GetGUID()) : nullptr;
    if (!guild || !questId || !GetSettings().squadsEnable)
        return;
    std::string const before = WhyHard(guild->ownerAccount, questId, player->GetLevel());
    HardMarks& marks = _hard[guild->ownerAccount][questId];
    Prune(marks);
    marks.deaths.push_back({ GameNow(), player->GetLevel(), player->GetName(), player->GetLevel() });
    std::string const now = WhyHard(guild->ownerAccount, questId, player->GetLevel());
    if (before.empty() && !now.empty())
        LOG_INFO(ActivityLog, "activity: quest {} \"{}\" is hard for the guild now ({}, the last {} at level {}): guild bots up to that "
            "level do it with a partner", questId, QuestTitle(questId), now, player->GetName(), uint32(player->GetLevel()));
}

void QuestPartnerService::OnTooHard(Player* player, uint32 questId, uint8 level)
{
    AdventureGuild* guild = player ? GuildOf(player->GetGUID()) : nullptr;
    if (!guild || !questId || !GetSettings().squadsEnable || !GetSettings().squadsPartnerTooHard)
        return;
    std::string const before = WhyHard(guild->ownerAccount, questId, level);
    HardMarks& marks = _hard[guild->ownerAccount][questId];
    Prune(marks);
    marks.tooHard.push_back({ GameNow(), level, player->GetName(), player->GetLevel() });
    if (before.empty())
        LOG_INFO(ActivityLog, "activity: quest {} \"{}\" is hard for the guild now (too much for {} at level {}): guild bots up to "
            "level {} do it with a partner", questId, QuestTitle(questId), player->GetName(), uint32(player->GetLevel()), uint32(level));
}

void QuestPartnerService::OnLeftUntil(Player* player, uint32 questId, uint8 untilLevel)
{
    if (!player || !questId)
        return;
    if (untilLevel > player->GetLevel())
        _leftUntil[player->GetGUID()][questId] = untilLevel;
    else if (auto bot = _leftUntil.find(player->GetGUID()); bot != _leftUntil.end())
        bot->second.erase(questId);
}

void QuestPartnerService::OnOwnerSplit(AdventureGuild const& guild, uint32 questId)
{
    _retryAt[guild.ownerAccount][questId] = GameNow() + GetSettings().squadsPartnerRetryMinutes * MINUTE;
}

void QuestPartnerService::Split(AdventureGuild& guild, ObjectGuid member, ObjectGuid leader, uint32 questId, std::string const& why,
    bool retryLater)
{
    GuildBot const* memberBot = guild.FindBot(member);
    GuildBot const* leaderBot = guild.FindBot(leader);
    std::string error;
    _idleSince.erase(member);
    sSquadService.Set(guild, member, ObjectGuid::Empty, error);
    // Neither is left standing in a place it cannot take alone: a level-19 partner, left in the Blackrock orc camp of
    // Redridge when its pair parted, pulled two orcs alone and died twice within 93 seconds. With hostile creatures near,
    // both go back the way they came first (Escape::BackOut), before their own orders turn them to anything else.
    for (ObjectGuid const& partner : { member, leader })
        if (Player* player = InWorld(partner))
            if (BotRuntime* rt = OrderService::Instance().FindRuntime(partner))
                Escape::BackOut(player, *rt, "as its quest pair parts");
    uint32 const retry = GetSettings().squadsPartnerRetryMinutes;
    if (retryLater)
    {
        _retryAt[guild.ownerAccount][questId] = GameNow() + retry * MINUTE;
        _pairRestUntil[member] = GameNow() + PairRestSeconds;
        _pairRestUntil[leader] = GameNow() + PairRestSeconds;
    }
    LOG_INFO(ActivityLog, "activity: {} and {} part after quest {} \"{}\": {}{}", memberBot ? memberBot->name : member.ToString(),
        leaderBot ? leaderBot->name : leader.ToString(), questId, QuestTitle(questId), why,
        retryLater ? Acore::StringFormat("; the quest waits {} min before partners pair up for it again", retry) : std::string());
    PushBot(guild, member, false);
    if (leaderBot)
        PushBot(guild, leader, false);
}

void QuestPartnerService::Update(AdventureGuild& guild)
{
    uint32 const nowMs = getMSTime();
    uint32& next = _nextUpdate[guild.ownerAccount];
    if (nowMs < next)
        return;
    next = nowMs + UpdateMs;

    Settings const& settings = GetSettings();
    uint32 const now = GameNow();
    if (auto owner = _hard.find(guild.ownerAccount); owner != _hard.end())
    {
        for (auto itr = owner->second.begin(); itr != owner->second.end();)
        {
            Prune(itr->second);
            itr = itr->second.deaths.empty() && itr->second.tooHard.empty() ? owner->second.erase(itr) : std::next(itr);
        }
    }
    if (auto owner = _retryAt.find(guild.ownerAccount); owner != _retryAt.end())
        std::erase_if(owner->second, [now](auto const& entry) { return entry.second <= now; });
    if (auto owner = _held.find(guild.ownerAccount); owner != _held.end())
        std::erase_if(owner->second, [now](auto const& entry) { return entry.second.until <= now; });

    // (1) The partner links of this guild: met, done, turned round or parted.
    for (ObjectGuid const& member : sSquadService.GetPartnerMembers())
    {
        GuildBot const* memberBot = guild.FindBot(member);
        if (!memberBot)
            continue;
        ObjectGuid const leader = sSquadService.GetLeader(member);
        uint32 const questId = sSquadService.GetPartnerQuest(member);
        GuildBot const* leaderBot = guild.FindBot(leader);
        Player* memberPlayer = InWorld(member);
        Player* leaderPlayer = InWorld(leader);
        bool const met = sSquadService.HasMet(member);
        uint32 const age = now - std::min(now, sSquadService.GetSetAt(member));

        if (!settings.squadsEnable)
        {
            Split(guild, member, leader, questId, "squads are switched off", false);
            continue;
        }
        if (!leaderBot || !memberPlayer || !leaderPlayer)
        {
            Split(guild, member, leader, questId, Acore::StringFormat("{} is not in the world",
                !memberPlayer ? memberBot->name : leaderBot ? leaderBot->name : std::string("its partner")), true);
            continue;
        }
        if (!PlaysOnItsOwn(*memberBot) || !PlaysOnItsOwn(*leaderBot))
        {
            Split(guild, member, leader, questId, Acore::StringFormat("{} has another order or was benched",
                !PlaysOnItsOwn(*memberBot) ? memberBot->name : leaderBot->name), false);
            continue;
        }
        if (!IsEnabled(memberPlayer) || !IsEnabled(leaderPlayer))
        {
            Split(guild, member, leader, questId, "quest partners were switched off for one of them", false);
            continue;
        }

        bool const leaderHas = InLog(leaderPlayer, questId);
        bool const memberHas = InLog(memberPlayer, questId);
        if (!leaderHas && !memberHas)
        {
            Split(guild, member, leader, questId, "the quest is done", false);
            continue;
        }
        // The leader has finished its part (complete, or handed in) and its member has not: the roles turn round (the one
        // that still needs it leads, the other helps), when nobody else follows that leader. Turning round at "complete"
        // keeps the leader from walking off to its hand-ins and errands with its member in tow.
        bool const leaderFinished = !leaderHas || (leaderPlayer->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE &&
            memberPlayer->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE);
        if (leaderFinished)
        {
            if (memberPlayer->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
            {
                Split(guild, member, leader, questId, Acore::StringFormat("{} handed it in, {} hands it in itself", leaderBot->name,
                    memberBot->name), false);
                continue;
            }
            std::string error;
            if (sSquadService.GetMembers(leader).size() == 1 && sSquadService.Set(guild, member, ObjectGuid::Empty, error) &&
                sSquadService.Set(guild, leader, member, error, false, questId))
            {
                if (met)
                    sSquadService.MarkMet(leader);
                LOG_INFO(ActivityLog, "activity: {} is done with quest {} \"{}\" and {} is not: {} leads now and {} helps it",
                    leaderBot->name, questId, QuestTitle(questId), memberBot->name, memberBot->name, leaderBot->name);
                PushBot(guild, member, false);
                PushBot(guild, leader, false);
            }
            else
                Split(guild, member, leader, questId, Acore::StringFormat("{} is done with it", leaderBot->name), false);
            continue;
        }

        if (OrderService::Instance().FindRuntime(member) && OrderService::Instance().FindRuntime(member)->squad.meetFailed)
        {
            OrderService::Instance().FindRuntime(member)->squad.meetFailed = false;
            Split(guild, member, leader, questId, Acore::StringFormat("{} found no way to {}", memberBot->name, leaderBot->name), true);
            continue;
        }
        if (BotRuntime const* leaderRt = OrderService::Instance().FindRuntime(leader);
            met && leaderRt && leaderRt->IsDangerous(questId, leaderPlayer->GetLevel()) &&
            sSquadService.CountTogether(leaderPlayer, TogetherYards) >= 2)
        {
            // Left until a level (its targets above the level cap, a creature far above it there): a second pair at the
            // same level would part again, so the quest is paired once a holder of it is that level. In the first live
            // round 12 of 14 pairs parted so within 20 s of meeting.
            std::string hold;
            if (auto left = _leftUntil.find(leader); left != _leftUntil.end())
                if (auto quest = left->second.find(questId); quest != left->second.end() && quest->second > leaderPlayer->GetLevel())
                {
                    _held[guild.ownerAccount][questId] = { quest->second, now + HoldSeconds };
                    hold = Acore::StringFormat(", and it waits until one of them is level {}", uint32(quest->second));
                }
            Split(guild, member, leader, questId, Acore::StringFormat("{} set the quest aside although both stood together (deaths, packs on "
                "the way, or every spot too much){}", leaderBot->name, hold), true);
            continue;
        }
        // After they met the leader works the pair's quest, or nothing comes of the pair: a leader whose walks to the quest's
        // targets were refused (round 138: the walk to Vyral the Vile led through a camp of level-61 creatures, refused every
        // half hour) held its member at the Cenarion Hold inn until the hour of PartnerMaxMinutes was up, and the two paired
        // again half an hour later. With no task of the quest for NoWorkSeconds out of a fight and a rest, they part, and
        // the quest waits two hours.
        if (BotRuntime const* leaderRt = OrderService::Instance().FindRuntime(leader); met && leaderRt)
        {
            bool const working = leaderRt->task.questId == questId || leaderPlayer->IsInCombat() || leaderRt->rest.active ||
                !leaderPlayer->IsAlive();
            uint32& since = _idleSince[member];
            if (working)
                since = 0;
            else if (!since)
                since = now;
            else if (now - since >= NoWorkSeconds)
            {
                Split(guild, member, leader, questId, Acore::StringFormat("{} found nothing of the quest to work on for {} min (its "
                    "ways to the quest's targets refused, or no target it may take); the quest waits {} h before partners pair up "
                    "for it again", leaderBot->name, NoWorkSeconds / MINUTE, HoldSeconds / HOUR), false);
                _retryAt[guild.ownerAccount][questId] = now + HoldSeconds;
                _pairRestUntil[member] = now + PairRestSeconds;
                _pairRestUntil[leader] = now + PairRestSeconds;
                continue;
            }
        }
        if (!met && age > settings.squadsPartnerMeetMinutes * MINUTE)
        {
            Split(guild, member, leader, questId, Acore::StringFormat("they did not meet within {} min", settings.squadsPartnerMeetMinutes),
                true);
            continue;
        }
        if (age > settings.squadsPartnerMaxMinutes * MINUTE)
        {
            Split(guild, member, leader, questId, Acore::StringFormat("it took longer than {} min", settings.squadsPartnerMaxMinutes), true);
            continue;
        }
    }

    // (2) Bots that keep a hard quest for a partner: the line once, and pairs where two of them have the same.
    std::map<uint32, std::vector<Player*>> holders;
    for (auto const& [guid, bot] : guild.bots)
    {
        Player* player = PlaysOnItsOwn(bot) ? InWorld(guid) : nullptr;
        if (!player || !player->IsAlive() || !IsEnabled(player))
        {
            _announced.erase(guid);
            continue;
        }
        if (BotRuntime const* rt = OrderService::Instance().FindRuntime(guid); rt && rt->dungeon.id)
            continue;
        // (Nor a rogue on its way to a poison vendor, RoguePoisons.h: a pairing 15 s after it set out took it 1460 yd
        // away with bare weapons, twice.)
        if (BotRuntime const* rt = OrderService::Instance().FindRuntime(guid); rt && rt->poisonTripSince &&
            getMSTimeDiff(rt->poisonTripSince, nowMs) < 3 * MINUTE * IN_MILLISECONDS)
            continue;
        std::vector<uint32> const waiting = Waiting(bot, player);
        // (Once per quest while it stays in the log: partners that are apart for a moment do not write it again.)
        std::unordered_set<uint32>& announced = _announced[guid];
        std::erase_if(announced, [player](uint32 questId) { return player->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE; });
        for (uint32 questId : waiting)
            if (announced.insert(questId).second)
                LOG_INFO(ActivityLog, "activity: {} keeps quest {} \"{}\" ({}) until another guild member has it too", bot.name, questId,
                    QuestTitle(questId), WhyHard(guild.ownerAccount, questId, player->GetLevel()));

        if (!sSquadService.GetLeader(guid).IsEmpty() || !sSquadService.GetMembers(guid).empty())
            continue;
        if (auto rest = _pairRestUntil.find(guid); rest != _pairRestUntil.end())
        {
            if (now < rest->second)
                continue;
            _pairRestUntil.erase(rest);
        }
        auto retries = _retryAt.find(guild.ownerAccount);
        for (uint32 questId : waiting)
            if (retries == _retryAt.end() || !retries->second.count(questId))
                holders[questId].push_back(player);
    }

    // Elite and group quests first, then quests the guild died on, then those that were too much for one.
    std::vector<std::pair<uint32, uint32>> order; // (rank, quest)
    for (auto const& [questId, list] : holders)
        if (list.size() >= 2)
        {
            std::string const why = WhyHard(guild.ownerAccount, questId, list.front()->GetLevel());
            order.emplace_back(IsGroupQuest(questId) ? 0 : why.find("deaths") != std::string::npos ? 1 : 2, questId);
        }
    std::sort(order.begin(), order.end());

    std::unordered_set<ObjectGuid> taken;
    for (auto const& [rank, questId] : order)
    {
        std::vector<Player*>& list = holders[questId];
        std::sort(list.begin(), list.end(), [](Player const* a, Player const* b)
        {
            return a->GetLevel() != b->GetLevel() ? a->GetLevel() > b->GetLevel() : a->GetGUID() < b->GetGUID();
        });
        if (auto owner = _held.find(guild.ownerAccount); owner != _held.end())
            if (auto hold = owner->second.find(questId); hold != owner->second.end())
            {
                if (list.front()->GetLevel() < hold->second.level)
                    continue;
                owner->second.erase(hold);
            }
        // The leader works the quest with its planner, which plans quests of the zone it levels in: the quest's zone must
        // be the one its order or the autopilot chose (the one it stands in only while it has none: its planner leaves a
        // quest of the zone it passes through alone).
        uint32 const questZone = sWorldIndex.GetQuestZone(questId);
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            Player* leader = list[i];
            if (taken.count(leader->GetGUID()))
                continue;
            BotRuntime const* leaderRt = OrderService::Instance().FindRuntime(leader->GetGUID());
            uint32 const plansFor = leaderRt && leaderRt->targetZone ? leaderRt->targetZone : leader->GetZoneId();
            if (!questZone || questZone != plansFor)
                continue;
            // A quest that is hard because it was too much for one (rank 2): judged for the level of the one it was too
            // much for, so a partner more than a level below the leader is not taken into its places (a level-19 partner
            // of a level-21 leader died twice in the Blackrock orc camp of Redridge). Elite, group and death quests keep
            // the squads' level gap.
            int32 const maxGap = rank == 2 ? std::min<int32>(1, int32(settings.squadsMaxLevelGap)) : int32(settings.squadsMaxLevelGap);
            // One partner is enough for such a quest (it was judged with one more character), and every member walks:
            // in the live rounds before 2026-11-13 19:54 four members walked 1260 to 1416 yards to one leader for one
            // quest, and walks to a partner took 11 % of all task time. Elite, group and death quests take the squad's
            // size. Members come from the leader's zone and within PartnerYards (400) of it.
            uint32 const maxSize = rank == 2 ? std::min<uint32>(2, settings.squadsMaxSize) : settings.squadsMaxSize;
            uint32 size = 1;
            for (std::size_t j = i + 1; j < list.size() && size < maxSize; ++j)
            {
                Player* member = list[j];
                if (taken.count(member->GetGUID()) || member->GetMapId() != leader->GetMapId() ||
                    member->GetZoneId() != leader->GetZoneId() ||
                    std::abs(int32(member->GetLevel()) - int32(leader->GetLevel())) > maxGap ||
                    !member->IsWithinDistInMap(leader, settings.squadsPartnerYards))
                    continue;
                std::string error;
                if (!sSquadService.Set(guild, member->GetGUID(), leader->GetGUID(), error, false, questId))
                    continue;
                taken.insert(member->GetGUID());
                ++size;
                // Both look at the quest anew with the squad (a spot too much for one alone may do for both).
                for (Player* partner : { leader, member })
                {
                    if (BotRuntime* rt = OrderService::Instance().FindRuntime(partner->GetGUID()))
                        if (auto danger = rt->dangerousQuests.find(questId); danger != rt->dangerousQuests.end())
                            danger->second.until = 0;
                    if (auto left = _leftUntil.find(partner->GetGUID()); left != _leftUntil.end())
                        left->second.erase(questId);
                }
                LOG_INFO(ActivityLog, "activity: {} (level {}) and {} (level {}) team up for quest {} \"{}\" ({}): {} walks to {} ({:.0f} yd)",
                    member->GetName(), uint32(member->GetLevel()), leader->GetName(), uint32(leader->GetLevel()), questId,
                    QuestTitle(questId), WhyHard(guild.ownerAccount, questId, leader->GetLevel()), member->GetName(), leader->GetName(),
                    member->GetDistance2d(leader));
                PushBot(guild, member->GetGUID(), false);
            }
            if (size > 1)
            {
                taken.insert(leader->GetGUID());
                PushBot(guild, leader->GetGUID(), false);
            }
        }
    }
}

std::vector<std::string> QuestPartnerService::DescribeGuild(AdventureGuild const& guild) const
{
    std::vector<std::string> lines;
    if (auto owner = _hard.find(guild.ownerAccount); owner != _hard.end())
        for (auto const& [questId, marks] : owner->second)
        {
            uint8 level = 0;
            for (Mark const& mark : marks.deaths)
                level = std::max(level, mark.level);
            for (Mark const& mark : marks.tooHard)
                level = std::max(level, mark.level);
            std::string const why = WhyHard(guild.ownerAccount, questId, level);
            if (!why.empty())
                lines.push_back(Acore::StringFormat("Hard quest {} \"{}\": {} (for guild bots up to level {})", questId, QuestTitle(questId),
                    why, uint32(level)));
        }
    for (auto const& [guid, bot] : guild.bots)
    {
        Player* player = InWorld(guid);
        if (!player)
            continue;
        std::string kept;
        for (uint32 questId : Waiting(bot, player))
            kept += Acore::StringFormat("{}{} \"{}\"", kept.empty() ? "" : ", ", questId, QuestTitle(questId));
        if (!kept.empty())
            lines.push_back(Acore::StringFormat("{} keeps for a guild partner: {}", bot.name, kept));
    }
    if (lines.empty())
        lines.push_back("No quest waits for a guild partner.");
    return lines;
}
