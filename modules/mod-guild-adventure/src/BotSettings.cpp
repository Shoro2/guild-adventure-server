/*
 * Behaviour settings of the guild leader. See BotSettings.h and docs/bot-settings.md.
 */

#include "BotSettings.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GuildAdventureMgr.h"
#include "GuildAdventureSettings.h"
#include "Log.h"
#include "Player.h"
#include "StringFormat.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <type_traits>

using namespace GuildAdventure;

namespace
{
    constexpr uint32 IndexIntervalMs = 5 * IN_MILLISECONDS;
    constexpr uint32 SchemaIntervalMs = 10 * IN_MILLISECONDS;

    int32 Bool(bool value) { return value ? 1 : 0; }
    int32 Scaled(float value, int32 scale) { return int32(std::lround(value * float(scale))); }

    // The conf field a setting reads its default from and writes the guild's value into (BotSettings::For).
#define GA_BOOL(field) \
    [](Settings const& s) { return Bool(s.field); }, \
    [](Settings& s, int32 v) { s.field = v != 0; }
#define GA_INT(field) \
    [](Settings const& s) { return int32(s.field); }, \
    [](Settings& s, int32 v) { s.field = static_cast<std::remove_reference_t<decltype(s.field)>>(std::max(v, 0)); }
#define GA_FLOAT(field, scale) \
    [](Settings const& s) { return Scaled(s.field, scale); }, \
    [](Settings& s, int32 v) { s.field = float(v) / float(scale); }

