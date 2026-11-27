/*
 * DungeonRoles: the dungeon role of a guild bot and its dungeon set (docs/dungeon-roles.md).
 *
 * The owner (2026-11-13): every bot levels as a damage dealer with the gear and talents for it. A dungeon group needs a
 * tank and a healer. [quote left out].
 *
 *   - Role: the guild leader gives a bot the dungeon role tank or healer (only what its class can fill: a talent
 *     template of the class with that role exists). No role: the bot goes into a dungeon as it levels.
 *   - Talents: a bot with a role has two talent specs (the game's dual spec, free and from level 10): the first is
 *     its levelling template, the second the template of its role. The second is active during a dungeon run only.
 *   - Dungeon set: while it levels, a bot with a role keeps equipment that is better for its role than what it wears
 *     and than what it has collected for that place: loot, quest rewards, pieces of the guild storage. Such a piece
 *     is rated by the role (stamina, armour and avoidance for a tank; intellect, spirit and healing for a healer),
 *     not by the talents the bot levels with.
 *   - Role store: the pieces of the dungeon set lie in a store of the bot's own, outside its bags (ga_bot_role_gear).
 *     For a dungeon run the bot puts the dungeon set on and its levelling pieces from those places into the store;
 *     after the run it changes back. A place without a piece of the dungeon set keeps the levelling piece.
 *
 * Every move between bags, body and store is written in one database transaction with the character's inventory,
 * and leaves a line in the activity log.
 *
 * World thread only.
 */

#ifndef MOD_GUILD_ADVENTURE_DUNGEON_ROLES_H
#define MOD_GUILD_ADVENTURE_DUNGEON_ROLES_H

#include "ObjectGuid.h"
#include <string>
#include <unordered_map>
#include <vector>

class Item;
class Player;
class Quest;
struct ItemTemplate;

namespace GuildAdventure
{
    struct AdventureGuild;
    struct GuildBot;
    struct SpecTemplate;
    struct StorageItem;

    enum class DungeonRole : uint8
    {
        None = 0,
        Tank = 1,
        Healer = 2
    };

    // Protocol token (BOT^dr): "none", "tank", "heal".
    char const* ToToken(DungeonRole role);
    // For texts: "tank", "healer", "no role".
    char const* ToText(DungeonRole role);

    constexpr uint8 RolePieceDungeon = 1;   // a piece of the dungeon set
    constexpr uint8 RolePieceLevelling = 2; // a levelling piece put aside while the dungeon set is on
    // RolePiece::slot of a piece of the dungeon set that is soulbound and was never worn (the guild storage's
    // never-worn mark, StorageService::IsMarkedUnworn): when the set lets it go again, it gets the mark back and
    // can go into the guild storage for somebody else.
    constexpr uint8 RoleSlotUnworn = 254;

    // One piece in a bot's role store: what item_instance holds about an item.
    struct RolePiece
    {
        uint32 id = 0;
        uint8 kind = RolePieceDungeon;
        uint8 slot = 255;            // the equipment slot it was worn in (kind 2); 255 = none, 254 = never worn (kind 1)
        uint32 entry = 0;
        int32 randomProperty = 0;
        std::string enchantments;
        std::string charges;
        uint32 durability = 0;
        uint32 flags = 0;
        uint32 creator = 0;
        uint32 storedAt = 0;
    };

    struct BotRole
    {
        DungeonRole role = DungeonRole::None;
        uint32 templateId = 0;       // 0 = the default template of the class for the role
        bool worn = false;           // the dungeon set is on
        std::vector<RolePiece> store;

        // Not stored.
        uint32 nextStep = 0;         // getMSTime() of the next look at the bags
        uint64 storageSeen = 0;      // the guild storage it last looked through for its role
        uint8 storageLevel = 0;
        // While the dungeon set is on: the levelling pieces that stayed on (no piece of the dungeon set for their
        // place), by equipment slot. One that is taken off during the run is put on again after it.
        std::unordered_map<uint8, ObjectGuid> shared;
        // Test aid (".ga admin roleset <bot> on"): the dungeon set stays on without a dungeon run.
        bool held = false;
        // The gear level in the dungeon set, kept for a few seconds (the addon's records ask for it every two).
        mutable uint32 gearLevelAt = 0;
        mutable uint16 gearLevel = 0;
    };
}

class RoleService
{
public:
    static RoleService& instance();

    // ga_bot_role, ga_bot_role_gear (characters). Startup and ".ga reload".
    void Load();

    // --- the role ---
    // The roles a class can fill for this guild: those a talent template of the class has (built-in or the guild's).
    [[nodiscard]] std::vector<GuildAdventure::DungeonRole> RolesOf(uint32 owner, uint8 classId) const;
    [[nodiscard]] GuildAdventure::DungeonRole GetRole(ObjectGuid bot) const;
    // The role a bot fills in a dungeon group: its dungeon role, else the role of the talent template it levels with.
    [[nodiscard]] GuildAdventure::DungeonRole EffectiveRole(GuildAdventure::GuildBot const& bot) const;
    // What is stored as the role's talent template (0 = the default), and the template that is used.
    [[nodiscard]] uint32 GetTemplateId(ObjectGuid bot) const;
    [[nodiscard]] GuildAdventure::SpecTemplate const* RoleTemplate(uint32 owner, ObjectGuid bot, uint8 classId) const;
    // The guild leader's choice. templateId: 0 = the default of the class for the role, else a template of the class
    // with that role. A change of role empties the dungeon set into the bags (its pieces were chosen for the old one).
    bool SetRole(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot, GuildAdventure::DungeonRole role,
        uint32 templateId, std::string& error);
    // The bot left the guild.
    void Forget(ObjectGuid bot);

