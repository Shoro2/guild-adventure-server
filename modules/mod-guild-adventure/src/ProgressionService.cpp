/*
 * ProgressionService implementation. See ProgressionService.h.
 */

#include "ProgressionService.h"
#include "BotSettings.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "GuildBank.h"
#include "GuildBudget.h"
#include "GuildEvents.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"
#include <algorithm>

using namespace GuildAdventure;

namespace
{
    constexpr uint8 DeathKnightUnlockLevel = 55;

    uint32 ParseUnlockValue(UnlockTemplate const& unlock)
    {
        try
        {
            return uint32(std::stoul(unlock.value));
        }
        catch (...)
        {
            LOG_ERROR("module", "mod-guild-adventure: unlock {} has a non-numeric value '{}'", unlock.id, unlock.value);
            return 0;
        }
    }
}

char const* GuildAdventure::ToToken(UnlockState state)
{
    switch (state)
    {
        case UnlockState::Owned:     return "owned";
        case UnlockState::Available: return "available";
        case UnlockState::Locked:    return "locked";
        case UnlockState::Covered:   return "covered";
    }
    return "locked";
}

char const* GuildAdventure::ToToken(UnlockPayFrom from)
{
    switch (from)
    {
        case UnlockPayFrom::Bank:           return "bank";
        case UnlockPayFrom::BankThenLeader: return "bankleader";
        case UnlockPayFrom::Leader:         return "leader";
    }
    return "bank";
}

ProgressionService& ProgressionService::Instance()
{
    static ProgressionService instance;
    return instance;
}

int32 ProgressionService::AwardForEvent(AdventureGuild& guild, std::string const& eventType, ObjectGuid /*bot*/)
{
    int32 const points = sGuildAdventureMgr.GetRenownPoints(eventType);
    if (points)
        AddRenown(guild, points);
    return points;
}

void ProgressionService::AddRenown(AdventureGuild& guild, int32 points)
{
    if (points < 0 && uint32(-points) > guild.renownTotal)
        guild.renownTotal = 0;
    else
        guild.renownTotal = uint32(int64(guild.renownTotal) + points);

    uint8 const oldLevel = guild.renownLevel;
    RecalculateLevel(guild);

    if (guild.renownLevel > oldLevel)
        for (uint8 level = oldLevel + 1; level <= guild.renownLevel; ++level)
            RecordEvent(guild, "RENOWN_LEVEL", ObjectGuid::Empty, Acore::StringFormat("The guild reached renown level {}.", level), level);

    ApplyUnlocks(guild);
    sGuildAdventureMgr.SaveGuild(guild);
    PushGuildState(guild);
    if (guild.renownLevel != oldLevel)
        PushUnlocks(guild);
}

void ProgressionService::RecalculateLevel(AdventureGuild& guild)
{
    uint8 const maxLevel = sGuildAdventureMgr.GetMaxRenownLevel();
    uint8 level = 1;
    uint32 needed = 0;
    while (level < maxLevel)
    {
        uint32 const step = sGuildAdventureMgr.GetRenownToNext(level);
        if (!step || guild.renownTotal < needed + step)
            break;
        needed += step;
        ++level;
    }
    guild.renownLevel = level;
}

uint32 ProgressionService::GetPointsInLevel(AdventureGuild const& guild) const
{
    uint32 needed = 0;
    for (uint8 level = 1; level < guild.renownLevel; ++level)
        needed += sGuildAdventureMgr.GetRenownToNext(level);
    return guild.renownTotal >= needed ? guild.renownTotal - needed : 0;
}