    // The registry: every setting the leader can change. Order = the order in the addon within a category.
    std::vector<SettingDef> const sDefs =
    {
        // --- Combat and rest -------------------------------------------------------------------------------------
        { Setting::RestEnable, "Rest.Enable", "combat", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(restEnable),
            "Rest between fights",
            "Out of combat a bot with low health or mana stops, eats and drinks what it carries (or sits) before it goes on." },
        { Setting::RestBelowHealth, "Rest.BelowHealth", "combat", SettingType::Int, 0, 95, 5, 1, "%", true, false, "",
            GA_INT(restBelowHealth),
            "Rest below health",
            "A rest starts when health is below this. 0 = never because of health." },
        { Setting::RestBelowMana, "Rest.BelowMana", "combat", SettingType::Int, 0, 95, 5, 1, "%", true, false, "",
            GA_INT(restBelowMana),
            "Rest below mana",
            "For mana users: a rest starts when mana is below this. 0 = never because of mana." },
        { Setting::RestUntilHealth, "Rest.UntilHealth", "combat", SettingType::Int, 10, 100, 5, 1, "%", true, false, "",
            GA_INT(restUntilHealth),
            "Rest until health",
            "A rest ends at this health (never below the start value)." },
        { Setting::RestUntilMana, "Rest.UntilMana", "combat", SettingType::Int, 10, 100, 5, 1, "%", true, false, "",
            GA_INT(restUntilMana),
            "Rest until mana",
            "A rest ends at this mana (never below the start value)." },
        { Setting::RestMaxSeconds, "Rest.MaxSeconds", "combat", SettingType::Int, 5, 600, 5, 1, "s", true, true, "",
            GA_INT(restMaxSeconds),
            "Longest rest without food",
            "A rest without food and drink ends after this time once health and mana are above the start values." },
        { Setting::RestSaferPlaceYards, "Rest.SaferPlaceYards", "combat", SettingType::Int, 0, 100, 5, 1, "yd", true, true, "",
            GA_FLOAT(restSaferPlaceYards, 1),
            "Walk to a quieter place to rest",
            "Before it sits down among hostile creatures a bot walks up to this far to a quieter spot. 0 = rests where it stands." },
        { Setting::RestAirAfterSeconds, "Rest.AirAfterSeconds", "combat", SettingType::Int, 0, 170, 5, 1, "s", true, true, "",
            GA_INT(restAirAfterSeconds),
            "Go ashore to breathe after",
            "A bot with its head under water this long goes ashore to breathe. 0 = never." },
        { Setting::RestBeforeWalk, "Rest.BeforeWalk", "combat", SettingType::Int, 0, 100, 5, 1, "%", true, false, "",
            GA_INT(restBeforeWalk),
            "Rest before a walk through hostile ground, up to",
            "Before a walk of more than 60 yards past hostile creatures of about its level a bot eats and drinks up to this much health and mana, although it is above the thresholds it rests at. 0 = off: a bot rests only below those thresholds (and before a fight it could not afford otherwise)." },
        { Setting::FightPotionBelowHealth, "Fight.PotionBelowHealth", "combat", SettingType::Int, 0, 90, 5, 1, "%", true, false, "",
            GA_INT(potionBelowHealth),
            "Healing potion in a fight below health",
            "In a fight below this health a bot drinks the best healing potion it carries or the guild storage has (one piece, taken out at that moment). 0 = never." },
        { Setting::FightPotionBelowMana, "Fight.PotionBelowMana", "combat", SettingType::Int, 0, 90, 5, 1, "%", true, false, "",
            GA_INT(potionBelowMana),
            "Mana potion in a fight below mana",
            "For mana users: in a fight below this mana, while its target still has more than a quarter of its health, a bot drinks the best mana potion it carries or the guild storage has. 0 = never." },
        { Setting::FightConsecrationMinFoes, "Fight.ConsecrationMinFoes", "combat", SettingType::Int, 1, 10, 1, 1, "", true, false, "",
            GA_INT(consecrationMinFoes),
            "Paladin: Consecration from foes",
            "A paladin casts Consecration only when at least this many foes in a fight stand within its eight yards, in the open world and in dungeons, whatever its talents. 1 = whenever its rotation would." },
        { Setting::FightInterrupt, "Fight.Interrupt", "combat", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(fightInterrupt),
            "Interrupt enemy spellcasts",
            "A bot interrupts a spell a foe of its fight casts (its target or one that attacks it) whenever its class's interrupt is ready and the foe is in its reach: Kick, Counterspell, Wind Shear, Mind Freeze, Silence, Silencing Shot, a bear's Bash, Shield Bash with a shield, Pummel (a warrior changes to Berserker Stance for it when the rage it keeps through the change pays for it). While levelling and in dungeons." },
        { Setting::FightTemplatesByDefault, "Fight.TemplatesByDefault", "combat", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(fightTemplatesByDefault),
            "Fight by the template of its spec",
            "A bot whose fight profile slot is empty plays the fight profile template of its class, talent tree and role (the wowbot335 templates and the module's own for tanks and healers) instead of mod-playerbots' rotation. In a dungeon or raid group a tank plays the tank template, a healer the healer template. Off: mod-playerbots' rotation, as before." },
        { Setting::PullCareful, "Pull.Careful", "combat", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(pullCareful),
            "Pull creatures out of their group",
            "A bot turns to a creature that stands alone when there is one. One that stands with others is pulled out: with a ranged spell or weapon from a distance, or by walking into its sight and back a few steps, so the fight happens away from the group. Off: the bot walks up to its target." },
        { Setting::EscapeEnable, "Escape.Enable", "combat", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(escapeEnable),
            "Leave fights it would lose",
            "Against several foes, an elite or a foe far above it a bot estimates the rest of the fight from its own fights and runs when it would lose." },
        { Setting::EscapeMargin, "Escape.Margin", "combat", SettingType::Decimal, 25, 400, 5, 100, "", true, false, "",
            GA_FLOAT(escapeMargin, 100),
            "Courage",
            "1.00 = a bot runs when the rest of the fight would cost all its health; 1.50 = only at one and a half times that; 0.75 = earlier." },
        { Setting::EscapeMaxFoes, "Escape.MaxFoes", "combat", SettingType::Int, 0, 20, 1, 1, "foes", true, false, "",
            GA_INT(escapeMaxFoes),
            "Run at once from this many foes",
            "This many foes at once (not far below the bot) and it leaves, whatever the estimate says. 0 = the estimate alone decides." },
        { Setting::EscapeStepAside, "Escape.StepAside", "combat", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(escapeStepAside),
            "Keep away from strong roaming creatures",
            "A bot steps aside for a rare, elite or patrol far above its level that comes near." },
        { Setting::EscapeMaxSeconds, "Escape.MaxSeconds", "combat", SettingType::Int, 10, 120, 5, 1, "s", true, true, "",
            GA_INT(escapeMaxSeconds),
            "Longest run",
            "A run that has not shaken the foes off by then ends: the bot fights on." },

        // --- Danger and deaths -----------------------------------------------------------------------------------
        { Setting::RiskEnable, "Risk.Enable", "danger", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(riskEnable),
            "Careful quest and grind spots",
            "Bots work an objective at its safest spawn, leave packs and targets above them for later and remember where they died." },
        { Setting::RiskMaxFoeLevelAbove, "Risk.MaxFoeLevelAbove", "danger", SettingType::Int, 0, 10, 1, 1, "levels", true, false, "",
            GA_INT(riskMaxFoeLevelAbove),
            "Quest targets above its level",
            "Quest targets more levels above the bot wait until it is stronger (one more with a pet). 0 = only targets up to its own level." },
        { Setting::RiskMaxFightCostPercent, "Risk.MaxFightCostPercent", "danger", SettingType::Int, 10, 1000, 5, 100, "%", true, false, "",
            GA_FLOAT(riskMaxFightCost, 100),
            "Most a fight may cost",
            "A quest or grind target waits while the fight it means would cost the bot more than this share of its health (from its own fights; squads count together). 100% = all of it." },
        { Setting::RiskCautiousDeaths, "Risk.CautiousDeaths", "danger", SettingType::Int, 0, 20, 1, 1, "deaths", true, false, "",
            GA_INT(riskCautiousDeaths),
            "Play it safe after deaths",
            "This many deaths in a short time and a bot only turns in, picks up and grinds at a safe spot for a while. 0 = never." },
        { Setting::RiskCautiousWindowMinutes, "Risk.CautiousWindowMinutes", "danger", SettingType::Int, 1, 30, 1, 1, "min", true, true, "",
            GA_INT(riskCautiousWindowMinutes),
            "... deaths counted over",
            "The time in which the deaths for \"play it safe\" count." },
        { Setting::RiskCautiousMinutes, "Risk.CautiousMinutes", "danger", SettingType::Int, 1, 240, 5, 1, "min", true, true, "",
            GA_INT(riskCautiousMinutes),
            "... play it safe for",
            "How long a bot plays it safe (or until its next level)." },
        { Setting::RiskRescueDeaths, "Risk.RescueDeaths", "danger", SettingType::Int, 0, 20, 1, 1, "deaths", true, false, "",
            GA_INT(riskRescueDeaths),
            "Take home from a death loop after",
            "This many deaths in a short time among foes far above it and the bot is taken to its bind point at the spirit healer's price. 0 = never." },
        { Setting::RiskRescueWindowMinutes, "Risk.RescueWindowMinutes", "danger", SettingType::Int, 1, 30, 1, 1, "min", true, true, "",
            GA_INT(riskRescueWindowMinutes),
            "... deaths counted over",
            "The time in which the deaths for the rescue count." },

        // --- Questing and travel ---------------------------------------------------------------------------------
        { Setting::QuestsEscorts, "Quests.Escorts", "quests", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(questsEscorts),
            "Escort quests",
            "Bots take escort quests and follow the NPC until the quest is done or fails." },
        { Setting::QuestsEvents, "Quests.Events", "quests", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(questsEvents),
            "Quests with events",
            "Bots play quests that complete through a talk, waves at the quest giver, an NPC to lead or a place to enter." },
        { Setting::QuestsItemsFromGuild, "Quests.ItemsFromGuild", "quests", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(questsItemsFromGuild),
            "Quest items from the guild",
            "A quest item a bot lacks comes out of the guild storage, from another guild character or from a guild recipe." },
        { Setting::QuestsAbandonGrey, "Quests.AbandonGrey", "quests", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(questsAbandonGrey),
            "Abandon grey quests",
            "A bot abandons a quest as soon as it is grey for it (far below its level). A finished grey quest is still handed in when its ender is close by; otherwise it is abandoned too." },
        { Setting::SquadsQuestPartners, "Squads.QuestPartners", "quests", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(squadsQuestPartners),
            "Quest partners for hard quests",
            "Elite and group quests, and quests guild members died on, wait in the log until another guild bot has them too; then the two team up, finish the quest together and part again. Off: the bot tries them alone." },
        { Setting::SquadsCatchUpFallback, "Squads.CatchUpFallback", "quests", SettingType::Bool, 0, 1, 1, 1, "", false, true, "",
            GA_BOOL(squadsCatchUpFallback),
            "Squad: put a member beside its leader when it finds no way",
            "A member far behind its leader (the leader flew, took a ship, a portal) travels after it as a player would: on foot, by flight, ship, zeppelin or portal. When it finds no way at all, it is put beside its leader (a line in the activity log). Off: it waits and tries the way again." },
        { Setting::AutoAdvanceDefault, "AutoAdvanceDefault", "quests", SettingType::Bool, 0, 1, 1, 1, "", false, false, "",
            GA_BOOL(autoAdvanceDefault),
            "New recruits move on by themselves",
            "A new recruit goes on to the next zone when it has out-levelled its zone (each bot can change it in its orders)." },
        { Setting::RoadsEnable, "Roads.Enable", "quests", SettingType::Bool, 0, 1, 1, 1, "", true, true, "",
            GA_BOOL(roadsEnable),
            "Long walks follow roads",
            "Long walks go along the travel routes, around camps and zones far above the bot, instead of straight across." },
        { Setting::RoadsMinDistance, "Roads.MinDistance", "quests", SettingType::Int, 100, 5000, 50, 1, "yd", true, true, "",
            GA_FLOAT(roadsMinDistance, 1),
            "... from this distance",
            "Walks longer than this follow a route; shorter ones go straight." },

        { Setting::WalksMaxLevelsAbove, "Walks.MaxLevelsAbove", "quests", SettingType::Int, 0, 10, 1, 1, "levels", true, false, "",
            GA_INT(walksMaxLevelsAbove),
            "Walk past creatures above its level",
            "The way to a task may pass hostile creatures this many levels above the bot; a target behind stronger ones waits." },
        { Setting::WalksPostponeMinutes, "Walks.PostponeMinutes", "quests", SettingType::Int, 1, 240, 5, 1, "min", true, true, "",
            GA_INT(walksPostponeMinutes),
            "... such a target waits for",
            "How long a target behind stronger creatures is left alone." },
        { Setting::WalksPlaceCheck, "Walks.PlaceCheck", "quests", SettingType::Bool, 0, 1, 1, 1, "", true, true, "",
            GA_BOOL(walksPlaceCheck),
            "Avoid quest givers among strong creatures",
            "No hand-in or pickup where every creature around the quest giver is above the bot." },
        { Setting::TravelHearthstone, "Travel.Hearthstone", "quests", SettingType::Enum, 0, 2, 1, 1, "", true, false,
            "emergency=Only in an emergency;far=To another continent or a capital;any=Whenever it shortens a travel",
            GA_INT(travelHearthstone),
            "Hearthstone on travels",
            "To another continent or a capital: a bot uses its hearthstone on a travel only when its inn is on another continent or in a capital of its side; else it walks or flies. Only in an emergency: never on a travel, only when it has no other way out. Whenever it shortens a travel: also to an inn on its own continent." },

        // --- Grinding -------------------------------------------------------------------------------------------
        { Setting::GrindEnable, "Grind.Enable", "grind", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(grindEnable),
            "Grind when no quest is left",
            "A bot without quest work in its zone kills creatures nearby for experience." },
        { Setting::GrindLevelsBelow, "Grind.LevelsBelow", "grind", SettingType::Int, 0, 10, 1, 1, "levels", true, false, "",
            GA_INT(grindLevelsBelow),
            "Targets down to this far below it",
            "A grinding bot fights creatures down to this many levels below its own." },
        { Setting::GrindLevelsAbove, "Grind.LevelsAbove", "grind", SettingType::Int, 0, 5, 1, 1, "levels", true, false, "",
            GA_INT(grindLevelsAbove),
            "... and up to this far above it",
            "A grinding bot fights creatures up to this many levels above its own." },
        { Setting::GrindMaxDistance, "Grind.MaxDistance", "grind", SettingType::Int, 100, 3000, 50, 1, "yd", true, false, "",
            GA_FLOAT(grindMaxDistance, 1),
            "Grind only nearby",
            "Grind spots are looked for at most this far from the bot." },

        // --- Farm orders ----------------------------------------------------------------------------------------
        { Setting::FarmLevelsBelow, "Farm.LevelsBelow", "farm", SettingType::Int, 0, 20, 1, 1, "levels", true, false, "",
            GA_INT(farmLevelsBelow),
            "Targets at least this far below it",
            "A bot with a farm order fights only creatures at least this many levels below its own." },
        { Setting::FarmGroundRadius, "Farm.GroundRadius", "farm", SettingType::Int, 30, 200, 10, 1, "yd", true, false, "",
            GA_FLOAT(farmGroundRadius, 1),
            "Size of a hunting ground",
            "The creatures within this many yards of one spot are one hunting ground: what the bot covers on foot." },
        { Setting::FarmMinSpawns, "Farm.MinSpawns", "farm", SettingType::Int, 1, 20, 1, 1, "", true, false, "",
            GA_INT(farmMinSpawns),
            "Creatures a hunting ground needs",
            "A hunting ground needs this many creatures that drop the item. Smaller groups are only taken when no place has that many." },
        { Setting::FarmMaxDistance, "Farm.MaxDistance", "farm", SettingType::Int, 0, 20000, 500, 1, "yd", true, true, "",
            GA_FLOAT(farmMaxDistance, 1),
            "Farm only this near",
            "Hunting grounds are looked for at most this far from the bot. 0: anywhere on its land mass." },

        // --- Dungeon runs (package PW: the value of the bot that leads the run counts) ------------------------------
        { Setting::DungeonsEngageMargin, "Dungeons.EngageMargin", "dungeons", SettingType::Int, 0, 30, 1, 1, "yd", false, false, "",
            GA_INT(dungeonsEngageMargin),
            "The tank engages creatures within",
            "On its way to a boss the tank takes on every creature whose reach for the group's lowest member plus this many yards touches the way, before anybody walks past it (at least 15 yd in all)." },
        { Setting::DungeonsBigPullAt, "Dungeons.BigPullAt", "dungeons", SettingType::Int, 2, 10, 1, 1, "creatures", false, false, "",
            GA_INT(dungeonsBigPullAt),
            "A big pull from",
            "A pack of this many creatures (two more when the group is five levels above them) is pulled to the group waiting a little back on its way, after the whole group is ready at the tank; a smaller one the tank walks into. Casters, boss rooms and a run after a wipe always count as big." },
        { Setting::DungeonsGateHealth, "Dungeons.GateHealth", "dungeons", SettingType::Int, 0, 100, 5, 1, "%", false, false, "",
            GA_INT(dungeonsGateHealth),
            "Before a boss: everybody's health",
            "Before a boss or a big pull the group gathers at the tank and waits until everybody has this much health." },
        { Setting::DungeonsGateHealerMana, "Dungeons.GateHealerMana", "dungeons", SettingType::Int, 0, 100, 5, 1, "%", false, false, "",
            GA_INT(dungeonsGateHealerMana),
            "Before a boss: the healer's mana",
            "... and the healer this much mana." },
        { Setting::DungeonsGateDamageMana, "Dungeons.GateDamageMana", "dungeons", SettingType::Int, 0, 100, 5, 1, "%", false, false, "",
            GA_INT(dungeonsGateDamageMana),
            "Before a boss: a damage dealer's mana",
            "... and a damage dealer that uses mana this much (one with nothing to drink anywhere does not hold the group)." },

        // --- Vendors and provisions ------------------------------------------------------------------------------
        { Setting::VendorTrips, "Vendor.Trips", "upkeep", SettingType::Enum, 0, 2, 1, 1, "", true, false,
            "hub=At a hub after a hand-in;full=Only when urgent;any=Whenever due",
            GA_INT(vendorTrips),
            "Vendor trips",
            "At a hub after a hand-in: a bot sells, repairs and buys when it turns in a quest near a vendor, and walks to one only when it is urgent (full bags, gear about to break, a hunter out of ammunition). Only when urgent: never at a hub. Whenever due: as soon as something is due." },
        { Setting::ProvisionsMinLevel, "Provisions.MinLevel", "upkeep", SettingType::Int, 0, 80, 1, 1, "", true, false, "",
            GA_INT(provisionsMinLevel),
            "Buy food and drink from level",
            "Below this level a bot buys no food and drink (hunters still buy their ammunition)." },
        { Setting::ProvisionsStack, "Provisions.Stack", "upkeep", SettingType::Int, 2, 200, 2, 1, "pieces", true, false, "",
            GA_INT(provisionsStack),
            "Food and drink it carries",
            "A bot stocks up to this many pieces of food, and of drink, and buys again below half of it." },        { Setting::ProvisionsMinMoney, "Provisions.MinMoney", "upkeep", SettingType::Money, 0, 1000000, 100, 1, "", true, false, "",
            GA_INT(provisionsMinMoney),
            "Buy food and drink from",
            "A bot buys food and drink only when it has at least this much money." },
        { Setting::ProvisionsBudgetPercent, "Provisions.BudgetPercent", "upkeep", SettingType::Int, 1, 100, 5, 1, "%", true, false, "",
            GA_INT(provisionsBudgetPercent),
            "Spend on food and drink at most",
            "The share of its money one purchase of food and drink may cost." },
        { Setting::ProvisionsRestockBelow, "Provisions.RestockBelow", "upkeep", SettingType::Int, 1, 20, 1, 1, "pieces", true, false, "",
            GA_INT(provisionsRestockBelow),
            "Restock trip below",
            "A trip to a food vendor is planned when a bot carries fewer pieces of food or drink than this." },
        { Setting::ProvisionsHubYards, "Provisions.HubYards", "upkeep", SettingType::Int, 0, 3000, 10, 1, "yd", true, true, "",
            GA_FLOAT(provisionsHubYards, 1),
            "A hub's vendor within",
            "A vendor this near sells a bot its provisions when it has come to a hub." },
        { Setting::ProvisionsTripWhenLow, "Provisions.TripWhenLow", "upkeep", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(provisionsTripWhenLow),
            "Walk to a vendor when nearly out of food",
            "Off: provisions are only bought at a hub; without them a rest just takes longer." },
        { Setting::ProvisionsTripWhenOut, "Provisions.TripWhenOut", "upkeep", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(provisionsTripWhenOut),
            "Go to a vendor when out of drink",
            "A bot that uses mana and has no drink left goes to a vendor that sells some at once, unless the guild storage has drink "
            "for it (it drinks from there until it comes to a town, where it buys). Off: drink is only bought at a hub." },
        { Setting::ProvisionsTripWhenOutYards, "Provisions.TripWhenOutYards", "upkeep", SettingType::Int, 0, 3000, 50, 1, "yd", true, false, "",
            GA_FLOAT(provisionsTripWhenOutYards, 1),
            "... to a vendor within",
            "How far a bot out of drink goes for it at most. 0: as far as it takes. A vendor further away waits until the bot comes "
            "to a hub." },
        { Setting::AmmoHunterStock, "Ammo.HunterStock", "upkeep", SettingType::Int, 200, 20000, 200, 1, "rounds", true, false, "",
            GA_INT(ammoHunterStock),
            "A hunter's ammunition",
            "A hunter stocks up to this many arrows or bullets at a vendor and never carries more: what is over it is sold at vendor price, the worst kind first. Ammunition never goes into the guild storage from a character's bags; only what a guild character made does." },
        { Setting::AmmoHunterStockBelow20, "Ammo.HunterStockBelow20", "upkeep", SettingType::Int, 200, 20000, 200, 1, "rounds", true, false, "",
            GA_INT(ammoHunterStockBelow20),
            "... below level 20",
            "The stock of a hunter below level 20." },
        { Setting::AmmoHunterStockBelow5, "Ammo.HunterStockBelow5", "upkeep", SettingType::Int, 200, 20000, 200, 1, "rounds", true, false, "",
            GA_INT(ammoHunterStockBelow5),
            "... below level 5",
            "The stock of a hunter below level 5." },
        { Setting::AmmoRestockBelow, "Ammo.RestockBelow", "upkeep", SettingType::Int, 0, 5000, 50, 1, "rounds", true, false, "",
            GA_INT(ammoRestockBelow),
            "Walk to a vendor for ammunition below",
            "A hunter with fewer rounds than this walks to the nearest vendor that sells its ammunition, from wherever it is. Above it a hunter tops up at a vendor it visits anyway, or one near a hub after a hand-in. No trip and no purchase while the guild storage holds ammunition for it." },
        { Setting::AmmoOthersStacks, "Ammo.OthersStacks", "upkeep", SettingType::Int, 1, 10, 1, 1, "stacks", true, false, "",
            GA_INT(ammoOthersStacks),
            "Ammunition of other classes",
            "A warrior, rogue or any other class with a bow, crossbow or gun carries at most this many stacks." },
        { Setting::AmmoTopUpYards, "Ammo.TopUpYards", "upkeep", SettingType::Int, 0, 3000, 10, 1, "yd", true, true, "",
            GA_FLOAT(ammoTopUpYards, 1),
            "A hub's ammunition vendor within",
            "A hunter below its stock visits a vendor that sells its ammunition this near when it has come to a hub or is on a vendor trip." },
        { Setting::StorageSupplyProvisions, "Storage.Supply.Provisions", "upkeep", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(storageSupplyProvisions),
            "Food and drink from the guild storage",
            "A bot that eats or drinks in a rest and carries nothing for it takes one piece of the best food or drink for its level out of the guild storage, at that moment. While the storage has some it buys none. A mage takes none of what it conjures." },
        { Setting::StorageSupplyPotions, "Storage.Supply.Potions", "upkeep", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(storageSupplyPotions),
            "Potions from the guild storage",
            "The healing or mana potion a bot drinks in a fight may come out of the guild storage: one piece, the best it can use, at that moment. Off: only potions it carries." },
        { Setting::StorageSupplyBandages, "Storage.Supply.Bandages", "upkeep", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(storageSupplyBandages),
            "Bandages from the guild storage",
            "A bot with First Aid bandages itself at the start of a rest for its health, with the best bandage its skill allows out of the guild storage (one piece). Off: only bandages it carries." },
        { Setting::StorageSupplyScrolls, "Storage.Supply.Scrolls", "upkeep", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(storageSupplyScrolls),
            "Scrolls from the guild storage",
            "While a bot works on a task with fights it uses the best scroll of its main stat (strength, agility or intellect) out of the guild storage when it has no such buff and nothing on it that does not stack with it, at most one per half hour. Not in a dungeon run, which uses what you ticked for it." },
        { Setting::StorageSupplyElixirs, "Storage.Supply.Elixirs", "upkeep", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(storageSupplyElixirs),
            "Elixirs from the guild storage",
            "Off by default. On: while a bot works on a task with fights it uses the best battle elixir and guardian elixir out of the guild storage that help the way it fights, when it has none of that kind on and nothing on it that does not stack with it. Not in a dungeon run." },
        { Setting::ProvisionsConjureBelow, "Provisions.ConjureBelow", "upkeep", SettingType::Int, 0, 40, 1, 1, "pieces", true, true, "",
            GA_INT(provisionsConjureBelow),
            "A mage conjures water and food below",
            "A mage that carries fewer pieces of water (or, from level 6, food) conjures some when nothing hostile is near, up to twice this. It buys none and takes none out of the guild storage. 0 = it conjures only in a rest in which it has none." },

        // --- Gear and the guild's gold ---------------------------------------------------------------------------
        { Setting::StorageSupplyGear, "Storage.Supply.Gear", "gear", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(storageSupplyGear),
            "Better gear from the guild storage",
            "A bot takes the weapon or armour piece from the guild storage that is the best upgrade for it, and gives back what it cannot wear." },
        { Setting::GearBuyEnable, "Gear.Buy.Enable", "gear", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(gearBuyEnable),
            "Buy gear upgrades at vendors",
            "A bot at a vendor buys weapons and armour that are upgrades for it, paid within the guild's budget." },
        { Setting::GearBuyNearYards, "Gear.Buy.NearYards", "gear", SettingType::Int, 0, 1000, 25, 1, "yd", true, false, "",
            GA_FLOAT(gearBuyNearYards, 1),
            "Walk to a gear vendor within",
            "A bot walks to a vendor this near that sells an upgrade for it. 0 = it buys only at vendors it visits anyway." },
        { Setting::GearWarnGap, "Gear.WarnGap", "gear", SettingType::Int, 0, 40, 1, 1, "levels", true, false, "",
            GA_INT(gearWarnGap),
            "Warn about gear far behind at",
            "You get a warning when a bot's weapon or body armour is this many item levels below its level. 0 = never." },
        { Setting::DungeonRolesWearForLevelling, "DungeonRoles.WearForLevelling", "gear", SettingType::Enum, 0, 2, 1, 1, "", true, false,
            "off=Never;armour=Armour and jewellery;all=Armour, jewellery and weapons",
            GA_INT(rolesWearForLevelling),
            "Wear the dungeon set while levelling",
            "A tank or healer wears the pieces of its role store while it levels when they are better for its levelling talents than what it wears: armour and weapons when they are an upgrade by the bots' own gear rating, jewellery by the stats (a bare place takes any). They stay part of its dungeon set: a run finds them on it. Weapons piece by piece: a sword replaces a two-hand weapon only when the sword alone is the upgrade." },
        { Setting::GearFillEmptySlots, "Gear.FillEmptySlots", "gear", SettingType::Bool, 0, 1, 1, 1, "", true, true, "",
            GA_BOOL(gearFillEmptySlots),
            "Fill empty slots",
            "Armour in the bags goes on where the bot wears nothing, whatever its kind." },
        { Setting::BudgetItemPercent, "Budget.ItemPercent", "gear", SettingType::Int, 0, 100, 1, 1, "%", false, false, "",
            GA_INT(budgetItemPercent),
            "Most one item may cost",
            "The guild bank pays at most this share of its money for one piece of gear or one quest item (the bot pays the rest of its own money). 0 = the guild bank pays for nothing." },
        { Setting::BudgetBotAllowance, "Budget.BotAllowance", "gear", SettingType::Money, 0, 10000000, 1000, 1, "", true, false, "",
            GA_INT(budgetBotAllowance),
            "Gold budget per bot",
            "What one guild character may spend on gear and quest items over its whole life (what it spent so far: .ga admin gear)." },
        { Setting::BudgetMemberPercent, "Budget.MemberPercent", "gear", SettingType::Int, 0, 100, 5, 1, "%", false, true, "",
            GA_INT(budgetMemberPercent),
            "What one character gives per purchase",
            "The share of its own money a guild character pays for one purchase at most; the guild bank pays what is missing." },
        { Setting::GuildTaxPercent, "Guild.TaxPercent", "gear", SettingType::Int, 0, 100, 1, 1, "%", false, false, "",
            GA_INT(guildTaxPercent),
            "Guild tax",
            "This share of every copper a roster bot gets (looted coin, quest rewards, what it sells, money by mail or trade from outside the guild) goes into the guild bank at the moment it gets it, a line each in the activity log. Not taxed: the guild bank's own gold, money handed over within the guild (see below), and the guild leader, who pays no tax. 0 = no tax." },
        { Setting::UnlocksPayFrom, "Unlocks.PayFrom", "gear", SettingType::Enum, 0, 2, 1, 1, "", false, false,
            "bank=The guild bank;bankleader=The guild bank, then the leader's gold;leader=The leader's gold",
            GA_INT(unlocksPayFrom),
            "Who pays for unlocks",
            "Unlocks that cost gold (more bot slots on the Progression tab) are paid from this. The guild bank, then the leader's gold: what the guild bank lacks comes out of the guild leader's purse (the leader must be in the world)." },
        { Setting::GuildTaxHandovers, "Guild.TaxHandovers", "gear", SettingType::Bool, 0, 1, 1, 1, "", false, true, "",
            GA_BOOL(guildTaxHandovers),
            "Tax money handed over within the guild",
            "On: money a bot gets by trade or mail from your own characters or another character of the guild is taxed too. Off: it is no income and goes untaxed (the money was taxed when it was earned)." },

        // --- Class and training ----------------------------------------------------------------------------------
        { Setting::AutoLearnClassSpells, "AutoLearnClassSpells", "class", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(autoLearnClassSpells),
            "Learn class spells where they are",
            "A bot learns its class trainer's new spells on every level where it stands, for free. Off: it walks to its trainer and pays." },
        { Setting::ClassAbilitiesEnable, "ClassAbilities.Enable", "class", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(classAbilitiesEnable),
            "Abilities of the class quests",
            "A bot gets what only class quests teach (stances, totems, demons, bear form ...) when it reaches the level." },
        { Setting::ClassAbilitiesHunterPet, "ClassAbilities.HunterPet", "class", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(classAbilitiesHunterPet),
            "Hunter pets",
            "A hunter gets a first pet at level 10 and a good one at 60 (exotic for a beast master)." },
        { Setting::ClassAbilitiesPetTalents, "ClassAbilities.PetTalents", "class", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(classAbilitiesPetTalents),
            "Hunter pet talents",
            "A hunter's pet spends its talent points along a fixed build of its tree: Ferocity and Cunning for damage, Tenacity for tanking. A pet whose talents the owner chose himself is left alone." },
        { Setting::PoisonsEnable, "Poisons.Enable", "class", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(poisonsEnable),
            "Rogue poisons",
            "From level 20 a rogue keeps a stock of poisons for its weapons: out of the guild storage first, else bought at a poison vendor at a hub after a hand-in or at a vendor it visits anyway. It puts them on by itself." },
        { Setting::PoisonsStock, "Poisons.Stock", "class", SettingType::Int, 0, 40, 1, 1, "", true, true, "",
            GA_INT(poisonsStock),
            "Poison doses per hand",
            "How many doses a rogue keeps for each weapon hand (one dose lasts an hour). It tops up at a hub when it has less than half of it." },
        { Setting::PoisonsPair, "Poisons.Pair", "class", SettingType::Enum, 0, 2, 1, 1, "", true, false,
            "split=Instant on the main hand, Deadly on the off hand;instant=Instant on both hands;deadly=Deadly on both hands",
            GA_INT(poisonsPair),
            "Poisons on the weapons",
            "Which poisons a rogue keeps. Deadly Poison comes at level 30; before that it is Instant Poison on both hands." },
        { Setting::ClassAbilitiesRuneforging, "ClassAbilities.Runeforging", "class", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(classAbilitiesRuneforging),
            "Death knight runes",
            "A death knight puts the best rune it knows on a weapon that has no enchantment (as at a runeforge, where it stands): for a tank Stoneskin Gargoyle or Swordshattering, else Fallen Crusader, Razorice or Cinderglacier. A weapon that has any enchantment is left alone." },
        { Setting::DeathKnightsSkipStart, "DeathKnights.SkipStart", "class", SettingType::Enum, 0, 2, 1, 1, "", false, false,
            "acherus=In Acherus (the starting area);rewards=In the capital, with the campaign's rewards;outfit=In the capital, with the starting gear only",
            GA_INT(deathKnightsSkipStart),
            "Death knight recruits start",
            "Where a death knight you recruit begins. In the capital: at the core's death knight level (55) in Stormwind or Orgrimmar with its hearthstone bound there, the starting campaign counted as done (Death Gate, its mount, runeforging, reputation; no experience, no gold) and ready to level like any guild bot; with the campaign's rewards it also gets the campaign's item rewards (blue, item level 65-70) and wears what is better for it. In Acherus: the bot stands in the starting area, which the bots do not play." },

        // --- Professions ----------------------------------------------------------------------------------------
        { Setting::ProfessionsLearnInPlace, "Professions.LearnInPlace", "professions", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(professionsLearnInPlace),
            "Learn planned professions where they are",
            "The professions of a bot's plan, their ranks, recipes and tools come where it stands (like class spells)." },
        { Setting::ProfessionsGatherDetour, "Professions.GatherDetour", "professions", SettingType::Int, 0, 100, 5, 1, "yd", true, false, "",
            GA_FLOAT(professionsGatherDetour, 1),
            "Gather on the way within",
            "A herb, vein or skinnable corpse the bot can gather this close to its way is fetched. 0 = never." },
        { Setting::ProfessionsCatchUpGathering, "Professions.CatchUpGathering", "professions", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(professionsCatchUpGathering),
            "Catch up a gathering skill before questing",
            "On the autopilot: when Mining, Herbalism or Skinning is too low for everything in the bot's zone, it goes where its skill can gather and quests on once the zone's lowest node is in reach." },
        { Setting::ProfessionsCatchUpMinGap, "Professions.CatchUpMinGap", "professions", SettingType::Int, 0, 150, 5, 1, "points", true, false, "",
            GA_INT(professionsCatchUpMinGap),
            "... only when it is this far behind",
            "A catch-up is a journey to where the skill can gather: it only begins when the skill is at least this many points below what the zone's lowest node asks for. 0 = for any gap." },
        { Setting::ProfessionsFocusBehind, "Professions.FocusBehind", "professions", SettingType::Int, 0, 300, 5, 1, "points", true, false, "",
            GA_INT(professionsFocusBehind),
            "Profession mode: a profession this far behind first",
            "In the profession mode a profession more than this many points below the bot's highest is worked on alone until it is within that again: a crafting one with the guild storage's materials (bars and the like made first), a gathering one by gathering. 0 = the professions always take turns." },
        { Setting::ProfessionsGreenMinChance, "Professions.GreenMinChance", "professions", SettingType::Int, 0, 100, 5, 1, "%", true, false, "",
            GA_INT(professionsGreenMinChance),
            "No green recipe below this chance",
            "Crafting and enchanting for skill take orange and yellow recipes first and a green one only when no orange or yellow one can be made; a green recipe whose chance of a skill point a cast is below this is not made at all (its materials would go for little). 0 = any green recipe." },
        { Setting::ProfessionsDisenchantSurplus, "Professions.DisenchantSurplus", "professions", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(professionsDisenchantSurplus),
            "Disenchant surplus crafted gear",
            "An enchanter disenchants crafted green and blue weapons and armour of the guild storage that nobody of the guild can put on, and the copies of a crafted piece beyond those somebody would wear (one always stays). At most three per look." },
        { Setting::ProfessionsCookStock, "Professions.CookStock", "professions", SettingType::Int, 0, 200, 10, 1, "pieces", true, false, "",
            GA_INT(professionsCookStock),
            "Cooks keep this much food in the storage",
            "A bot that can cook makes food for the guild out of the guild storage's meat and fish and its own: the kinds the guild's characters eat at their levels, food with a buff first, until the storage holds this many of a kind. At a fire, after a hand-in at a hub and in the profession mode. 0 = no food for the guild." },
        { Setting::ProfessionsAmmoStock, "Professions.AmmoStock", "professions", SettingType::Int, 0, 20000, 500, 1, "rounds", true, false, "",
            GA_INT(professionsAmmoStock),
            "Engineers keep this much ammunition in the storage",
            "A bot with Engineering makes ammunition for the guild's hunters with a gun when the guild storage holds less than this of their kind: for each such hunter the best ammunition its recipes make that the hunter can use, out of its bags and the guild storage (blasting powder and bars are made first, nothing is bought). After a hand-in at a hub and in the profession mode. 0 = no ammunition for the guild." },
        { Setting::ProfessionsConsumableStock, "Professions.ConsumableStock", "professions", SettingType::Int, 0, 200, 5, 1, "pieces", true, false, "",
            GA_INT(professionsConsumableStock),
            "Crafters keep this many endgame consumables in the storage",
            "A bot whose recipes make the consumables of the guild's dungeon runs (elixirs and flasks, scrolls, sharpening stones, weightstones and oils, scopes, healing and mana potions) makes them out of the guild storage when it holds fewer than this of one: of each kind the best its recipes make for the guild's highest level, nothing more than 15 item levels below it. What a recipe lacks that the bot makes itself comes first, a vendor's material is bought from the guild's budget. After a hand-in at a hub, in the profession mode, and at the level cap the bot goes into the profession mode for it. A dungeon run's characters use them at its start and after a wipe. 0 = none for the guild." },
        { Setting::ProfessionsEnchantScrolls, "Professions.EnchantScrolls", "professions", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(professionsEnchantScrolls),
            "Enchantments for the guild's gear",
            "An enchanter writes the best enchantment it can make for a piece a guild character wears (blue quality or better, item level Professions.EnchantMinItemLevel or more, the best for the wearer's dungeon role, else its talents) on a vellum of the guild storage; the scroll goes into the guild storage and the wearer puts it on. A vellum the storage lacks is ordered from the guild's scribes. Off for an enchanter: it writes none; off for a bot: it puts none on." },
        { Setting::ProfessionsEnchantMinItemLevel, "Professions.EnchantMinItemLevel", "professions", SettingType::Int, 1, 300, 5, 1, "", false, false, "",
            GA_INT(professionsEnchantMinItemLevel),
            "Guild enchantments from this item level",
            "The enchantments the guild's enchanters write for the guild's gear (Professions.EnchantScrolls) go on pieces of blue quality or better from this item level on." },
        { Setting::ProfessionsProcessAbove, "Professions.ProcessAbove", "professions", SettingType::Int, 0, 100000, 100, 1, "pieces", true, false, "",
            GA_INT(professionsProcessAbove),
            "Materials are processed above this many in the storage",
            "When the guild storage holds more than this of a raw material (ore, stone, cloth, leather, bars), a bot whose recipes process it makes the processed material for the guild: ore into bars, stone into grinding stones, cloth into bolts, bars into parts. It works on what is above this many and stops there. Vendor materials are bought from the guild's budget, an anvil or forge is walked to. After a hand-in at a hub and in the profession mode; at the level cap the bot goes into the profession mode for it. 0 = nothing is processed." },
        { Setting::ProfessionsProcessStock, "Professions.ProcessStock", "professions", SettingType::Int, 0, 100000, 50, 1, "pieces", true, false, "",
            GA_INT(professionsProcessStock),
            "A processed material is made up to this many",
            "The stock of a processed material (a bolt, a grinding stone, a part) the processing above makes, at most. Bars out of ore are made whatever their stock: ore is good for nothing else. 0 = no cap." },
        { Setting::ProfessionsSkillCraftStock, "Professions.SkillCraftStock", "professions", SettingType::Int, 0, 5000, 50, 1, "pieces", true, false, "",
            GA_INT(professionsSkillCraftStock),
            "No consumable made for skill beyond this stock",
            "A bot that crafts for its skill does not make a consumable (a potion, a bandage, food, an explosive) while the guild storage holds this many of it, nor ammunition while it holds Professions.AmmoStock of it: it takes another recipe, or waits. Nothing is sold or deleted. 0 = no cap." },
        { Setting::ProfessionsCookMinutes, "Professions.CookMinutes", "professions", SettingType::Int, 5, 120, 5, 1, "min", true, true, "",
            GA_INT(professionsCookMinutes),
            "... looked at again after",
            "How long a cook waits before it looks again after it found nothing to cook for the guild." },
        { Setting::ProfessionsBuyRecipes, "Professions.BuyRecipes", "professions", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(professionsBuyRecipes),
            "Buy recipes at vendors",
            "A bot buys the recipes of its professions that a vendor it visits or passes sells and that it can learn now, and learns them at once. The guild bank pays within the guild's budget." },
        { Setting::ProfessionsRecipeMaxPrice, "Professions.RecipeMaxPrice", "professions", SettingType::Money, 0, 10000000, 1000, 1, "", true, false, "",
            GA_INT(professionsRecipeMaxPrice),
            "... for at most",
            "The most one recipe may cost." },
        { Setting::ProfessionsRecipeYards, "Professions.RecipeYards", "professions", SettingType::Int, 0, 1000, 25, 1, "yd", true, true, "",
            GA_FLOAT(professionsRecipeYards, 1),
            "... walking to a vendor within",
            "A bot walks to a vendor this near that sells a recipe it can learn. 0 = it buys only at vendors it visits anyway." },
        { Setting::ProfessionsCraftAtHub, "Professions.CraftAtHub", "professions", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(professionsCraftAtHub),
            "Craft for skill at a hub",
            "After a hand-in at a hub a bot crafts what still gives it skill, from the guild storage's materials." },
        { Setting::ProfessionsCraftBatch, "Professions.CraftBatch", "professions", SettingType::Int, 1, 50, 1, 1, "casts", true, true, "",
            GA_INT(professionsCraftBatch),
            "... at most this many casts per visit",
            "How often one recipe is crafted at one visit." },
        { Setting::ProfessionsCraftGear, "Professions.CraftGear", "professions", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(professionsCraftGear),
            "Craft gear for the guild",
            "At a hub a bot also crafts a piece of gear that a guild character would wear, to level in or for its dungeon role. It uses the recipe's materials from the crafter's bags and the guild storage. Off unless you switch it on." },
        { Setting::ProfessionsCraftOrders, "Professions.CraftOrders", "professions", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(professionsCraftOrders),
            "Work on the guild's craft orders",
            "On a town trip a bot makes what the guild ordered (Storage tab, Craft orders) when it knows a recipe for it: from its bags and the guild storage, the result goes into the guild storage. Materials a vendor sells are bought from the guild's budget." },
        { Setting::ProfessionsDisenchantForOrders, "Professions.DisenchantForOrders", "professions", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(professionsDisenchantForOrders),
            "Disenchant storage items for materials",
            "When a craft order or an enchanter's own skill-up lacks dust or essences, an enchanter turns green and blue weapons and armour of the guild storage that nobody of the guild would wear into their materials (at most three per town trip). Every item is named in the activity log." },

        // --- Guild chat ------------------------------------------------------------------------------------------
        { Setting::BotGuildChatMinSeconds, "BotGuildChat.MinSeconds", "chat", SettingType::Int, 0, 3600, 5, 1, "s", false, false, "",
            GA_INT(botGuildChatMinSeconds),
            "Bots in guild chat at most every",
            "Your client shows one guild chat line of the bots per pause; the pause is random between this and the next value. 0 = every line." },
        { Setting::BotGuildChatMaxSeconds, "BotGuildChat.MaxSeconds", "chat", SettingType::Int, 0, 3600, 5, 1, "s", false, false, "",
            GA_INT(botGuildChatMaxSeconds),
            "... up to",
            "The longest pause between two guild chat lines of the bots." },

        // --- Loot ------------------------------------------------------------------------------------------------
        { Setting::LootSellJunk, "LootFilter.SellJunk", "loot", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(lootSellJunk),
            "Sell junk at once",
            "Loot no rule of the loot filter decides is sold at once when it is junk: grey items, and white weapons and armour no guild character would wear. Crafting materials, quest items, consumables, bags, ammunition, recipes and what a bot uses itself are kept. The money goes to the looter." },
        { Setting::LootSellJunkQuality, "LootFilter.SellJunkQuality", "loot", SettingType::Enum, 0, 1, 1, 1, "", true, false,
            "grey=Grey only;white=Grey and white",
            GA_INT(lootSellJunkQuality),
            "Junk is",
            "Grey only: white weapons and armour go the default way (the guild storage) like any other loot." },
        { Setting::LootKeepWornGear, "LootFilter.KeepWornGear", "loot", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(lootKeepWornGear),
            "Keep gear a guild character would wear",
            "Off (the owner: grey and white weapons and armour are junk): they are sold unless the looter puts them on itself. On: a grey or white piece that is an upgrade for a guild character in the world goes into the guild storage instead." },
        { Setting::StorageUnwornBound, "Storage.UnwornBound", "loot", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(storageUnwornBound),
            "Never-worn bind-on-pickup loot into the storage",
            "A bind-on-pickup weapon or armour piece (green or better) that the character looted or got as a quest reward and never put on may go into the guild storage, so another guild character can wear it or an enchanter can disenchant it. What it has worn: see the next setting." },
        { Setting::LootWornGear, "LootFilter.WornGear", "loot", SettingType::Enum, 0, 2, 1, 1, "", true, false,
            "keep=Keep it;both=Disenchant or sell;de=Disenchant only",
            GA_INT(lootWornGear),
            "Gear it has worn and will not wear again",
            "A green or blue weapon or armour piece a bot has worn (soulbound: the guild storage cannot take it) and will not wear again: disenchanted by the bot itself when its Enchanting is high enough (the materials go into the guild storage), else sold at its next vendor visit. Never its dungeon set, a tool, a quest item, a unique piece, a piece the guild keeps a reserve of or one that came out of the guild storage. Keep it: such pieces stay in its bags." },
        { Setting::LootOpenContainers, "LootFilter.OpenContainers", "loot", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(lootOpenContainers),
            "Open containers",
            "A bot opens the containers it loots at once (clams, bags of loot, crates; a lockbox when it is a rogue with enough Lockpicking): what comes out goes through the loot filter like other loot. A lockbox it cannot pick goes the usual way." },
        { Setting::StorageOpenContainers, "Storage.OpenContainers", "loot", SettingType::Bool, 0, 1, 1, 1, "", false, false, "",
            GA_BOOL(storageOpenContainers),
            "Open the storage's containers",
            "Bots take the containers of the guild storage out, a few at a time, and open them (lockboxes only a rogue with enough Lockpicking); what comes out goes through the loot filter." },
        { Setting::StorageDarkmoonCards, "Storage.DarkmoonCards", "loot", SettingType::Bool, 0, 1, 1, 1, "", false, false, "",
            GA_BOOL(storageDarkmoonCards),
            "Darkmoon cards into the guild storage",
            "The Darkmoon Faire cards (Ace through Eight of a set) and decks a bot loots or carries go into the guild storage, in a category of their own. Quest items to the game, they are never sold as junk. Off: they stay in its bags." },
        { Setting::StorageReputationTurnIns, "Storage.ReputationTurnIns", "loot", SettingType::Bool, 0, 1, 1, 1, "", false, false, "",
            GA_BOOL(storageReputationTurnIns),
            "Reputation turn-ins into the guild storage",
            "The quest items repeatable reputation quests ask for (the Argent Dawn's Core of Elements, Bone Fragments and Scourgestones, the Darkmoon Faire's Evil Bat Eyes) a bot loots or carries go into the guild storage, in a category of their own, unless a quest in its log asks for them. Off: they stay in its bags." },
        { Setting::AuctionSellUnsold, "Auction.SellUnsold", "loot", SettingType::Bool, 0, 1, 1, 1, "", false, false, "",
            GA_BOOL(auctionSellUnsold),
            "Sell what the auction house sends back",
            "What the guild storage put up in the auction house and did not sell is sold at the vendor price when it comes back, the copper into the guild bank; only what the item's auction rule or its reserve keeps goes back into the storage. Never tools, Darkmoon cards or reputation turn-ins, nor what no vendor buys. Off: it all goes back into the storage and is put up again." },
        { Setting::StorageLockboxMinutes, "Storage.LockboxMinutes", "loot", SettingType::Int, 0, 1440, 5, 1, "min", false, true, "",
            GA_INT(storageLockboxMinutes),
            "A lockbox from the storage every",
            "Lockboxes stay in the guild storage, and a rogue whose Lockpicking is high enough or a blacksmith with a skeleton key for it takes one out and opens it regularly: at most one per character in this time. 0: as many as it can, like the other containers." },
        { Setting::StorageReserveForDungeons, "Storage.ReserveForDungeons", "loot", SettingType::Bool, 0, 1, 1, 1, "", false, false, "",
            GA_BOOL(storageReserveForDungeons),
            "Dungeon runs may use the reserves",
            "The reserve of an item in the guild storage is what the bots leave there while they level. On: a dungeon run may use it (what its bots use at the start and after a wipe). Off: a dungeon run leaves the reserves too." },
        { Setting::LootSpareQuestItems, "LootFilter.SpareQuestItems", "loot", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(lootSpareQuestItems),
            "Quest items no quest needs any more",
            "A grey or white quest item that no quest in the bot's log uses and no quest it can still do for experience asks for is sold at its next vendor visit, or deleted where it is when no vendor buys it. A quest starter goes only once its quest gives no experience any more (done, grey or the level cap). Off: they stay in its bags." },
        { Setting::ShoppingEnable, "Shopping.Enable", "loot", SettingType::Bool, 0, 1, 1, 1, "", true, false, "",
            GA_BOOL(shoppingEnable),
            "Buy for the shopping list",
            "At a vendor it visits anyway, a bot buys what the guild storage lacks of the shopping list (the Storage tab) and this vendor sells, paid from the guild bank; it goes straight into the guild storage. No trip is made for the list." },
        { Setting::ShoppingMaxPerVisit, "Shopping.MaxPerVisit", "loot", SettingType::Money, 0, 100000000, 10000, 1, "", false, false, "",
            GA_INT(shoppingMaxPerVisit),
            "Shopping list: most per vendor visit",
            "The most a bot spends of the guild bank on the shopping list at one vendor visit. 0 = nothing is bought." },
        { Setting::ShoppingMaxPerDay, "Shopping.MaxPerDay", "loot", SettingType::Money, 0, 1000000000, 10000, 1, "", false, false, "",
            GA_INT(shoppingMaxPerDay),
            "Shopping list: most per day",
            "The most the guild spends of the guild bank on the shopping list in one day (server time). 0 = nothing is bought." },
        { Setting::ShoppingFromPurse, "Shopping.FromPurse", "loot", SettingType::Bool, 0, 1, 1, 1, "", false, true, "",
            GA_BOOL(shoppingFromPurse),
            "Shopping list: the bot's gold when the guild bank is short",
            "Off: what the guild bank cannot pay is not bought (a line in the activity log). On: the bot pays the rest from its own gold." },
    };

