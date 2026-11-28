/*
 * PerkService: guild perks, permanent upgrades the guild buys with the points of its achievements (the owner, 2026-11-12:
 * [quote left out]; docs/guild-management.md "Guild perks").
 *
 * Points: the achievement points of the guild's achievements (AchievementService, STATE^achp), plus what a GM granted
 * (`.ga admin perk grant`, the ga_guild_perk row with perk id 0), minus what the guild spent on perks
 * (ga_guild_perk.spent). They are a balance of their own: renown, its level and its unlocks are not touched by buying.
 *
 * Perks: rows of ga_perk_template (world database: ranks, the cost of each rank, the value of each rank); the module
 * knows what each key does. A perk works for every character of the guild (the leader and its bots), at once for
 * those in the world and on login for the others.
 *
 * Effects and where they act:
 *   move_speed       +x % movement speed            core hook CP-8 OnPlayerUpdateSpeed
 *   experience       +x % experience                PlayerScript::OnPlayerGiveXP
 *   gold             +x % looted gold               PlayerScript::OnPlayerBeforeLootMoney (also auto loot)
 *   extra_material   x % chance of one more crafting material (trade goods) per loot   PlayerScript::OnPlayerLootItem
 *   kill_regen       x % health and mana over 10 s after a kill, 10 s cooldown   PlayerScript::OnPlayerCreatureKill
 *   no_reagents      class spells need no reagents  PLAYER_NO_REAGENT_COST_* fields (tradeskill spells still do)
 *   instant_flight   a flight from a flight master lands at once   world update
 *   all_flight_paths every flight path of the faction is known      on login and on buying
 *   materials_storage crafting materials the owner's own character loots go into the guild storage   LootService
 *   drop_chance      +x % drop chance of items up to rare   GlobalScript::OnItemRoll
 *   craft_speed      +x % crafting speed            core hook CP-8 OnPlayerSpellCastTime (tradeskill spells)
 *   gather_speed     +x % mining, herb gathering and skinning speed   core hook CP-8 OnPlayerSpellCastTime
 *   water_breathing  breathe under water            core hook CP-8 OnPlayerCheckWaterBreathing
 *   no_fall_damage   no fall damage                 core hook CP-8 OnPlayerFallDamage
 * Without core patch CP-8 the four core-hook perks are not offered (IsAvailable).
 *
 * Threads: buying, the ranks and the templates live on the world thread. The hooks fire on map threads too (XP, loot,
 * speed, cast times); they read a snapshot of every guild character's perk values, rebuilt on the world thread and
 * guarded by a shared mutex.
 */

#ifndef MOD_GUILD_ADVENTURE_PERK_SERVICE_H
#define MOD_GUILD_ADVENTURE_PERK_SERVICE_H

#include "Define.h"
#include "ObjectGuid.h"
#include <array>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <set>
#include <unordered_map>
#include <vector>

class Player;
class SpellInfo;
struct ItemTemplate;

namespace GuildAdventure
{
    struct AdventureGuild;

    enum class PerkKind : uint8
    {
        MoveSpeed,
        Experience,
        Gold,
        ExtraMaterial,
        KillRegen,
        NoReagents,
        InstantFlight,
        AllFlightPaths,
        MaterialsStorage,
        DropChance,
        CraftSpeed,
        GatherSpeed,
        WaterBreathing,
        NoFallDamage,
        SharedFlightPaths,
        Count
    };

    constexpr std::size_t PerkKindCount = std::size_t(PerkKind::Count);

    char const* ToKey(PerkKind kind);

    struct PerkTemplate
    {
        uint32 id = 0;
        PerkKind kind = PerkKind::Count;
        std::string key;
        std::string name;
        std::vector<uint32> costs;   // per rank
        std::vector<float> values;   // per rank (a single unlock: 1)
        std::string unit;            // "%" or ""
        std::string description;     // "{}" = the value
        uint8 MaxRank() const { return uint8(costs.size()); }
        float ValueAt(uint8 rank) const;
        std::string Describe(uint8 rank) const;
    };

    // What the perks of one guild character do (the hooks' snapshot).
    struct PerkEffects
    {
        std::array<float, PerkKindCount> value{};
        bool Has(PerkKind kind) const { return value[std::size_t(kind)] > 0.0f; }
        float Get(PerkKind kind) const { return value[std::size_t(kind)]; }
    };
}

class PerkService
{
public:
    static PerkService& Instance();

    // Startup (world thread): templates (world database) and the guilds' ranks (characters database).
    void LoadTemplates();
    void Load();
    // World thread, every tick: snapshot, regeneration, instant flights, the no-reagent fields.
    void Update(uint32 diff);