bool ProgressionService::ApplyUnlocks(AdventureGuild& guild)
{
    Settings const& settings = GetSettings();
    bool changed = false;

    // The level follows the curve of ga_renown_level as it is now: it is stored with the guild and the curve can change
    // between two starts (2026-11-27: five times the points). A lower level takes no owned unlock away.
    uint8 const storedLevel = guild.renownLevel;
    RecalculateLevel(guild);
    if (guild.renownLevel != storedLevel)
        changed = true;

    for (UnlockTemplate const& unlock : sGuildAdventureMgr.GetUnlocks())
    {
        if (unlock.goldCost || unlock.renownLevel > guild.renownLevel || guild.ownedUnlocks.count(unlock.id))
            continue;

        guild.ownedUnlocks.insert(unlock.id);
        sGuildAdventureMgr.SaveUnlock(guild, unlock.id);
        changed = true;

        // The starting unlocks (level 1) are owned silently when the guild is founded.
        if (unlock.renownLevel > 1)
            RecordEvent(guild, "UNLOCK", ObjectGuid::Empty, Acore::StringFormat("Unlocked: {}.", unlock.description), unlock.renownLevel);
    }

    uint32 slots = settings.startSlots;
    uint32 offline = settings.offlineWindowStartMinutes;
    for (UnlockTemplate const& unlock : sGuildAdventureMgr.GetUnlocks())
    {
        if (!guild.ownedUnlocks.count(unlock.id))
            continue;
        if (unlock.kind == UnlockKind::Slots)
            slots = std::max(slots, ParseUnlockValue(unlock));
        else if (unlock.kind == UnlockKind::Offline)
            offline = std::max(offline, ParseUnlockValue(unlock));
    }

    // Unlocks only ever raise the values; a higher GM override stays.
    slots = std::min<uint32>(std::max<uint32>(slots, guild.unlockedSlots), settings.maxSlots);
    offline = std::min<uint32>(std::max<uint32>(offline, guild.offlineWindowMin), settings.offlineWindowMaxMinutes);
    if (slots != guild.unlockedSlots || offline != guild.offlineWindowMin)
    {
        guild.unlockedSlots = uint8(slots);
        guild.offlineWindowMin = uint16(offline);
        changed = true;
    }

    if (changed)
        sGuildAdventureMgr.SaveGuild(guild);
    return changed;
}

UnlockState ProgressionService::GetUnlockState(AdventureGuild const& guild, UnlockTemplate const& unlock) const
{
    if (guild.ownedUnlocks.count(unlock.id))
        return UnlockState::Owned;
    // A tier that gives no more than the guild has already (10 slots for gold when renown gave 30) would buy nothing.
    if (unlock.kind == UnlockKind::Slots && ParseUnlockValue(unlock) <= guild.unlockedSlots)
        return UnlockState::Covered;
    if (unlock.kind == UnlockKind::Offline && ParseUnlockValue(unlock) <= guild.offlineWindowMin)
        return UnlockState::Covered;
    return unlock.renownLevel <= guild.renownLevel ? UnlockState::Available : UnlockState::Locked;
}

UnlockPayFrom ProgressionService::GetPayFrom(AdventureGuild const& guild) const
{
    int32 const value = sBotSettings.GetForGuild(guild.ownerAccount, Setting::UnlocksPayFrom);
    return UnlockPayFrom(std::clamp<int32>(value, 0, 2));
}

uint64 ProgressionService::GetUnlockFunds(AdventureGuild const& guild) const
{
    UnlockPayFrom const from = GetPayFrom(guild);
    uint64 funds = 0;
    if (from != UnlockPayFrom::Leader)
        funds += GuildBank::Money(guild.ownerAccount);
    if (from != UnlockPayFrom::Bank)
        if (Player* leader = ObjectAccessor::FindPlayer(guild.leader))
            funds += leader->GetMoney();
    return funds;
}

namespace
{
    std::string Copper(uint64 copper)
    {
        return GuildAdventure::GuildBudget::MoneyText(uint32(std::min<uint64>(copper, UINT32_MAX)));
    }
}