    std::vector<SettingCategory> const sCategories =
    {
        { "combat", "Combat and rest", "When bots rest, and when they leave a fight." },
        { "danger", "Danger and deaths", "How careful bots choose their spots, and what happens after deaths." },
        { "quests", "Questing and travel", "Which quests bots take and how they travel." },
        { "grind", "Grinding", "Whether and where bots grind when no quest is left." },
        { "farm", "Farm orders", "What a bot with the order to farm an item hunts, and where." },
        { "upkeep", "Vendors and provisions", "Food, drink and vendor trips." },
        { "gear", "Gear and guild gold", "Where bots get gear and how much of the guild's gold they may spend." },
        { "professions", "Professions", "Gathering and crafting by the bots' profession plans." },
        { "class", "Class and training", "Spells and abilities bots get on their own." },
        { "chat", "Guild chat", "How often the bots' guild chat reaches you." },
        { "loot", "Loot", "What happens to what the bots loot before it reaches the guild storage." },
        { "dungeons", "Dungeon runs", "How a dungeon group goes its way, pulls and waits before a boss." },
    };

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    }

    std::string Trim(std::string const& text)
    {
        std::size_t const first = text.find_first_not_of(" \t");
        if (first == std::string::npos)
            return {};
        std::size_t const last = text.find_last_not_of(" \t");
        return text.substr(first, last - first + 1);
    }

    bool ParseInteger(std::string const& text, int64& out)
    {
        if (text.empty())
            return false;
        char* end = nullptr;
        long long const value = std::strtoll(text.c_str(), &end, 10);
        if (!end || *end != '\0')
            return false;
        out = value;
        return true;
    }

    // FNV-1a
    void Hash(uint32& hash, std::string const& text)
    {
        for (unsigned char c : text)
        {
            hash ^= c;
            hash *= 16777619u;
        }
        hash ^= 0xFFu;
        hash *= 16777619u;
    }
}

