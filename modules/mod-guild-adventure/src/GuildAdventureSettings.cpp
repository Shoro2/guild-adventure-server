/*
 * Settings of the guild management part of the module. See GuildAdventureSettings.h.
 */

#include "GuildAdventureSettings.h"
#include "Config.h"
#include "Log.h"
#include <algorithm>
#include <cmath>

namespace
{
    GuildAdventure::Settings sSettings;
}

void GuildAdventure::LoadSettings()
{
    Settings settings;

    // Playerbots allows at most Playerbots.MaxAddedBots bots per master; the client lists at most 10 characters
    // per account, which is what a bot account may hold.
    // A guild is at most 40 people: the leader and 39 bots (the owner, 2026-11-27).
    settings.maxSlots = uint8(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.MaxSlots", 39), 1, 39));
    settings.startSlots = uint8(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.StartSlots", 3), 1, settings.maxSlots));
    settings.botAccountPrefix = sConfigMgr->GetOption<std::string>("GuildAdventure.BotAccount.Prefix", "GABOT");
    settings.botAccountMaxCharacters = uint8(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.BotAccount.MaxCharacters", 10), 1, 10));
    settings.offlineWindowMaxMinutes = uint16(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.OfflineWindow.MaxMinutes", 480), 0, 24 * 60));
    settings.offlineWindowStartMinutes = uint16(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.OfflineWindow.StartMinutes", 60), 0,
        settings.offlineWindowMaxMinutes));
    settings.autoAdvanceDefault = sConfigMgr->GetOption<bool>("GuildAdventure.AutoAdvanceDefault", true);
    settings.protocolMaxMsgPerSecond = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("GuildAdventure.Protocol.MaxMsgPerSecond", 20));
    settings.eventLogKeepDays = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.EventLog.KeepDays", 30), 3650);
    settings.guildNameFormat = sConfigMgr->GetOption<std::string>("GuildAdventure.Guild.NameFormat", "{} Adventurers");
    settings.achievementsShare = sConfigMgr->GetOption<bool>("GuildAdventure.Achievements.Share", true);
    settings.achievementsRenownPerPoint = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Achievements.RenownPerPoint", 1), 100);
    settings.renownRate = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.Renown.Rate", 1.0f), 0.0f, 100.0f);
    settings.milestonesEnable = sConfigMgr->GetOption<bool>("GuildAdventure.Milestones.Enable", true);
    settings.milestonesScanSeconds = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Milestones.ScanSeconds", 30), 5, 600);
    settings.milestonesLastTierFactor = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Milestones.LastTierFactor", 2), 1, 10);
    settings.storageEnable = sConfigMgr->GetOption<bool>("GuildAdventure.Storage.Enable", true);
    settings.storageReagents = sConfigMgr->GetOption<bool>("GuildAdventure.Storage.Reagents", true);
    settings.lootFilterEnable = sConfigMgr->GetOption<bool>("GuildAdventure.LootFilter.Enable", true);
    settings.lootAllowSell = sConfigMgr->GetOption<bool>("GuildAdventure.LootFilter.AllowSell", true);
    settings.lootAllowDisenchant = sConfigMgr->GetOption<bool>("GuildAdventure.LootFilter.AllowDisenchant", true);
    settings.lootAllowDelete = sConfigMgr->GetOption<bool>("GuildAdventure.LootFilter.AllowDelete", true);
    settings.lootMaxRules = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.LootFilter.MaxRules", 30), 1, 100);
    settings.lootSellJunk = sConfigMgr->GetOption<bool>("GuildAdventure.LootFilter.SellJunk", true);
    settings.lootSellJunkQuality = uint8(std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.LootFilter.SellJunkQuality", 1), 1));
    settings.lootKeepWornGear = sConfigMgr->GetOption<bool>("GuildAdventure.LootFilter.KeepWornGear", false);
    settings.storageUnwornBound = sConfigMgr->GetOption<bool>("GuildAdventure.Storage.UnwornBound", true);
    settings.lootSpareQuestItems = sConfigMgr->GetOption<bool>("GuildAdventure.LootFilter.SpareQuestItems", true);
    settings.lootWornGear = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.LootFilter.WornGear", 1), 2);
    settings.lootOpenContainers = sConfigMgr->GetOption<bool>("GuildAdventure.LootFilter.OpenContainers", true);
    settings.storageOpenContainers = sConfigMgr->GetOption<bool>("GuildAdventure.Storage.OpenContainers", true);
    settings.storageDarkmoonCards = sConfigMgr->GetOption<bool>("GuildAdventure.Storage.DarkmoonCards", true);
    settings.storageReputationTurnIns = sConfigMgr->GetOption<bool>("GuildAdventure.Storage.ReputationTurnIns", true);
    settings.auctionSellUnsold = sConfigMgr->GetOption<bool>("GuildAdventure.Auction.SellUnsold", true);
    settings.storageLockboxMinutes = sConfigMgr->GetOption<uint32>("GuildAdventure.Storage.LockboxMinutes", 2);
    settings.shoppingEnable = sConfigMgr->GetOption<bool>("GuildAdventure.Shopping.Enable", true);
    settings.shoppingMaxPerVisit = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Shopping.MaxPerVisit", 50000), 100000000);
    settings.shoppingMaxPerDay = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Shopping.MaxPerDay", 500000), 1000000000);
    settings.shoppingFromPurse = sConfigMgr->GetOption<bool>("GuildAdventure.Shopping.FromPurse", false);
    settings.storageReserveForDungeons = sConfigMgr->GetOption<bool>("GuildAdventure.Storage.ReserveForDungeons", false);
    settings.autoLootEnable = sConfigMgr->GetOption<bool>("GuildAdventure.AutoLoot.Enable", true);
    settings.autoLootRange = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.AutoLoot.Range", 10.0f), 1.0f, 30.0f);
    settings.autoLootScanMs = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.AutoLoot.ScanIntervalMs", 250), 5000);
    settings.autoLootArmByKill = sConfigMgr->GetOption<bool>("GuildAdventure.AutoLoot.ArmByKill", true);
    settings.plannerSpread = sConfigMgr->GetOption<bool>("GuildAdventure.Planner.Spread", true);
    settings.plannerBudgetMs = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Planner.BudgetMs", 4), 1000);
    settings.plannerSearchesPerTick = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Planner.SearchesPerTick", 0), 1000);
    settings.plannerRouteCache = sConfigMgr->GetOption<bool>("GuildAdventure.Planner.RouteCache", true);
    settings.perfSlowTickMs = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Perf.SlowTickMs", 50), 60000);
    settings.auctionEnable = sConfigMgr->GetOption<bool>("GuildAdventure.Auction.Enable", true);
    settings.auctionIntervalSeconds = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Auction.IntervalSeconds", 300), 10, 86400);
    settings.auctionHours = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Auction.Hours", 24), 1, 48);
    settings.auctionPriceFactor = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.Auction.PriceFactor", 0.85f), 0.01f, 100.0f);
    settings.auctionMinPrice = sConfigMgr->GetOption<uint32>("GuildAdventure.Auction.MinPrice", 1000);
    settings.auctionMinPriceTradeGoods = sConfigMgr->GetOption<uint32>("GuildAdventure.Auction.MinPrice.TradeGoods", 850);
    settings.auctionMaxStacksPerItem = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Auction.MaxStacksPerItem", 10), 1, 100);
    settings.auctionMaxRules = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Auction.MaxRules", 100), 1, 1000);
    settings.autoLearnClassSpells = sConfigMgr->GetOption<bool>("GuildAdventure.AutoLearnClassSpells", true);
    settings.leaderOfflinePlay = sConfigMgr->GetOption<bool>("GuildAdventure.Leader.OfflinePlay", true);
    settings.leaderWatchOnLogin = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Leader.WatchOnLogin", 1), 2);
    settings.zonesWithinLevelCap = sConfigMgr->GetOption<bool>("GuildAdventure.Zones.WithinLevelCap", true);
    settings.questsFilter = sConfigMgr->GetOption<bool>("GuildAdventure.Quests.Filter", true);
    settings.questsLearnedBlockBots = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Quests.LearnedBlockBots", 2), 40);
    settings.questsEscorts = sConfigMgr->GetOption<bool>("GuildAdventure.Quests.Escorts", true);
    settings.questsEvents = sConfigMgr->GetOption<bool>("GuildAdventure.Quests.Events", true);
    settings.pathsStayOnGround = sConfigMgr->GetOption<bool>("GuildAdventure.Paths.StayOnGround", true);
    settings.roadsEnable = sConfigMgr->GetOption<bool>("GuildAdventure.Roads.Enable", true);
    settings.roadsMinDistance = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.Roads.MinDistance", 350.0f), 100.0f, 5000.0f);
    settings.roadsCostFromLevelsAbove = std::clamp(sConfigMgr->GetOption<int32>("GuildAdventure.Roads.CostFromLevelsAbove", 4), 1, 20);
    settings.roadsPairLevels = std::clamp(sConfigMgr->GetOption<int32>("GuildAdventure.Roads.PairLevels", 0), 0, 10);
    settings.walksGroupFrom = std::clamp(sConfigMgr->GetOption<uint32>("GuildAdventure.Walks.GroupFrom", 2), 2u, 10u);
    settings.walksMaxLevelsAbove = std::clamp(sConfigMgr->GetOption<int32>("GuildAdventure.Walks.MaxLevelsAbove", 3), 0, 10);
    settings.walksPostponeMinutes = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Walks.PostponeMinutes", 30), 1, 240);
    settings.walksPlaceCheck = sConfigMgr->GetOption<bool>("GuildAdventure.Walks.PlaceCheck", true);
    settings.travelHearthstone = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Travel.Hearthstone", 1), 2);
    settings.grindEnable = sConfigMgr->GetOption<bool>("GuildAdventure.Grind.Enable", true);
    settings.grindLevelsBelow = std::clamp(sConfigMgr->GetOption<int32>("GuildAdventure.Grind.LevelsBelow", 3), 0, 10);
    settings.grindLevelsAbove = std::clamp(sConfigMgr->GetOption<int32>("GuildAdventure.Grind.LevelsAbove", 0), 0, 5);
    settings.grindMaxDistance = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.Grind.MaxDistance", 600.0f), 100.0f, 3000.0f);
    settings.farmLevelsBelow = std::clamp(sConfigMgr->GetOption<int32>("GuildAdventure.Farm.LevelsBelow", 4), 0, 20);
    settings.farmGroundRadius = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.Farm.GroundRadius", 80.0f), 30.0f, 200.0f);
    settings.farmMinSpawns = std::clamp(sConfigMgr->GetOption<int32>("GuildAdventure.Farm.MinSpawns", 4), 1, 20);
    settings.farmMaxDistance = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.Farm.MaxDistance", 0.0f), 0.0f, 20000.0f);
    settings.vendorTrips = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Vendor.Trips", 0), 2);
    settings.questsItemsFromGuild = sConfigMgr->GetOption<bool>("GuildAdventure.Quests.ItemsFromGuild", true);
    settings.questsAbandonGrey = sConfigMgr->GetOption<bool>("GuildAdventure.Quests.AbandonGrey", true);
    settings.professionsLearnInPlace = sConfigMgr->GetOption<bool>("GuildAdventure.Professions.LearnInPlace", true);
    settings.professionsGatherDetour = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.Professions.GatherDetour", 40.0f), 0.0f, 100.0f);
    settings.professionsCatchUpGathering = sConfigMgr->GetOption<bool>("GuildAdventure.Professions.CatchUpGathering", true);
    settings.professionsCatchUpMinGap = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Professions.CatchUpMinGap", 25), 300);
    settings.professionsFocusBehind = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Professions.FocusBehind", 50), 300);
    settings.professionsGreenMinChance = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Professions.GreenMinChance", 20), 100);
    settings.professionsCookStock = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Professions.CookStock", 40), 1000);
    settings.professionsCookMinutes = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Professions.CookMinutes", 15), 1, 720);
    settings.professionsAmmoStock = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Professions.AmmoStock", 5000), 100000);
    settings.professionsSkillCraftStock = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Professions.SkillCraftStock", 200), 100000);
    settings.professionsConsumableStock = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Professions.ConsumableStock", 20), 200);
    settings.professionsEnchantScrolls = sConfigMgr->GetOption<bool>("GuildAdventure.Professions.EnchantScrolls", true);
    settings.professionsEnchantMinItemLevel = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Professions.EnchantMinItemLevel", 60), 1, 300);
    settings.professionsProcessAbove = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Professions.ProcessAbove", 1000), 100000);
    settings.professionsProcessStock = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Professions.ProcessStock", 200), 100000);
    settings.professionsBuyRecipes = sConfigMgr->GetOption<bool>("GuildAdventure.Professions.BuyRecipes", true);
    settings.professionsRecipeMaxPrice = sConfigMgr->GetOption<uint32>("GuildAdventure.Professions.RecipeMaxPrice", 10000);
    settings.professionsRecipeYards = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.Professions.RecipeYards", 300.0f), 0.0f, 2000.0f);
    settings.professionsDisenchantSurplus = sConfigMgr->GetOption<bool>("GuildAdventure.Professions.DisenchantSurplus", true);
    settings.professionsCraftAtHub = sConfigMgr->GetOption<bool>("GuildAdventure.Professions.CraftAtHub", true);
    settings.professionsCraftGear = sConfigMgr->GetOption<bool>("GuildAdventure.Professions.CraftGear", false);
    settings.professionsCraftOrders = sConfigMgr->GetOption<bool>("GuildAdventure.Professions.CraftOrders", true);
    settings.professionsDisenchantForOrders = sConfigMgr->GetOption<bool>("GuildAdventure.Professions.DisenchantForOrders", true);
    settings.professionsCraftBatch = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Professions.CraftBatch", 5), 1, 50);
    settings.botGuildChatMinSeconds = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.BotGuildChat.MinSeconds", 30), 3600);
    settings.botGuildChatMaxSeconds = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.BotGuildChat.MaxSeconds", 60),
        settings.botGuildChatMinSeconds, 3600);
    settings.restEnable = sConfigMgr->GetOption<bool>("GuildAdventure.Rest.Enable", true);
    settings.restBelowHealth = uint8(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Rest.BelowHealth", 65), 0, 95));
    settings.restBelowMana = uint8(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Rest.BelowMana", 40), 0, 95));
    settings.restUntilHealth = uint8(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Rest.UntilHealth", 85),
        settings.restBelowHealth, 100));
    settings.restUntilMana = uint8(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Rest.UntilMana", 75),
        settings.restBelowMana, 100));
    settings.restSaferPlaceYards = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.Rest.SaferPlaceYards", 40.0f), 0.0f, 100.0f);
    settings.selfHealBelow = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Fight.SelfHealBelow", 45), 90);
    settings.potionBelowHealth = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Fight.PotionBelowHealth", 35), 90);
    settings.potionBelowMana = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Fight.PotionBelowMana", 15), 90);
    settings.consecrationMinFoes = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Fight.ConsecrationMinFoes", 2), 1, 10);
    settings.fightInterrupt = sConfigMgr->GetOption<bool>("GuildAdventure.Fight.Interrupt", true);
    settings.fightTemplatesByDefault = sConfigMgr->GetOption<bool>("GuildAdventure.Fight.TemplatesByDefault", true);
    settings.provisionsConjureBelow = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Provisions.ConjureBelow", 4), 40);
    settings.restAirAfterSeconds = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Rest.AirAfterSeconds", 90), 170);
    settings.restBeforeWalk = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Rest.BeforeWalk", 0), 100);
    settings.pullCareful = sConfigMgr->GetOption<bool>("GuildAdventure.Pull.Careful", true);
    settings.restMaxSeconds = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Rest.MaxSeconds", 60), 5, 600);
    settings.provisionsMinLevel = uint8(std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Provisions.MinLevel", 8), 80));
    settings.provisionsStack = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Provisions.Stack", 20), 2, 200);
    settings.provisionsTripWhenOut = sConfigMgr->GetOption<bool>("GuildAdventure.Provisions.TripWhenOut", true);
    settings.provisionsTripWhenOutYards = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.Provisions.TripWhenOutYards", 0.0f), 0.0f, 3000.0f);
    settings.ammoHunterStock = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Ammo.HunterStock", 2000), 200, 20000);
    settings.ammoHunterStockBelow20 = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Ammo.HunterStockBelow20", 1000), 200, 20000);
    settings.ammoHunterStockBelow5 = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Ammo.HunterStockBelow5", 200), 200, 20000);
    settings.ammoRestockBelow = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Ammo.RestockBelow", 100), 5000);
    settings.ammoOthersStacks = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Ammo.OthersStacks", 1), 1, 10);
    settings.ammoTopUpYards = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.Ammo.TopUpYards", 200.0f), 0.0f, 3000.0f);
    settings.provisionsDrinkStack = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Provisions.DrinkStack", 40), 2, 200);
    settings.provisionsHubYards = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.Provisions.HubYards", 150.0f), 0.0f, 3000.0f);
    settings.provisionsTripWhenLow = sConfigMgr->GetOption<bool>("GuildAdventure.Provisions.TripWhenLow", false);
    settings.provisionsMinMoney = sConfigMgr->GetOption<uint32>("GuildAdventure.Provisions.MinMoney", 100);
    settings.provisionsBudgetPercent = uint8(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Provisions.BudgetPercent", 50), 1, 100));
    settings.provisionsRestockBelow = uint8(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Provisions.RestockBelow", 4), 1, 20));
    settings.activityLogFights = sConfigMgr->GetOption<bool>("GuildAdventure.ActivityLog.Fights", true);
    settings.specsEnable = sConfigMgr->GetOption<bool>("GuildAdventure.Specs.Enable", true);
    settings.escapeEnable = sConfigMgr->GetOption<bool>("GuildAdventure.Escape.Enable", true);
    settings.escapeMargin = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.Escape.Margin", 1.0f), 0.1f, 10.0f);
    settings.escapeMaxSeconds = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Escape.MaxSeconds", 35), 10, 120);
    settings.escapeMaxFoes = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Escape.MaxFoes", 3), 20);
    settings.escapeStepAside = sConfigMgr->GetOption<bool>("GuildAdventure.Escape.StepAside", true);
    settings.riskEnable = sConfigMgr->GetOption<bool>("GuildAdventure.Risk.Enable", true);
    settings.riskMaxFoeLevelAbove = uint8(std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Risk.MaxFoeLevelAbove", 3), 10));
    settings.riskMaxFightCost = float(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Risk.MaxFightCostPercent", 80), 10, 1000)) / 100.0f;
    settings.riskCautiousDeaths = uint8(std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Risk.CautiousDeaths", 4), 100));
    settings.riskCautiousWindowMinutes = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Risk.CautiousWindowMinutes", 20), 1, 30);
    settings.riskCautiousMinutes = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Risk.CautiousMinutes", 15), 1, 240);
    settings.riskRescueDeaths = uint8(std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Risk.RescueDeaths", 3), 100));
    settings.riskRescueWindowMinutes = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Risk.RescueWindowMinutes", 10), 1, 30);
    settings.classAbilitiesEnable = sConfigMgr->GetOption<bool>("GuildAdventure.ClassAbilities.Enable", true);
    settings.classAbilitiesHunterPet = sConfigMgr->GetOption<bool>("GuildAdventure.ClassAbilities.HunterPet", true);
    settings.classAbilitiesPetTalents = sConfigMgr->GetOption<bool>("GuildAdventure.ClassAbilities.PetTalents", true);
    settings.poisonsEnable = sConfigMgr->GetOption<bool>("GuildAdventure.Poisons.Enable", true);
    settings.poisonsStock = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Poisons.Stock", 10), 40);
    settings.poisonsPair = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Poisons.Pair", 0), 2);
    settings.classAbilitiesRuneforging = sConfigMgr->GetOption<bool>("GuildAdventure.ClassAbilities.Runeforging", true);
    settings.deathKnightsSkipStart = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.DeathKnights.SkipStart", 1), 2);
    settings.storageSupplyProvisions = sConfigMgr->GetOption<bool>("GuildAdventure.Storage.Supply.Provisions", true);
    settings.storageSupplyPotions = sConfigMgr->GetOption<bool>("GuildAdventure.Storage.Supply.Potions", true);
    settings.storageSupplyBandages = sConfigMgr->GetOption<bool>("GuildAdventure.Storage.Supply.Bandages", true);
    settings.storageSupplyScrolls = sConfigMgr->GetOption<bool>("GuildAdventure.Storage.Supply.Scrolls", true);
    settings.storageSupplyElixirs = sConfigMgr->GetOption<bool>("GuildAdventure.Storage.Supply.Elixirs", false);
    settings.storageSupplyGear = sConfigMgr->GetOption<bool>("GuildAdventure.Storage.Supply.Gear", true);
    settings.gearBuyEnable = sConfigMgr->GetOption<bool>("GuildAdventure.Gear.Buy.Enable", true);
    settings.gearBuyNearYards = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.Gear.Buy.NearYards", 300.0f), 0.0f, 3000.0f);
    settings.gearFillEmptySlots = sConfigMgr->GetOption<bool>("GuildAdventure.Gear.FillEmptySlots", true);
    settings.gearWarnGap = sConfigMgr->GetOption<uint32>("GuildAdventure.Gear.WarnGap", 8);
    settings.budgetItemPercent = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Budget.ItemPercent", 10), 100);
    settings.budgetBotAllowance = sConfigMgr->GetOption<uint32>("GuildAdventure.Budget.BotAllowance", 10000);
    settings.budgetMemberPercent = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Budget.MemberPercent", 50), 100);
    settings.guildTaxPercent = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Guild.TaxPercent", 10), 100);
    settings.unlocksPayFrom = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Unlocks.PayFrom", 0), 2);
    settings.guildTaxHandovers = sConfigMgr->GetOption<bool>("GuildAdventure.Guild.TaxHandovers", false);
    settings.dungeonsOpenDoors = sConfigMgr->GetOption<bool>("GuildAdventure.Dungeons.OpenDoors", true);
    settings.dungeonsMaxRuns = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Dungeons.MaxRuns", 10), 1, 50);
    settings.dungeonsFieldRepair = sConfigMgr->GetOption<bool>("GuildAdventure.Dungeons.FieldRepair", true);
    settings.dungeonsTeleport = sConfigMgr->GetOption<bool>("GuildAdventure.Dungeons.Teleport", true);
    settings.dungeonsUnlockInOrder = sConfigMgr->GetOption<bool>("GuildAdventure.Dungeons.UnlockInOrder", true);
    settings.dungeonsTankLeadSeconds = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Dungeons.TankLeadSeconds", 4), 30);
    settings.dungeonsEmergencyHealth = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Dungeons.EmergencyHealth", 50), 100);
    settings.dungeonsCarefulPulls = sConfigMgr->GetOption<bool>("GuildAdventure.Dungeons.CarefulPulls", true);
    settings.dungeonsCarefulPullPack = std::max<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Dungeons.CarefulPullPack", 3), 2);
    settings.dungeonsFocusFleeing = sConfigMgr->GetOption<bool>("GuildAdventure.Dungeons.FocusFleeing", true);
    settings.dungeonsMinReadiness = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Dungeons.MinReadiness", 0), 100);
    settings.dungeonsHopWhenNoWay = sConfigMgr->GetOption<bool>("GuildAdventure.Dungeons.HopWhenNoWay", true);
    settings.dungeonsTakeOwnerAlong = sConfigMgr->GetOption<bool>("GuildAdventure.Dungeons.TakeOwnerAlong", true);
    settings.dungeonsEngageMargin = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Dungeons.EngageMargin", 8), 30);
    settings.dungeonsBigPullAt = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Dungeons.BigPullAt", 4), 2, 10);
    settings.dungeonsGateHealth = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Dungeons.GateHealth", 90), 100);
    settings.dungeonsGateHealerMana = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Dungeons.GateHealerMana", 80), 100);
    settings.dungeonsGateDamageMana = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Dungeons.GateDamageMana", 60), 100);
    settings.dungeonsSuppliesByDefault = sConfigMgr->GetOption<bool>("GuildAdventure.Dungeons.SuppliesByDefault", true);
    settings.dungeonsLingerSeconds = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Dungeons.LingerSeconds", 30), 600);
    settings.rolesEnable = sConfigMgr->GetOption<bool>("GuildAdventure.DungeonRoles.Enable", true);
    settings.statWeightsEnable = sConfigMgr->GetOption<bool>("GuildAdventure.StatWeights.Enable", true);
    settings.rolesFromStorage = sConfigMgr->GetOption<bool>("GuildAdventure.DungeonRoles.FromStorage", true);
    settings.rolesWearForLevelling = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.DungeonRoles.WearForLevelling", 2), 2);
    settings.rolesRespecCost = sConfigMgr->GetOption<uint32>("GuildAdventure.DungeonRoles.RespecCost", 0);
    settings.squadsEnable = sConfigMgr->GetOption<bool>("GuildAdventure.Squads.Enable", true);
    settings.squadsMaxSize = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Squads.MaxSize", 5), 2, 5);
    settings.squadsMaxLevelGap = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Squads.MaxLevelGap", 3), 20);
    settings.squadsAuto = sConfigMgr->GetOption<bool>("GuildAdventure.Squads.Auto", false);
    settings.squadsWaitYards = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.Squads.WaitYards", 80.0f), 0.0f, 200.0f);
    settings.squadsMaxWaitSeconds = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Squads.MaxWaitSeconds", 120), 10, 600);
    settings.squadsCatchUpYards = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.Squads.CatchUpYards", 250.0f), 60.0f, 2000.0f);
    settings.squadsQuestPartners = sConfigMgr->GetOption<bool>("GuildAdventure.Squads.QuestPartners", true);
    settings.squadsCatchUpFallback = sConfigMgr->GetOption<bool>("GuildAdventure.Squads.CatchUpFallback", true);
    settings.squadsPartnerDeaths = std::min<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Squads.PartnerDeaths", 2), 20);
    settings.squadsPartnerHours = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Squads.PartnerHours", 6), 1, 72);
    settings.squadsPartnerTooHard = sConfigMgr->GetOption<bool>("GuildAdventure.Squads.PartnerTooHard", true);
    settings.squadsPartnerYards = std::clamp(sConfigMgr->GetOption<float>("GuildAdventure.Squads.PartnerYards", 400.0f), 50.0f, 10000.0f);
    settings.squadsPartnerMeetMinutes = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Squads.PartnerMeetMinutes", 8), 1, 60);
    settings.squadsPartnerMaxMinutes = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Squads.PartnerMaxMinutes", 60), 5, 600);
    settings.squadsPartnerRetryMinutes = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GuildAdventure.Squads.PartnerRetryMinutes", 30), 1, 600);
    settings.debug = sConfigMgr->GetOption<bool>("GuildAdventure.Debug", false);

    // Account names: prefix + 5-digit owner id + 2-digit pool index must fit MAX_ACCOUNT_STR (16).
    if (settings.botAccountPrefix.empty() || settings.botAccountPrefix.size() > 9)
    {
        LOG_ERROR("module", "mod-guild-adventure: GuildAdventure.BotAccount.Prefix must be 1-9 characters, using GABOT");
        settings.botAccountPrefix = "GABOT";
    }

    if (settings.guildNameFormat.find("{}") == std::string::npos)
    {
        LOG_ERROR("module", "mod-guild-adventure: GuildAdventure.Guild.NameFormat needs a {{}} for the leader's name, using '{{}} Adventurers'");
        settings.guildNameFormat = "{} Adventurers";
    }

    sSettings = settings;
}

GuildAdventure::Settings const& GuildAdventure::GetSettings()
{
    return sSettings;
}

int32 GuildAdventure::ScaleRenown(int64 points)
{
    double const scaled = std::llround(double(points) * sSettings.renownRate);
    return int32(std::clamp<double>(scaled, -2000000000.0, 2000000000.0));
}
