/*
 * BotSettings: behaviour settings the guild leader changes in game (the owner, 2026-11-12: [quote left out]; docs/bot-settings.md, the design log section 7 "Bot
 * behaviour settings").
 *
 * A bot setting is a conf key of the module the leader may override. Its key is the conf key without "GuildAdventure."
 * (Rest.BelowHealth <-> GuildAdventure.Rest.BelowHealth), the conf value is the default for every guild. The leader sets
 * a value for the whole guild and, where the setting allows it (perCharacter), for one guild character (its leader or a
 * roster bot). The value that counts for a character: its own value, else its guild's value, else the conf default.
 *
 * Reading (any thread; two hash lookups under a shared lock):
 *
 *     #include "BotSettings.h"
 *     uint32 below = sBotSettings.Get(bot, GuildAdventure::Setting::RestBelowHealth);
 *     if (sBotSettings.IsOn(bot, GuildAdventure::Setting::RestEnable)) ...
 *     float margin = sBotSettings.GetFloat(bot, GuildAdventure::Setting::EscapeMargin);  // value / scale
 *
 * A character that is in no adventure guild (or nullptr) gets the conf default.
 *
 * Adding a setting: an id in the Setting enum (never reuse one; ids are grouped by category and go over the wire), a
 * row in the registry in BotSettings.cpp (key, category, type, range, per character or guild-wide, the conf field it
 * defaults to, the label and help the addon shows), and the call site reads sBotSettings instead of GetSettings(). The
 * addon learns the new setting from the server (SETDEFS, docs/protocol.md section 7.19); nothing else changes.
 *
 * Values are int32: bool 0/1, an enum as the index of its option, money in copper, a percentage 0-100, a decimal
 * multiplied by its scale (Escape.Margin 1.25 = 125). They are stored by key in ga_guild_setting and
 * ga_character_setting (characters database) and written through on every change (world thread).
 */

#ifndef MOD_GUILD_ADVENTURE_BOT_SETTINGS_H
#define MOD_GUILD_ADVENTURE_BOT_SETTINGS_H

#include "Define.h"
#include "GuildAdventureSettings.h"
#include "ObjectGuid.h"
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