char const* GuildAdventure::ToToken(SettingType type)
{
    switch (type)
    {
        case SettingType::Bool: return "bool";
        case SettingType::Int: return "int";
        case SettingType::Enum: return "enum";
        case SettingType::Money: return "money";
        case SettingType::Decimal: return "dec";
    }
    return "int";
}

BotSettings& BotSettings::Instance()
{
    static BotSettings instance;
    return instance;
}

std::vector<SettingDef> const& BotSettings::Defs()
{
    return sDefs;
}

std::vector<SettingCategory> const& BotSettings::Categories()
{
    return sCategories;
}

SettingDef const* BotSettings::Find(Setting id)
{
    return Find(uint32(id));
}

SettingDef const* BotSettings::Find(uint32 id)
{
    static std::unordered_map<uint32, SettingDef const*> const byId = []()
    {
        std::unordered_map<uint32, SettingDef const*> map;
        for (SettingDef const& def : sDefs)
            map.emplace(uint32(def.id), &def);
        return map;
    }();
    auto itr = byId.find(id);
    return itr == byId.end() ? nullptr : itr->second;
}

SettingDef const* BotSettings::Find(std::string const& key)
{
    std::string wanted = Lower(Trim(key));
    if (wanted.rfind("guildadventure.", 0) == 0)
        wanted = wanted.substr(15);
    for (SettingDef const& def : sDefs)
        if (Lower(def.key) == wanted)
            return &def;
    return nullptr;
}