    [[nodiscard]] bool IsDungeonSetOn(ObjectGuid bot) const;
    [[nodiscard]] std::vector<GuildAdventure::RolePiece> const* GetStore(ObjectGuid bot) const;

    // --- what the role wants ---
    // True if that kind of item would improve the bot's dungeon set: it can wear it, and for the role it is better
    // than what the set has for that place (the collected piece, else the levelling piece worn there).
    [[nodiscard]] bool IsRoleUpgrade(Player* bot, uint32 itemEntry, int32 randomProperty) const;
    // Loot filter, bag sweeps, sales: true if the item stays with the bot for its role (an upgrade of its dungeon
    // set, or a levelling piece that was taken off while the dungeon set is on).
    [[nodiscard]] bool Wants(Player* bot, Item const* item) const;
    // Quest rewards: the choice that improves the dungeon set most (-1: none does). The caller takes a levelling
    // upgrade first.
    [[nodiscard]] int32 RewardChoice(Player* bot, Quest const* quest) const;
    // The bag changed (loot the filter kept for the role): the next Step looks at once.
    void LookSoon(ObjectGuid bot);

    // --- the planner's steps ---
    // A living bot out of combat, every planner tick (cheap: it looks about once a minute). Moves role upgrades from
    // the bags into the role store, lets pieces go the set no longer needs, and puts the levelling set back on when
    // the dungeon set is on without a run (`onRun` false: after a restart in the middle of one).
    void Step(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot, Player* player, bool onRun);
    // Before a dungeon run goes in: the role's talents (the second spec) and the dungeon set. False while it cannot
    // be done now (in a fight, dead); true when it is on, or the bot has no role.
    bool EnterDungeonMode(GuildAdventure::AdventureGuild& guild, GuildAdventure::GuildBot const& bot, Player* player);
    // Test aid: the dungeon set stays on although no dungeon run is going (until LeaveDungeonMode).
    void Hold(ObjectGuid bot, bool held);
    // After the run: the levelling talents and the levelling set. False while it cannot be done now.
    bool LeaveDungeonMode(GuildAdventure::GuildBot const& bot, Player* player);
    // Guild storage (StorageSupply): takes pieces that improve the dungeon set and that no guild character in the
    // world would wear for levelling, into the role store. Returns the names for the log ("" = nothing).
    std::string TakeFromStorage(Player* bot, uint32 owner, std::vector<GuildAdventure::StorageItem> const& stock);

    // --- readiness ---
    // The gear level of the bot in its dungeon set (Readiness.h's gear level over the set it would wear in a run).
    [[nodiscard]] uint16 RoleGearLevel(Player const* bot) const;
    // "id:percent,..." for the dungeons in the bot's reach, with the dungeon set's gear level ("" without a role).
    [[nodiscard]] std::string ReadinessSummary(Player const* bot) const;
    // How many pieces of the dungeon set the bot has (collected, or on while it wears the set).
    [[nodiscard]] uint32 CountDungeonPieces(ObjectGuid bot) const;

    // Lines for ".ga role <character>".
    [[nodiscard]] std::vector<std::string> Describe(GuildAdventure::AdventureGuild const& guild, GuildAdventure::GuildBot const& bot,
        Player* player) const;

private:
    RoleService() = default;

    GuildAdventure::BotRole* Find(ObjectGuid bot);
    GuildAdventure::BotRole const* Find(ObjectGuid bot) const;

    // Writes the bot's role row, its whole store and (when given) the character in one transaction: its inventory
    // and money, or with `whole` everything (the talents and the active talent spec too: a change of set).
    void Save(ObjectGuid bot, GuildAdventure::BotRole const& state, Player* player, bool whole = false) const;

    // Bags or body -> store. The item is gone afterwards.
    void Stash(Player* player, GuildAdventure::BotRole& state, Item* item, uint8 kind, uint8 slot);
    // Store -> body (slot != 255) or bags. False when it could not be put there (the piece stays in the store).
    // `placed`: the item where it went.
    bool Unstash(Player* player, GuildAdventure::BotRole& state, std::size_t index, uint8 slot, Item** placed = nullptr);
    // While it levels: the pieces of the dungeon set that are better for the levelling talents than what the bot
    // wears go on (the owner, 2026-11-14). What they replace goes into the bags. Returns the pieces for the log.
    std::string WearForLevelling(Player* player, GuildAdventure::BotRole& state);
    // Store -> bags, for pieces the set no longer needs. Returns the names.
    std::string Release(Player* player, GuildAdventure::BotRole& state, bool all);

    bool PutDungeonSetOn(Player* player, GuildAdventure::BotRole& state, std::string& moved);
    bool PutLevellingSetOn(Player* player, GuildAdventure::BotRole& state, std::string& moved);
    bool SwitchTalents(GuildAdventure::GuildBot const& bot, Player* player, GuildAdventure::SpecTemplate const* roleSpec, bool toRole);
    std::string Collect(Player* player, GuildAdventure::BotRole& state);

    std::unordered_map<ObjectGuid::LowType, GuildAdventure::BotRole> _roles;
    uint32 _nextPieceId = 1;
};

#define sRoleService RoleService::instance()

#endif