namespace GuildAdventure
{
    // Wire ids (SETDEF^id, SETVAL^v, SETTING_SET^v). Grouped by category in hundreds; never reuse an id.
    enum class Setting : uint16
    {
        // Combat and rest (100s)
        RestEnable = 101,
        RestBelowHealth = 102,
        RestBelowMana = 103,
        RestUntilHealth = 104,
        RestUntilMana = 105,
        RestMaxSeconds = 106,
        RestSaferPlaceYards = 107,
        RestAirAfterSeconds = 108,
        RestBeforeWalk = 109,
        PullCareful = 131,
        FightPotionBelowHealth = 132,
        FightPotionBelowMana = 133,
        FightConsecrationMinFoes = 134,
        FightInterrupt = 135,
        FightTemplatesByDefault = 136,
        EscapeEnable = 121,
        EscapeMargin = 122,
        EscapeMaxFoes = 123,
        EscapeStepAside = 124,
        EscapeMaxSeconds = 125,
        // Danger and deaths (200s)
        RiskEnable = 201,
        RiskMaxFoeLevelAbove = 202,
        // 203 retired (Risk.MaxSpotDanger)
        RiskMaxFightCostPercent = 209,
        RiskCautiousDeaths = 204,
        RiskCautiousWindowMinutes = 205,
        RiskCautiousMinutes = 206,
        RiskRescueDeaths = 207,
        RiskRescueWindowMinutes = 208,
        // Questing and travel (300s)
        QuestsEscorts = 301,
        QuestsEvents = 302,
        AutoAdvanceDefault = 303,
        RoadsEnable = 304,
        RoadsMinDistance = 305,
        WalksMaxLevelsAbove = 306,
        WalksPostponeMinutes = 307,
        WalksPlaceCheck = 308,
        QuestsItemsFromGuild = 309,
        QuestsAbandonGrey = 310,
        SquadsQuestPartners = 311,
        TravelHearthstone = 312,
        SquadsCatchUpFallback = 313,
        // Vendors and provisions (400s)
        ProvisionsMinMoney = 401,
        ProvisionsBudgetPercent = 402,
        ProvisionsRestockBelow = 403,
        StorageSupplyProvisions = 404,
        VendorTrips = 405,
        ProvisionsMinLevel = 406,
        ProvisionsStack = 407,
        ProvisionsHubYards = 408,
        ProvisionsTripWhenLow = 409,
        ProvisionsTripWhenOut = 410,
        ProvisionsTripWhenOutYards = 411,
        AmmoHunterStock = 412,
        AmmoHunterStockBelow20 = 413,
        AmmoHunterStockBelow5 = 414,
        AmmoRestockBelow = 415,
        AmmoOthersStacks = 416,
        AmmoTopUpYards = 417,
        StorageSupplyPotions = 418,
        StorageSupplyBandages = 419,
        StorageSupplyScrolls = 420,
        StorageSupplyElixirs = 421,
        ProvisionsConjureBelow = 422,
        // Gear and the guild's gold (500s)
        StorageSupplyGear = 501,
        GearBuyEnable = 502,
        // 503-505 retired (Gear.Buy.Order, .WeaponGap, .ArmorGap)
        GearFillEmptySlots = 506,
        GearBuyNearYards = 507,
        GearWarnGap = 508,
        DungeonRolesWearForLevelling = 509,
        // 521, 522 retired (Budget.Percent, .Minutes)
        BudgetItemPercent = 524,
        BudgetBotAllowance = 525,
        BudgetMemberPercent = 523,
        GuildTaxPercent = 526,
        UnlocksPayFrom = 527,
        GuildTaxHandovers = 528,
        // Class and training (600s)
        AutoLearnClassSpells = 601,
        ClassAbilitiesEnable = 602,
        ClassAbilitiesHunterPet = 603,
        ClassAbilitiesPetTalents = 604,
        PoisonsEnable = 605,
        PoisonsStock = 606,
        PoisonsPair = 607,
        ClassAbilitiesRuneforging = 608,
        DeathKnightsSkipStart = 609,
        // Professions (900s)
        ProfessionsLearnInPlace = 901,
        ProfessionsGatherDetour = 902,
        ProfessionsCraftAtHub = 903,
        ProfessionsCraftBatch = 904,
        ProfessionsCraftGear = 905,
        ProfessionsCraftOrders = 906,
        ProfessionsDisenchantForOrders = 907,
        ProfessionsCatchUpGathering = 908,
        ProfessionsDisenchantSurplus = 909,
        ProfessionsCatchUpMinGap = 910,
        ProfessionsCookStock = 911,
        ProfessionsCookMinutes = 912,
        ProfessionsBuyRecipes = 913,
        ProfessionsRecipeMaxPrice = 914,
        ProfessionsRecipeYards = 915,
        ProfessionsFocusBehind = 916,
        ProfessionsGreenMinChance = 917,
        ProfessionsAmmoStock = 918,
        ProfessionsSkillCraftStock = 919,
        ProfessionsConsumableStock = 920,
        ProfessionsEnchantScrolls = 921,
        ProfessionsProcessAbove = 922,
        ProfessionsProcessStock = 923,
        ProfessionsEnchantMinItemLevel = 924,
        // Guild chat (700s)
        BotGuildChatMinSeconds = 701,
        BotGuildChatMaxSeconds = 702,
        // Grinding (800s)
        GrindEnable = 801,
        GrindLevelsBelow = 802,
        GrindLevelsAbove = 803,
        GrindMaxDistance = 804,
        // Loot (1000s)
        LootSellJunk = 1001,
        LootSellJunkQuality = 1002,
        // 1003 retired (LootFilter.KeepWornGrey)
        LootKeepWornGear = 1004,
        StorageUnwornBound = 1005,
        LootOpenContainers = 1006,
        StorageOpenContainers = 1007,
        StorageLockboxMinutes = 1008,
        StorageReserveForDungeons = 1009,
        ShoppingEnable = 1010,
        ShoppingMaxPerVisit = 1011,
        ShoppingMaxPerDay = 1012,
        ShoppingFromPurse = 1013,
        LootWornGear = 1014,
        LootSpareQuestItems = 1015,
        StorageDarkmoonCards = 1016,
        StorageReputationTurnIns = 1017,
        AuctionSellUnsold = 1018,
        // Farm orders (1100s)
        FarmLevelsBelow = 1101,
        FarmGroundRadius = 1102,
        FarmMinSpawns = 1103,
        FarmMaxDistance = 1104,
        // Dungeon runs (1200s, package PW)
        DungeonsEngageMargin = 1201,
        DungeonsBigPullAt = 1202,
        DungeonsGateHealth = 1203,
        DungeonsGateHealerMana = 1204,
        DungeonsGateDamageMana = 1205,
    };

    enum class SettingType : uint8
    {
        Bool,     // 0 / 1
        Int,      // a count, a percentage, seconds, yards ... (unit)
        Enum,     // the index of one of `options`
        Money,    // copper
        Decimal,  // value / scale
    };

    char const* ToToken(SettingType type);

    struct SettingDef
    {
        Setting id;
        char const* key;           // the conf key without "GuildAdventure."
        char const* category;      // SettingCategory::key
        SettingType type;
        int32 min;
        int32 max;
        int32 step;                // the addon's +/- step (1 if 0)
        int32 scale;               // Decimal: value / scale is shown (1 otherwise)
        char const* unit;          // "%", "s", "min", "yd", "levels" ... ("" = none)
        bool perCharacter;         // a guild character may have its own value
        bool advanced;             // shown under "Show all options" only
        char const* options;       // Enum: "token=Label;token=Label" (index = value)
        int32 (*fromConf)(Settings const&);  // the conf default
        void (*toSettings)(Settings&, int32); // writes a value into the conf field (BotSettings::For)
        char const* label;         // English, shown by the addon and the commands
        char const* help;          // one or two sentences
    };