std::vector<std::pair<std::string, std::string>> BotSettings::Options(SettingDef const& def)
{
    std::vector<std::pair<std::string, std::string>> options;
    std::string const text = def.options ? def.options : "";
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find(';', start);
        if (end == std::string::npos)
            end = text.size();
        std::string const item = text.substr(start, end - start);
        std::size_t const eq = item.find('=');
        if (eq == std::string::npos)
            options.emplace_back(item, item);
        else
            options.emplace_back(item.substr(0, eq), item.substr(eq + 1));
        start = end + 1;
    }
    return options;
}

// ---------------------------------------------------------------------------------------------------------------
// Startup and update
// ---------------------------------------------------------------------------------------------------------------

void BotSettings::Load()
{
    std::unordered_map<uint32, SettingValues> guildValues;
    std::unordered_map<ObjectGuid, SettingValues> characterValues;
    uint32 unknown = 0;

    if (QueryResult result = CharacterDatabase.Query("SELECT owner_account, setting_key, value FROM ga_guild_setting"))
    {
        do
        {
            Field* fields = result->Fetch();
            SettingDef const* def = Find(fields[1].Get<std::string>());
            if (!def)
            {
                ++unknown;
                continue;
            }
            guildValues[fields[0].Get<uint32>()][uint16(def->id)] = fields[2].Get<int32>();
        } while (result->NextRow());
    }

    if (QueryResult result = CharacterDatabase.Query("SELECT character_guid, setting_key, value FROM ga_character_setting"))
    {
        do
        {
            Field* fields = result->Fetch();
            SettingDef const* def = Find(fields[1].Get<std::string>());
            if (!def || !def->perCharacter)
            {
                ++unknown;
                continue;
            }
            characterValues[ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>())][uint16(def->id)] =
                fields[2].Get<int32>();
        } while (result->NextRow());
    }

    {
        std::unique_lock lock(_lock);
        _guildValues = std::move(guildValues);
        _characterValues = std::move(characterValues);
    }
    RebuildIndex();
    _schemaVersion = ComputeSchemaVersion();

    std::size_t guildCount = 0;
    std::size_t characterCount = 0;
    {
        std::shared_lock lock(_lock);
        for (auto const& [owner, values] : _guildValues)
            guildCount += values.size();
        for (auto const& [guid, values] : _characterValues)
            characterCount += values.size();
    }
    LOG_INFO("module", "mod-guild-adventure: bot settings: {} settings in {} categories, {} values of guilds, {} of "
        "characters{}", sDefs.size(), sCategories.size(), guildCount, characterCount,
        unknown ? Acore::StringFormat(" ({} rows of unknown or guild-wide settings ignored)", unknown) : std::string());
}

