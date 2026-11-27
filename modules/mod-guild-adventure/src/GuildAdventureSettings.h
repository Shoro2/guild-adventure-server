/*
 * Settings of the guild management part of the module (mod_guild_adventure.conf, the design log section 3.7).
 * GuildAdventure.Enable and the take-control settings are read by GuildAdventureConfig / ControlSwapService.
 * Loaded on the world thread in WorldScript::OnAfterConfigLoad, so ".reload config" applies them too.
 */

#ifndef MOD_GUILD_ADVENTURE_SETTINGS_H
#define MOD_GUILD_ADVENTURE_SETTINGS_H

#include "Define.h"
#include <string>

namespace GuildAdventure
{
    struct Settings
    {
        uint8 startSlots = 3;
        uint8 maxSlots = 39;          // bots per guild; with the leader 40 people (the owner, 2026-11-27)
        std::string botAccountPrefix = "GABOT";
        uint8 botAccountMaxCharacters = 10;
        uint16 offlineWindowStartMinutes = 60;
        uint16 offlineWindowMaxMinutes = 480;
        bool autoAdvanceDefault = true;
        uint32 protocolMaxMsgPerSecond = 20;
        uint32 eventLogKeepDays = 30;     // ga_event_log rows older than this are deleted, 0 = keep everything
        std::string guildNameFormat = "{} Adventurers";
        bool achievementsShare = true;          // guild achievements go into every member's own achievement list
        uint32 achievementsRenownPerPoint = 1;  // renown per achievement point of a new guild achievement
        float renownRate = 1.0f;                // every renown a guild gains (events, achievements, tiers) times this
        // Tiered guild achievements (MilestoneService)
        bool milestonesEnable = true;           // the series count and give their tiers
        uint32 milestonesScanSeconds = 30;      // levels, skills, storage items and gold are read this often
        uint32 milestonesLastTierFactor = 2;    // the highest active tier of a series gives this times its points
        // Guild storage, loot filter, auto loot (docs/storage.md)
        bool storageEnable = true;              // the guild storage exists (loot routing, deposits, withdrawals)
        bool storageReagents = true;            // spells of guild characters take missing reagents from it (CP-7)
        bool storageSupplyProvisions = true;    // AI-played characters take food and drink from it, one piece when they eat or drink
        bool storageSupplyPotions = true;       // ... a healing or mana potion in a fight (Fight.PotionBelowHealth / .PotionBelowMana)
        bool storageSupplyBandages = true;      // ... a bandage in a rest, with First Aid
        bool storageSupplyScrolls = true;       // ... the best scroll of their main stat while they work on a task with fights
        bool storageSupplyElixirs = false;      // ... and a battle and a guardian elixir that help them (off: the owner named scrolls, not elixirs)
        bool storageSupplyGear = true;          // ... and equipment that is an upgrade for them
        bool gearBuyEnable = true;              // AI-played characters buy weapons and armour that are upgrades at vendors
        float gearBuyNearYards = 300.0f;        // ... and walk to a vendor this near that sells one (0 = only vendors they visit anyway)
        bool gearFillEmptySlots = true;         // armour in the bags goes on where the bot wears nothing, whatever its kind
        uint32 gearWarnGap = 8;                 // a warning when the weapon or the body armour is this many item levels below the level (0 = never)
        uint32 budgetItemPercent = 10;          // gear and quest items: one item at most this share of the guild's gold (GuildBudget.h)
        uint32 budgetBotAllowance = 10000;      // ... and this much copper per guild character over its whole life
        uint32 budgetMemberPercent = 50;        // what one guild character gives of its own money for one such purchase at most
        uint32 guildTaxPercent = 10;            // share of a roster bot's income that goes into the guild bank (GuildBank.h)
        uint32 unlocksPayFrom = 0;              // who pays a gold unlock (ProgressionService::BuyUnlock): 0 the guild bank, 1 the guild bank and then the leader, 2 the leader
        bool guildTaxHandovers = false;         // ... money handed over by trade or mail within the guild is taxed too
        bool lootFilterEnable = true;           // looted items of guild characters go through the guild's rules
        bool lootAllowSell = true;
        bool lootAllowDisenchant = true;
        bool lootAllowDelete = true;
        uint32 lootMaxRules = 30;
        bool lootSellJunk = true;               // loot no rule decides that is junk is sold at once (LootService::IsJunk)
        uint8 lootSellJunkQuality = 1;          // ... up to this quality: 0 grey only, 1 grey and white
        bool lootKeepWornGear = false;          // grey and white gear a guild character would wear is no junk
        bool storageUnwornBound = true;         // the guild storage takes never-worn bind-on-pickup loot (docs/storage.md)
        bool lootSpareQuestItems = true;        // quest items no quest needs any more are sold or deleted (QI1)
        uint32 lootWornGear = 1;                // worn soulbound green/blue gear a bot will not wear: 0 keep, 1 disenchant or sell, 2 disenchant only
        bool lootOpenContainers = true;         // bots open the containers they loot (docs/storage.md "Containers")
        bool storageOpenContainers = true;      // bots take the guild storage's containers out to open them
        bool storageDarkmoonCards = true;       // the Darkmoon Faire cards and decks bots carry go into the guild storage
        bool storageReputationTurnIns = true;   // the reputation turn-ins bots carry go into the guild storage (RT1)
        bool auctionSellUnsold = true;          // the storage's unsold auctions are sold at the vendor price when they come back (AH2)
        uint32 storageLockboxMinutes = 2;       // a rogue or blacksmith opens one lockbox of the storage per this many minutes
        bool shoppingEnable = true;             // bots buy what the guild storage lacks of its shopping list (ShoppingList.h)
        uint32 shoppingMaxPerVisit = 50000;     // ... at most this much copper of the guild bank per vendor visit
        uint32 shoppingMaxPerDay = 500000;      // ... and per day
        bool shoppingFromPurse = false;         // ... and what the guild bank lacks from the bot's own gold
        bool storageReserveForDungeons = false; // what a dungeon run uses may take into the reserves (off: the reserves hold for runs too)
        bool autoLootEnable = true;             // guild characters loot, skin and gather within autoLootRange
        float autoLootRange = 10.0f;
        uint32 autoLootScanMs = 250;
        bool autoLootArmByKill = true;          // corpses are looked for only after a kill the character has the loot of (PF-3)
        // The planner's cost on the world thread (docs/performance.md PF-1, PF-2, PF-0)
        bool plannerSpread = true;              // each character is stepped at a phase of its own, within a budget per world tick
        uint32 plannerBudgetMs = 4;             // ... the planner's time per world tick (at least a quarter of the last tick)
        uint32 plannerSearchesPerTick = 0;      // ... expensive searches (FindFlight, FindErrand, ChooseZone) per world tick (0 = no cap)
        bool plannerRouteCache = true;          // the path finder's walks (WalkReaches) are remembered
        uint32 perfSlowTickMs = 50;             // an activity line for a world tick in which the planner took longer (0 = never)
        // Auction house surplus of the guild storage (docs/storage.md "Auction house")
        bool auctionEnable = true;
        uint32 auctionIntervalSeconds = 300;    // how often the surplus is posted
        uint32 auctionHours = 24;               // auction duration
        float auctionPriceFactor = 0.85f;       // default price = max(vendor price, minimum) x quality x this
        uint32 auctionMinPrice = 1000;          // copper per item, the AH bot's minimum for most classes
        uint32 auctionMinPriceTradeGoods = 850; // copper per item for trade goods (the AH bot's)
        uint32 auctionMaxStacksPerItem = 10;    // auctions of one kind at the same time
        uint32 auctionMaxRules = 100;           // auction rules per guild
        bool autoLearnClassSpells = true;       // guild bots learn their class trainer's spells where they are (no trip)
        bool leaderOfflinePlay = true;          // a leader with an order plays on in the offline window, also after a restart
        uint32 leaderWatchOnLogin = 1;          // logging into a leader with an order keeps the AI playing it (watch mode):
                                                // 0 never, 1 when it was playing by itself, 2 always
        bool zonesWithinLevelCap = true;        // only zones whose level range ends at or below the server's MaxPlayerLevel
        bool questsFilter = true;               // AI-played characters do not take quests the planner cannot finish (CP-9)
        uint32 questsLearnedBlockBots = 2;      // a quest this many bots made no progress on is left alone by all (0 = off)
        bool questsEvents = true;               // quests that complete through a talk, an event at the giver or an NPC to lead are played
        bool questsEscorts = true;              // escort quests are taken and the NPC is followed
        bool pathsStayOnGround = true;          // AI-played characters do not start a movement that leaves the ground (CP-10)
        bool roadsEnable = true;                // long walks of the order planner follow routes of the road map
        float roadsMinDistance = 350.0f;        // walks longer than this (yards) take a route
        int32 roadsCostFromLevelsAbove = 4;     // a route costs more past creatures this many levels above the walker
        int32 roadsPairLevels = 0;              // each of two creatures that stand together counts this much higher on the
                                                // danger map (0: a pair is a group the risk model judges, walksGroupFrom)
        uint32 walksGroupFrom = 2;              // creatures standing together on a way are a group from this many: the
                                                // walk gate asks the risk model whether the walker can take them
        int32 walksMaxLevelsAbove = 3;          // a task's walk may pass creatures this many levels above the bot
        uint32 walksPostponeMinutes = 30;       // a target behind stronger creatures is left alone this long
        bool walksPlaceCheck = true;            // no hand-in or pickup where every creature around is above the bot
        uint32 travelHearthstone = 1;           // 0 only in an emergency, 1 to another continent or a capital, 2 whenever it shortens a travel
        bool grindEnable = true;                // a bot without quest work in its zone grinds
        int32 grindLevelsBelow = 3;             // a grinding bot fights creatures down to this many levels below it ...
        int32 grindLevelsAbove = 0;             // ... and up to this many above
        float grindMaxDistance = 600.0f;        // grind spots are looked for this far from the bot (yards)
        // The order FARM (docs/orders.md "Farm orders")
        int32 farmLevelsBelow = 4;              // a farming bot fights only creatures at least this many levels below it
        float farmGroundRadius = 80.0f;         // a hunting ground: the creatures within this many yards of its middle
        int32 farmMinSpawns = 4;                // ... of which it needs this many (unless no place has them)
        float farmMaxDistance = 0.0f;           // hunting grounds are looked for this far from the bot (yards, 0 = no limit)
        bool questsItemsFromGuild = true;       // a quest item the bot lacks comes from the storage, a member or a recipe
        bool questsAbandonGrey = true;          // an AI-played character abandons quests that are grey for it
        bool professionsLearnInPlace = true;    // planned professions, their ranks and recipes are learned in place
        float professionsGatherDetour = 40.0f;  // a node the bot can gather this close to its way is fetched (0 = off)
        bool professionsCatchUpGathering = true; // AUTO: a gathering skill too low for the bot's zone is caught up before it quests on
        uint32 professionsCatchUpMinGap = 25;   // ... only when the skill is at least this many points behind (a catch-up is a journey)
        uint32 professionsGreenMinChance = 20;  // crafting for skill: no green recipe whose chance of a point a cast is below (%)
        uint32 professionsFocusBehind = 50;     // profession mode: a profession more than this many points behind the bot's highest
                                                // is worked on alone (0: the professions always take turns)
        uint32 professionsCookStock = 40;       // a cook makes food for the guild until the storage holds this many of a kind (0: never)
        uint32 professionsCookMinutes = 15;     // ... and looks again this many minutes after it found nothing to cook
        uint32 professionsAmmoStock = 5000;     // an engineer makes ammunition for the guild's gun hunters below this (0: never)
        uint32 professionsSkillCraftStock = 200; // no consumable is made for skill while the storage holds this many (0: no cap)
        uint32 professionsConsumableStock = 20; // crafters keep this many of each endgame consumable for dungeon runs (0: none)
        bool professionsEnchantScrolls = true;  // enchanters write enchantments for the guild's gear on vellums, bots put them on
        uint32 professionsEnchantMinItemLevel = 60; // ... on blue or better pieces from this item level
        uint32 professionsProcessAbove = 1000;  // raw materials above this many in the storage are processed (ore into bars; 0: never)
        uint32 professionsProcessStock = 200;   // ... up to this stock of a processed material (bars out of ore: no cap; 0: no cap)
        bool professionsBuyRecipes = true;      // bots buy recipes of their professions that they can learn now at vendors in reach
        uint32 professionsRecipeMaxPrice = 10000; // ... when one costs no more than this (copper)
        float professionsRecipeYards = 300.0f;  // ... and walk to a vendor this near for one (0: only at vendors they visit anyway)
        bool professionsDisenchantSurplus = true; // an enchanter disenchants surplus crafted gear of the guild storage
        bool professionsCraftAtHub = true;      // at a hub after a hand-in the bot crafts what gives it skill
        bool professionsCraftGear = false;      // ... and a piece of gear a guild character would wear (package K5;
                                                // off until the owner switches it on: it uses the storage's materials)
        bool professionsCraftOrders = true;         // bots make what the guild ordered on their town trips (CraftOrders.h)
        bool professionsDisenchantForOrders = true;  // ... and an enchanter disenchants green and blue storage items for what is missing
        uint32 professionsCraftBatch = 5;       // at most this many casts of one recipe per visit
        uint32 vendorTrips = 0;                 // 0 hub: at a hub after a hand-in, or when urgent; 1 full: only when
                                                // urgent (full bags, gear breaking, no ammunition); 2 any: whenever due
        uint32 botGuildChatMinSeconds = 30;     // pause after a guild chat line of an AI-played character (0 = no limit)
        uint32 botGuildChatMaxSeconds = 60;     // the pause is random between min and max
        // Rest between fights and provisions (docs/orders.md "Rest and provisions")
        bool restEnable = true;                 // AI-played guild characters rest before they go on with low health or mana
        uint8 restBelowHealth = 65;             // rest starts below this health percentage ...
        uint8 restBelowMana = 40;               // ... or, for mana users, below this mana percentage
        uint8 restUntilHealth = 85;             // rest ends at this health percentage ...
        uint8 restUntilMana = 75;               // ... and this mana percentage
        float restSaferPlaceYards = 40.0f;      // before a rest a bot among hostile creatures walks up to this far to a quieter place (0 = rests where it stands)
        uint32 selfHealBelow = 45;              // a character with a healing spell heals itself in a fight below this health (percent; 0 = never)
        uint32 potionBelowHealth = 35;          // a healing potion in a fight below this health (percent; 0 = never)
        uint32 potionBelowMana = 15;            // a mana potion in a fight below this mana (percent; 0 = never)
        uint32 consecrationMinFoes = 2;         // a paladin's Consecration only with this many foes in a fight within 8 yards
        bool fightInterrupt = true;             // a bot interrupts a foe's spellcast with its class's interrupt
        bool fightTemplatesByDefault = true;    // a bot without a fight profile of its own plays its spec's template
        uint32 provisionsConjureBelow = 4;      // a mage conjures water (and food) when it carries fewer pieces, up to twice that (0 = only in a rest without any)
        uint32 restAirAfterSeconds = 90;        // a bot with its head under water this long goes ashore to breathe (0 = never)
        uint32 restBeforeWalk = 0;              // before a walk past hostile creatures a bot rests up to this percentage of health and mana (0 = only the thresholds above)
        bool pullCareful = true;                // a creature that stands with others is pulled out to the bot: with a ranged spell or weapon, or by walking into its sight and back
        uint32 restMaxSeconds = 60;             // a rest without food and drink ends after this time once above the start values
        uint8 provisionsMinLevel = 8;           // no food and drink are bought below this level
        uint32 provisionsStack = 20;            // pieces of food a bot stocks up to
        bool provisionsTripWhenOut = true;      // a mana user without any drink walks to a vendor that sells some (like a hunter without ammunition)
        float provisionsTripWhenOutYards = 600.0f; // ... when one stands within this distance (and the guild storage has no drink for it)
        uint32 ammoHunterStock = 2000;          // rounds a hunter stocks up to and carries at most (from level 20)
        uint32 ammoHunterStockBelow20 = 1000;   // ... below level 20
        uint32 ammoHunterStockBelow5 = 200;     // ... below level 5
        uint32 ammoRestockBelow = 100;          // a hunter with fewer rounds walks to a vendor that sells some, from wherever it is
        uint32 ammoOthersStacks = 1;            // stacks of ammunition any other class with a bow, crossbow or gun carries at most
        float ammoTopUpYards = 200.0f;          // a vendor this near tops a hunter's ammunition up when it has come to a hub
        uint32 provisionsDrinkStack = 40;       // ... and of drink (a caster drinks after nearly every fight)
        float provisionsHubYards = 150.0f;      // a vendor this near sells it provisions when it is below half of that (a hub it has come to)
        bool provisionsTripWhenLow = false;     // a walk to a vendor from the field when it is nearly out (else only at a hub)
        uint32 provisionsMinMoney = 100;        // copper a bot needs before it buys food and drink
        uint8 provisionsBudgetPercent = 50;     // share of its money one purchase of food and drink may cost
        uint8 provisionsRestockBelow = 4;       // a trip to a food vendor is planned below this many pieces of food or drink
        bool activityLogFights = true;          // one activity log line per fight and per rest
        // Risk (docs/orders.md "Risk"): safer quest spots and grind spots, deaths remembered by place and foe
        bool specsEnable = true;                // talent points go along the bot's template (SpecService)
        bool escapeEnable = true;               // leave a fight the bot would lose (FightEscape.h)
        float escapeMargin = 1.0f;              // leave when the rest of the fight costs more than this times its health
        uint32 escapeMaxFoes = 3;               // this many foes at once (not three levels below the bot) and it leaves, whatever the estimate (0 = estimate only)
        bool escapeStepAside = true;            // keep away from a roaming creature far above the bot's level
        uint32 escapeMaxSeconds = 35;           // a run that has not shaken the foes off by then ends: the bot fights on
        bool riskEnable = true;
        uint8 riskMaxFoeLevelAbove = 3;         // quest targets more levels above the bot wait (one more with a pet)
        float riskMaxFightCost = 0.8f;          // a spot waits while the fight it means would cost the bot more than this share of what it has
        uint8 riskCautiousDeaths = 4;           // this many deaths within the window make a bot play it safe (0 = never)
        uint32 riskCautiousWindowMinutes = 20;
        uint32 riskCautiousMinutes = 15;        // ... for this long (or until its next level): no objectives, a safe grind spot
        uint8 riskRescueDeaths = 3;             // this many deaths within the window far above its level: taken home (0 = never)
        uint32 riskRescueWindowMinutes = 10;
        bool classAbilitiesEnable = true;       // guild bots get what their class quests teach (ga_class_ability)
        bool classAbilitiesHunterPet = true;    // ... and a hunter its first pet at the level of its taming quest
        bool classAbilitiesPetTalents = true;   // a hunter's pet spends its talent points along its tree's build
        bool poisonsEnable = true;              // rogues from level 20 keep a stock of poisons (RoguePoisons.h)
        uint32 poisonsStock = 10;               // ... this many doses per weapon hand
        uint32 poisonsPair = 0;                 // ... 0 Instant main / Deadly off, 1 Instant on both, 2 Deadly on both
        bool classAbilitiesRuneforging = true;  // a death knight puts its best known rune on a weapon without an enchantment (DeathKnights.h)
        uint32 deathKnightsSkipStart = 1;       // a recruited death knight: 0 in Acherus, 1 in the capital with the campaign's rewards, 2 ... with the starting gear only
        // Squads (Squads.h, docs/orders.md "Squads"): two or three bots level together
        // Dungeon runs (OrderDungeons.cpp).
        bool dungeonsOpenDoors = true;          // the group opens a closed door it comes to inside a dungeon
        uint32 dungeonsMaxRuns = 10;            // the most runs one dungeon order may ask for
        bool dungeonsFieldRepair = true;        // between two runs of an order the group's gear is repaired where it stands, at the vendors' price
        bool dungeonsTeleport = true;           // a run's group is teleported into the dungeon and out to its hearthstone places (0: it walks to the entrance)
        uint32 dungeonsLingerSeconds = 30;      // after the last boss the group stays this long before it leaves
        bool dungeonsUnlockInOrder = true;      // a guild plays the dungeons in their order: a finished one opens the next
        // How a dungeon group fights (the owner, 2026-11-13; docs/orders.md "How a dungeon group fights").
        uint32 dungeonsTankLeadSeconds = 4;     // the others hold back this long at most while the tank takes the creatures (0: not at all)
        uint32 dungeonsEmergencyHealth = 50;    // ... the healer heals meanwhile when somebody is below this health (percent)
        bool dungeonsCarefulPulls = true;       // the tank brings a creature to the group: after a wipe of the run, or for a big group at its level
        uint32 dungeonsCarefulPullPack = 3;     // ... "big": this many creatures that would come together
        bool dungeonsFocusFleeing = true;       // everybody in reach turns to a creature that runs away
        uint32 dungeonsMinReadiness = 0;        // percent a bot needs for a dungeon run (0: readiness is shown, not asked for)
        bool dungeonsHopWhenNoWay = true;       // a group that finds no way on foot to a boss is put near it (ways players swim or jump)
        bool dungeonsSuppliesByDefault = true;  // a run that was not prepared in the run window uses the storage's consumables too
        bool dungeonsTakeOwnerAlong = true;     // a character its owner plays by hand in a dungeon group is taken along by the run's teleports
        // How a dungeon group goes its way (package PW, the owner's E2-E4; bot settings of the category Dungeon runs).
        uint32 dungeonsEngageMargin = 8;        // yd beyond the lowest member's aggro range: what the tank engages on its way
        uint32 dungeonsBigPullAt = 4;           // a pack of this many creatures (2 more five levels down) is pulled to a camp
        uint32 dungeonsGateHealth = 90;         // before a boss or a big pull: health of everybody
        uint32 dungeonsGateHealerMana = 80;     // ... the healer's mana
        uint32 dungeonsGateDamageMana = 60;     // ... a damage dealer's mana
        // Dungeon roles (DungeonRoles.h).
        bool rolesEnable = true;                // the dungeon role of a bot: second talent spec, dungeon set, role store
        bool statWeightsEnable = true;          // items are judged by the wowbot335 stat weights (ga_stat_weight, patch 0002)
        bool rolesFromStorage = true;           // a bot takes pieces for its dungeon set out of the guild storage
        uint32 rolesWearForLevelling = 2;       // 0 never, 1 armour and jewellery, 2 weapons too: role store pieces worn while levelling
        uint32 rolesRespecCost = 0;             // copper a bot pays for the change to its role's talents (0 = free)
        bool squadsEnable = true;
        uint32 squadsMaxSize = 5;               // characters per squad, the leader included (five: a dungeon group)
        uint32 squadsMaxLevelGap = 3;           // a member farther from its leader's level plays its own order
        bool squadsAuto = false;                // bots of the same zone within two levels form squads by themselves
        float squadsWaitYards = 80.0f;          // the leader waits for a member within this distance that rests, is dead or fights
        uint32 squadsMaxWaitSeconds = 120;      // ... for at most this long at a time
        float squadsCatchUpYards = 250.0f;      // a member this far behind (or on another map) catches up with its leader
        // Quest partners (QuestPartners.h): bots with the same hard quest team up for it
        bool squadsCatchUpFallback = true;     // bot setting: a member that finds no way to its far leader is put beside it
        bool squadsQuestPartners = true;        // bot setting: hard quests wait for a guild partner, and bots pair up for them
        uint32 squadsPartnerDeaths = 2;         // guild deaths on a quest within PartnerHours that make it hard (0 = never)
        uint32 squadsPartnerHours = 6;
        bool squadsPartnerTooHard = true;       // a quest whose every spot near was too much for a guild character is hard
        float squadsPartnerYards = 1500.0f;     // partners stand this near each other on one map when they pair up
        uint32 squadsPartnerMeetMinutes = 8;    // partners that have not met within this split
        uint32 squadsPartnerMaxMinutes = 60;    // ... and after this long in any case
        uint32 squadsPartnerRetryMinutes = 30;  // a quest whose partners split without finishing it waits this long
        bool debug = false;
    };

    void LoadSettings();
    Settings const& GetSettings();
    // Renown a guild gains: the points times GuildAdventure.Renown.Rate, rounded (a GM's .ga renown add is not scaled).
    int32 ScaleRenown(int64 points);
}

#endif