UnlockBuyResult ProgressionService::BuyUnlock(AdventureGuild& guild, UnlockTemplate const& unlock, std::string const& who)
{
    UnlockBuyResult result;
    switch (GetUnlockState(guild, unlock))
    {
        case UnlockState::Owned:
            result.code = "BAD_REQUEST";
            result.message = "You own that unlock already.";
            return result;
        case UnlockState::Covered:
            result.code = "COVERED";
            result.message = unlock.kind == UnlockKind::Slots
                ? Acore::StringFormat("Your guild has {} bot slots already: {} would add nothing.", guild.unlockedSlots, unlock.description)
                : Acore::StringFormat("Your guild has an offline window of {} min already: {} would add nothing.", guild.offlineWindowMin,
                    unlock.description);
            return result;
        case UnlockState::Locked:
            result.code = "LOCKED";
            result.message = Acore::StringFormat("Reach renown level {} first.", unlock.renownLevel);
            return result;
        case UnlockState::Available:
            break;
    }

    uint64 const cost = unlock.goldCost;
    UnlockPayFrom const from = GetPayFrom(guild);
    uint64 const bank = from != UnlockPayFrom::Leader ? GuildBank::Money(guild.ownerAccount) : 0;
    Player* leader = from != UnlockPayFrom::Bank ? ObjectAccessor::FindPlayer(guild.leader) : nullptr;
    uint64 const purse = leader ? leader->GetMoney() : 0;
    uint64 const fromBank = std::min(bank, cost);
    uint64 const fromLeader = cost - fromBank;

    if (fromLeader > purse)
    {
        std::string const leaderText = leader ? Acore::StringFormat("the guild leader has {}", Copper(purse))
                                              : std::string("the guild leader is not in the world");
        std::string have;
        if (from == UnlockPayFrom::Bank)
            have = Acore::StringFormat("the guild bank has {}", Copper(bank));
        else if (from == UnlockPayFrom::Leader)
            have = leaderText;
        else
            have = Acore::StringFormat("the guild bank has {} and {}", Copper(bank), leaderText);
        result.code = "NOT_ENOUGH_GOLD";
        result.message = Acore::StringFormat("{} costs {}, but {}.", unlock.description, Copper(cost), have);
        return result;
    }

    std::string const why = Acore::StringFormat("bought the unlock: {}", unlock.description);
    if (fromBank && !GuildBank::Withdraw(guild.ownerAccount, fromBank, why))
    {
        result.code = "NOT_ENOUGH_GOLD";
        result.message = "The guild bank could not pay (the adventure guild has no WoW guild bank).";
        return result;
    }
    if (fromLeader)
    {
        leader->ModifyMoney(-int32(fromLeader));
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        leader->SaveGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
    }

    guild.ownedUnlocks.insert(unlock.id);
    sGuildAdventureMgr.SaveUnlock(guild, unlock.id);
    ApplyUnlocks(guild);

    std::string paid;
    if (fromBank)
        paid = Acore::StringFormat("{} from the guild bank", Copper(fromBank));
    if (fromLeader)
        paid += Acore::StringFormat("{}{} from {}'s purse", paid.empty() ? "" : " and ", Copper(fromLeader), leader->GetName());
    if (paid.empty())
        paid = "for free";

    LOG_INFO("module.ga.activity", "activity: adventure guild of account {} bought the unlock {} \"{}\" ({}; {}): {} bot slots, "
        "offline window {} min", guild.ownerAccount, unlock.id, unlock.description, paid, who, guild.unlockedSlots, guild.offlineWindowMin);
    RecordEvent(guild, "UNLOCK", ObjectGuid::Empty, Acore::StringFormat("Bought: {} ({}).", unlock.description, paid), unlock.renownLevel);
    sGuildAdventureMgr.SaveGuild(guild);
    PushGuildState(guild);
    PushUnlocks(guild);

    result.ok = true;
    result.message = Acore::StringFormat("Unlocked: {} ({}).", unlock.description, paid);
    return result;
}

bool ProgressionService::HasFeature(AdventureGuild const& guild, std::string const& key) const
{
    for (UnlockTemplate const& unlock : sGuildAdventureMgr.GetUnlocks())
        if (unlock.kind == UnlockKind::Feature && unlock.value == key && guild.ownedUnlocks.count(unlock.id))
            return true;
    return false;
}

std::vector<std::string> ProgressionService::GetFeatures(AdventureGuild const& guild) const
{
    std::vector<std::string> features;
    for (UnlockTemplate const& unlock : sGuildAdventureMgr.GetUnlocks())
        if (unlock.kind == UnlockKind::Feature && guild.ownedUnlocks.count(unlock.id) &&
            std::find(features.begin(), features.end(), unlock.value) == features.end())
            features.push_back(unlock.value);
    return features;
}

uint16 ProgressionService::GetMaxOfflineWindow() const
{
    uint32 best = GetSettings().offlineWindowStartMinutes;
    for (UnlockTemplate const& unlock : sGuildAdventureMgr.GetUnlocks())
        if (unlock.kind == UnlockKind::Offline)
            best = std::max(best, ParseUnlockValue(unlock));
    return uint16(std::min<uint32>(best, GetSettings().offlineWindowMaxMinutes));
}

bool ProgressionService::IsDeathKnightUnlocked(AdventureGuild const& guild) const
{
    if (CharacterCacheEntry const* leader = sCharacterCache->GetCharacterCacheByGuid(guild.leader))
        if (leader->Level >= DeathKnightUnlockLevel)
            return true;

    for (auto const& [guid, bot] : guild.bots)
        if (bot.snapshot.level >= DeathKnightUnlockLevel)
            return true;
    return false;
}

void ProgressionService::SetSlots(AdventureGuild& guild, uint8 slots)
{
    guild.unlockedSlots = std::clamp<uint8>(slots, 1, GetSettings().maxSlots);
    sGuildAdventureMgr.SaveGuild(guild);
    PushGuildState(guild);
    PushUnlocks(guild); // tiers the new count covers
}

void ProgressionService::SetOfflineWindow(AdventureGuild& guild, uint16 minutes)
{
    guild.offlineWindowMin = std::min<uint16>(minutes, GetSettings().offlineWindowMaxMinutes);
    sGuildAdventureMgr.SaveGuild(guild);
    PushGuildState(guild);
    PushUnlocks(guild);
}