bool BotSettings::Update(uint32 diff)
{
    _indexTimer += diff;
    if (_indexTimer >= IndexIntervalMs)
    {
        _indexTimer = 0;
        RebuildIndex();
    }

    _schemaTimer += diff;
    if (_schemaTimer < SchemaIntervalMs)
        return false;
    _schemaTimer = 0;
    uint32 const version = ComputeSchemaVersion();
    if (version == _schemaVersion)
        return false;
    _schemaVersion = version;
    return true;
}

void BotSettings::RebuildIndex()
{
    std::unordered_map<ObjectGuid, uint32> owners;
    for (auto const& [owner, guild] : sGuildAdventureMgr.GetGuilds())
    {
        if (!guild.leader.IsEmpty())
            owners[guild.leader] = owner;
        for (auto const& [guid, bot] : guild.bots)
            owners[guid] = owner;
    }
    std::unique_lock lock(_lock);
    _owners = std::move(owners);
}

uint32 BotSettings::ComputeSchemaVersion() const
{
    uint32 hash = 2166136261u;
    for (SettingCategory const& category : sCategories)
    {
        Hash(hash, category.key);
        Hash(hash, category.label);
        Hash(hash, category.help);
    }
    for (SettingDef const& def : sDefs)
    {
        Hash(hash, Acore::StringFormat("{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", uint32(def.id), def.key, def.category,
            ToToken(def.type), def.min, def.max, def.step, def.scale, def.unit, def.perCharacter, def.advanced,
            def.options, Default(def)));
        Hash(hash, def.label);
        Hash(hash, def.help);
    }
    return hash ? hash : 1;
}