    // --- templates and guild state (world thread) ---
    std::vector<GuildAdventure::PerkTemplate> const& GetTemplates() const { return _templates; }
    GuildAdventure::PerkTemplate const* FindTemplate(uint32 id) const;
    GuildAdventure::PerkTemplate const* FindTemplate(std::string const& key) const;
    // Offered on this server: the four core-hook perks need core patch CP-8.
    static bool IsAvailable(GuildAdventure::PerkKind kind);
    uint8 GetRank(uint32 owner, uint32 perkId) const;
    uint32 GetEarned(GuildAdventure::AdventureGuild const& guild) const;  // achievement points + tiered achievement points
    uint32 GetSpent(uint32 owner) const;
    uint32 GetGranted(uint32 owner) const;                                // GM grants
    uint32 GetAvailable(GuildAdventure::AdventureGuild const& guild) const;
    // Buys the next rank. Error codes: UNKNOWN_PERK, NOT_AVAILABLE, MAX_RANK, NO_POINTS.
    // withGold: paid with gold instead of guild points (GuildAdventure.Perks.GoldPerPoint per point; NOT_ENOUGH_GOLD).
    bool Buy(GuildAdventure::AdventureGuild& guild, uint32 perkId, std::string& code, std::string& text, bool withGold = false);
    // The copper one guild point costs when a perk is bought with gold (STATE^gpgold); 0 = no gold purchase.
    uint64 GoldPerPoint() const;
    // Gives every point back and drops every rank and every GM grant (GM, tests).
    void Reset(GuildAdventure::AdventureGuild& guild);
    // GM: points on top of the achievements' (tests, rewards).
    void Grant(GuildAdventure::AdventureGuild& guild, uint32 points);

    // A guild character entered the world (hook queue): its perks take effect.
    void OnCharacterInWorld(ObjectGuid character);
    // A guild character killed a creature (hook queue): kill regeneration.
    void OnKill(ObjectGuid character);
    // A guild character looted an item (hook queue): the extra crafting material.
    void OnLooted(ObjectGuid character, uint32 itemEntry);

    // --- hooks, any thread (the snapshot) ---
    GuildAdventure::PerkEffects GetEffects(ObjectGuid character) const;
    float GetValue(ObjectGuid character, GuildAdventure::PerkKind kind) const;
    // A guild character discovered a flight path (world thread, queued by OnPlayerLearnTaxiNode): with the perk Shared
    // Maps every character of its guild in the world that is of its side learns it too (the others at their login).
    void OnTaxiNodeLearned(ObjectGuid character, uint32 node);
    bool Has(ObjectGuid character, GuildAdventure::PerkKind kind) const { return GetValue(character, kind) > 0.0f; }
    // Craft (tradeskill) and gathering spells (mining, herb gathering, skinning).
    static bool IsCraftSpell(SpellInfo const* spellInfo);
    static bool IsGatherSpell(SpellInfo const* spellInfo);
    static bool IsMaterial(ItemTemplate const* proto);

private:
    PerkService() = default;

    struct Regen
    {
        uint32 cooldownUntil = 0;  // getMSTime
        uint8 ticksLeft = 0;
        float pctPerTick = 0.0f;
    };

    void RebuildSnapshot();
    void Apply(Player* player, GuildAdventure::PerkEffects const& effects);
    void ApplyToGuild(GuildAdventure::AdventureGuild const& guild);
    void TickRegen(uint32 now);
    void TickFlights();
    void TickReagentFields();
    std::vector<ObjectGuid> Members(GuildAdventure::AdventureGuild const& guild) const;
    // Shared Maps: the flight paths the guild's characters know (read from their saved taximasks once per guild after a
    // start, then grown by every discovery), and the ones a character of the guild learns from it.
    std::set<uint32>& GuildNodes(GuildAdventure::AdventureGuild const& guild);
    uint32 ShareFlightPaths(GuildAdventure::AdventureGuild const& guild, Player* player);
    std::unordered_map<uint32, std::set<uint32>> _guildNodes; // owner -> taxi nodes

    std::vector<GuildAdventure::PerkTemplate> _templates;
    std::unordered_map<uint32, std::map<uint32, uint8>> _ranks;   // owner -> perk id -> rank
    std::unordered_map<uint32, uint32> _spent;                   // owner -> points spent
    std::unordered_map<uint32, uint32> _granted;                 // owner -> points a GM granted (perk id 0)

    mutable std::shared_mutex _snapshotLock;
    std::unordered_map<ObjectGuid, GuildAdventure::PerkEffects> _snapshot;

    std::unordered_map<ObjectGuid, Regen> _regen;
    std::unordered_map<ObjectGuid, bool> _reagentFieldsSet;
    uint32 _snapshotTimer = 0;
    uint32 _tickTimer = 0;
    uint32 _regenTimer = 0;
    bool _dirty = true;
};

#define sPerkService PerkService::Instance()

#endif