    struct SettingCategory
    {
        char const* key;
        char const* label;
        char const* help;
    };

    // Values of one scope (a guild or a character), by setting id.
    using SettingValues = std::map<uint16, int32>;
}

class BotSettings
{
public:
    static BotSettings& Instance();

    // --- registry (static, any thread) ---
    static std::vector<GuildAdventure::SettingDef> const& Defs();
    static std::vector<GuildAdventure::SettingCategory> const& Categories();
    static GuildAdventure::SettingDef const* Find(GuildAdventure::Setting id);
    static GuildAdventure::SettingDef const* Find(uint32 id);
    static GuildAdventure::SettingDef const* Find(std::string const& key);   // case-insensitive, also with "GuildAdventure."
    // The options of an enum setting: token and label, in index order.
    static std::vector<std::pair<std::string, std::string>> Options(GuildAdventure::SettingDef const& def);

    // --- startup and the world update (world thread) ---
    void Load();
    // Rebuilds the member index every few seconds; true when the schema (the defaults after a config reload) changed.
    bool Update(uint32 diff);

    // --- reads, any thread ---
    int32 Get(ObjectGuid character, GuildAdventure::Setting id) const;
    int32 Get(Player const* character, GuildAdventure::Setting id) const;
    bool IsOn(ObjectGuid character, GuildAdventure::Setting id) const { return Get(character, id) != 0; }
    bool IsOn(Player const* character, GuildAdventure::Setting id) const { return Get(character, id) != 0; }
    float GetFloat(ObjectGuid character, GuildAdventure::Setting id) const;
    float GetFloat(Player const* character, GuildAdventure::Setting id) const;
    // The whole conf settings struct as it counts for one guild character: every bot setting replaced by the value that
    // counts for it, the rest as in the conf. For code that reads many fields: Settings const s = sBotSettings.For(bot);.
    GuildAdventure::Settings For(ObjectGuid character) const;
    GuildAdventure::Settings For(Player const* character) const;
    // What a guild's characters get when they have no own value (the guild's value or the conf default).
    int32 GetForGuild(uint32 ownerAccount, GuildAdventure::Setting id) const;
    // The conf default.
    static int32 Default(GuildAdventure::SettingDef const& def);
    static int32 Default(GuildAdventure::Setting id);

    GuildAdventure::SettingValues GuildValues(uint32 ownerAccount) const;
    GuildAdventure::SettingValues CharacterValues(ObjectGuid character) const;
    // Guild characters with values of their own.
    std::vector<ObjectGuid> CharactersWithValues(uint32 ownerAccount) const;

    // --- writes (world thread) ---
    // Checks a value: range, per character or guild-wide. Error codes: UNKNOWN, RANGE, GUILD_ONLY.
    static bool Check(GuildAdventure::SettingDef const* def, bool forCharacter, int64 value, std::string& code,
        std::string& text);
    // character empty = the guild's value. A value equal to what the scope would get anyway is stored all the same
    // (the leader asked for it; it stays when the default changes).
    void Set(uint32 ownerAccount, ObjectGuid character, GuildAdventure::Setting id, int32 value);
    void Clear(uint32 ownerAccount, ObjectGuid character, GuildAdventure::Setting id);
    // Every value of the scope (character empty: the guild's own values, not its characters').
    void ClearAll(uint32 ownerAccount, ObjectGuid character);

    // --- text (commands, the activity log) ---
    static std::string Format(GuildAdventure::SettingDef const& def, int32 value);
    // "65", "65%", "on"/"off", "1g 20s 5c", "1.25", an option token or label.
    static bool Parse(GuildAdventure::SettingDef const& def, std::string const& text, int32& value);

    // A hash of everything the addon is sent about the settings (SETDEFS^sv): it changes with the registry and with
    // the conf defaults.
    uint32 GetSchemaVersion() const { return _schemaVersion; }

private:
    BotSettings() = default;

    void RebuildIndex();
    uint32 ComputeSchemaVersion() const;
    uint32 OwnerOf(ObjectGuid character) const;  // caller holds _lock

    mutable std::shared_mutex _lock;
    std::unordered_map<uint32, GuildAdventure::SettingValues> _guildValues;        // owner -> values
    std::unordered_map<ObjectGuid, GuildAdventure::SettingValues> _characterValues; // character -> values
    std::unordered_map<ObjectGuid, uint32> _owners;                                 // guild character -> owner
    uint32 _indexTimer = 0;
    uint32 _schemaTimer = 0;
    uint32 _schemaVersion = 0;
};

#define sBotSettings BotSettings::Instance()

#endif