// ---------------------------------------------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------------------------------------------

int32 BotSettings::Default(SettingDef const& def)
{
    return def.fromConf ? def.fromConf(GetSettings()) : 0;
}

int32 BotSettings::Default(Setting id)
{
    SettingDef const* def = Find(id);
    return def ? Default(*def) : 0;
}

uint32 BotSettings::OwnerOf(ObjectGuid character) const
{
    auto itr = _owners.find(character);
    return itr == _owners.end() ? 0 : itr->second;
}

int32 BotSettings::Get(ObjectGuid character, Setting id) const
{
    SettingDef const* def = Find(id);
    if (!def)
        return 0;
    if (!character.IsEmpty())
    {
        std::shared_lock lock(_lock);
        if (def->perCharacter)
        {
            auto own = _characterValues.find(character);
            if (own != _characterValues.end())
            {
                auto value = own->second.find(uint16(id));
                if (value != own->second.end())
                    return value->second;
            }
        }
        if (uint32 const owner = OwnerOf(character))
        {
            auto guild = _guildValues.find(owner);
            if (guild != _guildValues.end())
            {
                auto value = guild->second.find(uint16(id));
                if (value != guild->second.end())
                    return value->second;
            }
        }
    }
    return Default(*def);
}

int32 BotSettings::Get(Player const* character, Setting id) const
{
    return Get(character ? character->GetGUID() : ObjectGuid::Empty, id);
}

float BotSettings::GetFloat(ObjectGuid character, Setting id) const
{
    SettingDef const* def = Find(id);
    int32 const value = Get(character, id);
    return def && def->scale > 1 ? float(value) / float(def->scale) : float(value);
}

float BotSettings::GetFloat(Player const* character, Setting id) const
{
    return GetFloat(character ? character->GetGUID() : ObjectGuid::Empty, id);
}

Settings BotSettings::For(ObjectGuid character) const
{
    Settings settings = GetSettings();
    if (character.IsEmpty())
        return settings;
    {
        std::shared_lock lock(_lock);
        SettingValues const* own = nullptr;
        auto ownItr = _characterValues.find(character);
        if (ownItr != _characterValues.end())
            own = &ownItr->second;
        SettingValues const* guild = nullptr;
        if (uint32 const owner = OwnerOf(character))
        {
            auto guildItr = _guildValues.find(owner);
            if (guildItr != _guildValues.end())
                guild = &guildItr->second;
        }
        if (!own && !guild)
            return settings;
        for (SettingDef const& def : sDefs)
        {
            if (!def.toSettings)
                continue;
            uint16 const id = uint16(def.id);
            if (own && def.perCharacter)
            {
                auto value = own->find(id);
                if (value != own->end())
                {
                    def.toSettings(settings, value->second);
                    continue;
                }
            }
            if (guild)
            {
                auto value = guild->find(id);
                if (value != guild->end())
                    def.toSettings(settings, value->second);
            }
        }
    }
    // Pairs that must stay in order whatever was set (the conf loader clamps them the same way).
    settings.restUntilHealth = std::max(settings.restUntilHealth, settings.restBelowHealth);
    settings.restUntilMana = std::max(settings.restUntilMana, settings.restBelowMana);
    settings.botGuildChatMaxSeconds = std::max(settings.botGuildChatMaxSeconds, settings.botGuildChatMinSeconds);
    return settings;
}

Settings BotSettings::For(Player const* character) const
{
    return For(character ? character->GetGUID() : ObjectGuid::Empty);
}

int32 BotSettings::GetForGuild(uint32 ownerAccount, Setting id) const
{
    SettingDef const* def = Find(id);
    if (!def)
        return 0;
    {
        std::shared_lock lock(_lock);
        auto guild = _guildValues.find(ownerAccount);
        if (guild != _guildValues.end())
        {
            auto value = guild->second.find(uint16(id));
            if (value != guild->second.end())
                return value->second;
        }
    }
    return Default(*def);
}

SettingValues BotSettings::GuildValues(uint32 ownerAccount) const
{
    std::shared_lock lock(_lock);
    auto itr = _guildValues.find(ownerAccount);
    return itr == _guildValues.end() ? SettingValues() : itr->second;
}

SettingValues BotSettings::CharacterValues(ObjectGuid character) const
{
    std::shared_lock lock(_lock);
    auto itr = _characterValues.find(character);
    return itr == _characterValues.end() ? SettingValues() : itr->second;
}

std::vector<ObjectGuid> BotSettings::CharactersWithValues(uint32 ownerAccount) const
{
    std::vector<ObjectGuid> characters;
    AdventureGuild const* guild = sGuildAdventureMgr.FindGuild(ownerAccount);
    if (!guild)
        return characters;
    std::shared_lock lock(_lock);
    auto has = [&](ObjectGuid guid)
    {
        auto itr = _characterValues.find(guid);
        return itr != _characterValues.end() && !itr->second.empty();
    };
    if (has(guild->leader))
        characters.push_back(guild->leader);
    for (auto const& [guid, bot] : guild->bots)
        if (has(guid))
            characters.push_back(guid);
    return characters;
}

// ---------------------------------------------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------------------------------------------

bool BotSettings::Check(SettingDef const* def, bool forCharacter, int64 value, std::string& code, std::string& text)
{
    if (!def)
    {
        code = "UNKNOWN";
        text = "There is no such setting.";
        return false;
    }
    if (forCharacter && !def->perCharacter)
    {
        code = "GUILD_ONLY";
        text = Acore::StringFormat("\"{}\" ({}) is set for the whole guild only.", def->label, def->key);
        return false;
    }
    int64 max = def->max;
    if (def->type == SettingType::Enum)
        max = int64(Options(*def).size()) - 1;
    if (value < def->min || value > max)
    {
        code = "RANGE";
        text = Acore::StringFormat("\"{}\" ({}) takes {} to {}.", def->label, def->key, Format(*def, def->min),
            Format(*def, int32(max)));
        return false;
    }
    return true;
}

void BotSettings::Set(uint32 ownerAccount, ObjectGuid character, Setting id, int32 value)
{
    SettingDef const* def = Find(id);
    if (!def)
        return;
    uint32 const now = uint32(GameTime::GetGameTime().count());
    {
        std::unique_lock lock(_lock);
        if (character.IsEmpty())
            _guildValues[ownerAccount][uint16(id)] = value;
        else
            _characterValues[character][uint16(id)] = value;
    }
    if (character.IsEmpty())
        CharacterDatabase.Execute("REPLACE INTO ga_guild_setting (owner_account, setting_key, value, updated_at) "
            "VALUES ({}, '{}', {}, {})", ownerAccount, def->key, value, now);
    else
        CharacterDatabase.Execute("REPLACE INTO ga_character_setting (character_guid, owner_account, setting_key, value, "
            "updated_at) VALUES ({}, {}, '{}', {}, {})", character.GetCounter(), ownerAccount, def->key, value, now);
}

void BotSettings::Clear(uint32 ownerAccount, ObjectGuid character, Setting id)
{
    SettingDef const* def = Find(id);
    if (!def)
        return;
    {
        std::unique_lock lock(_lock);
        if (character.IsEmpty())
        {
            auto itr = _guildValues.find(ownerAccount);
            if (itr != _guildValues.end())
                itr->second.erase(uint16(id));
        }
        else
        {
            auto itr = _characterValues.find(character);
            if (itr != _characterValues.end())
                itr->second.erase(uint16(id));
        }
    }
    if (character.IsEmpty())
        CharacterDatabase.Execute("DELETE FROM ga_guild_setting WHERE owner_account = {} AND setting_key = '{}'",
            ownerAccount, def->key);
    else
        CharacterDatabase.Execute("DELETE FROM ga_character_setting WHERE character_guid = {} AND setting_key = '{}'",
            character.GetCounter(), def->key);
}

void BotSettings::ClearAll(uint32 ownerAccount, ObjectGuid character)
{
    {
        std::unique_lock lock(_lock);
        if (character.IsEmpty())
            _guildValues.erase(ownerAccount);
        else
            _characterValues.erase(character);
    }
    if (character.IsEmpty())
        CharacterDatabase.Execute("DELETE FROM ga_guild_setting WHERE owner_account = {}", ownerAccount);
    else
        CharacterDatabase.Execute("DELETE FROM ga_character_setting WHERE character_guid = {}", character.GetCounter());
}

// ---------------------------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------------------------

std::string BotSettings::Format(SettingDef const& def, int32 value)
{
    switch (def.type)
    {
        case SettingType::Bool:
            return value ? "on" : "off";
        case SettingType::Enum:
        {
            auto const options = Options(def);
            if (value >= 0 && std::size_t(value) < options.size())
                return options[std::size_t(value)].second;
            return std::to_string(value);
        }
        case SettingType::Money:
        {
            uint32 const copper = uint32(std::max(value, 0));
            std::string text;
            if (copper >= 10000)
                text += Acore::StringFormat("{}g", copper / 10000);
            if (copper % 10000 >= 100)
                text += Acore::StringFormat("{}{}s", text.empty() ? "" : " ", (copper % 10000) / 100);
            if (copper % 100 || copper == 0)
                text += Acore::StringFormat("{}{}c", text.empty() ? "" : " ", copper % 100);
            return text;
        }
        case SettingType::Decimal:
        {
            int32 const scale = std::max(def.scale, 1);
            int const decimals = scale >= 100 ? 2 : (scale >= 10 ? 1 : 0);
            return Acore::StringFormat("{:.{}f}", double(value) / double(scale), decimals);
        }
        case SettingType::Int:
        default:
        {
            std::string const unit = def.unit ? def.unit : "";
            if (unit.empty())
                return std::to_string(value);
            if (unit == "%")
                return std::to_string(value) + "%";
            return std::to_string(value) + " " + unit;
        }
    }
}

bool BotSettings::Parse(SettingDef const& def, std::string const& input, int32& value)
{
    std::string const text = Lower(Trim(input));
    if (text.empty())
        return false;

    switch (def.type)
    {
        case SettingType::Bool:
            if (text == "1" || text == "on" || text == "yes" || text == "true")
                value = 1;
            else if (text == "0" || text == "off" || text == "no" || text == "false")
                value = 0;
            else
                return false;
            return true;
        case SettingType::Enum:
        {
            auto const options = Options(def);
            for (std::size_t i = 0; i < options.size(); ++i)
                if (Lower(options[i].first) == text || Lower(options[i].second) == text)
                {
                    value = int32(i);
                    return true;
                }
            int64 index = 0;
            if (!ParseInteger(text, index))
                return false;
            value = int32(index);
            return true;
        }
        case SettingType::Money:
        {
            // "1g 20s 5c", "120s", "500" (copper)
            int64 total = 0;
            bool any = false;
            std::string number;
            for (std::size_t i = 0; i <= text.size(); ++i)
            {
                char const c = i < text.size() ? text[i] : '\0';
                if (std::isdigit(static_cast<unsigned char>(c)))
                {
                    number += c;
                    continue;
                }
                if (c == ' ' || c == '\0' || c == 'g' || c == 's' || c == 'c')
                {
                    if (!number.empty())
                    {
                        int64 const amount = std::atoll(number.c_str());
                        total += c == 'g' ? amount * 10000 : (c == 's' ? amount * 100 : amount);
                        number.clear();
                        any = true;
                    }
                    else if (c == 'g' || c == 's' || c == 'c')
                        return false;
                    continue;
                }
                return false;
            }
            if (!any || total > std::numeric_limits<int32>::max())
                return false;
            value = int32(total);
            return true;
        }
        case SettingType::Decimal:
        {
            char* end = nullptr;
            double const number = std::strtod(text.c_str(), &end);
            if (!end || *end != '\0')
                return false;
            value = int32(std::lround(number * double(std::max(def.scale, 1))));
            return true;
        }
        case SettingType::Int:
        default:
        {
            std::string digits = text;
            std::string const unit = def.unit ? Lower(def.unit) : "";
            if (!unit.empty() && digits.size() > unit.size() && digits.compare(digits.size() - unit.size(), unit.size(), unit) == 0)
                digits = Trim(digits.substr(0, digits.size() - unit.size()));
            int64 number = 0;
            if (!ParseInteger(digits, number) || number < std::numeric_limits<int32>::min() || number > std::numeric_limits<int32>::max())
                return false;
            value = int32(number);
            return true;
        }
    }
}
