/*
 * SpecService: talent templates per class, assigned to the guild bots by the guild leader (docs/specs.md, DQ4).
 *
 * A template is a list of talent points in the order they are spent (level 10 first). The built-in ones come from the
 * world table ga_spec_template (generated from the wowbot335 talent profiles); a guild leader can save the talents of a
 * character he plays as a template of his guild (ga_guild_spec_template). Every bot plays by one template: the one
 * assigned to it (ga_bot_spec), else the default of its class; "manual" means the module spends no points for it.
 *
 *   - Maintain: on login and after a level-up the bot spends its free talent points along its template. The first time
 *     the module sees a bot whose talents do not fit its template (mod-playerbots chose them before) they are reset once,
 *     for free.
 *   - Assign: the leader's choice; the bot's talents are reset for free and learned again by the new template.
 *   mod-playerbots picks its combat strategies (damage, tank, healer; the rotation) from the tree with the most points,
 *   so they are reset whenever that tree changes.
 *
 * World thread only.
 */

#ifndef MOD_GUILD_ADVENTURE_SPEC_SERVICE_H
#define MOD_GUILD_ADVENTURE_SPEC_SERVICE_H

#include "ObjectGuid.h"
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

namespace GuildAdventure
{
    // Values of ga_bot_spec.template_id besides template ids.
    constexpr uint32 SpecDefault = 0; // the default template of the class
    constexpr uint32 SpecManual = 1;  // the module spends no talent points for this bot
    // Templates of a guild (ga_guild_spec_template) have ids from here on.
    constexpr uint32 FirstGuildSpecId = 100000;

    enum class SpecPurpose : uint8
    {
        Leveling = 0,
        Group = 1,   // dungeon and raid
        Raid = 2,
        PvP = 3
    };

    struct SpecPoint
    {
        uint8 tab = 0;
        uint8 row = 0;
        uint8 col = 0;
    };

    struct SpecTemplate
    {
        uint32 id = 0;
        uint32 owner = 0;        // 0 = built-in, else the owner account of the guild it belongs to
        uint8 classId = 0;
        std::string name;
        uint8 purpose = 0;       // SpecPurpose
        uint8 role = 0;          // 0 damage, 1 tank, 2 healer
        uint8 tab = 0;           // the tree with most points at level 60
        bool isDefault = false;
        std::vector<SpecPoint> points;

        // Points per tree with the points a character of that level has: "13/0/38".
        std::string Summary(uint8 level) const;
    };

    class SpecService
    {
    public:
        static SpecService& instance();

        // Templates (world), guild templates and assignments (characters). Called at startup and by ".ga reload".
        void Load();

        SpecTemplate const* Get(uint32 templateId) const;
        SpecTemplate const* GetDefault(uint8 classId) const;
        // Built-in templates of the class and the guild's own, in id order.
        std::vector<SpecTemplate const*> ListFor(uint32 owner, uint8 classId) const;

        // What is stored for the bot (SpecDefault when nothing is).
        uint32 GetAssigned(ObjectGuid botGuid) const;
        // The template the bot plays by; nullptr = manual, or the class has none. On a dungeon run with a dungeon
        // role: the template of the role.
        SpecTemplate const* GetEffective(ObjectGuid botGuid, uint8 classId) const;
        // The template the bot levels with (what is assigned to it, else the default of its class), also during a run.
        SpecTemplate const* GetLevelling(ObjectGuid botGuid, uint8 classId) const;

        // The guild leader's choice. templateId: SpecDefault, SpecManual, a built-in template of the class or one of
        // the owner's guild. A bot in the world gets its talents reset for free and learned again at once, another
        // one when it next enters the world.
        bool Assign(uint32 owner, ObjectGuid botGuid, uint8 classId, uint32 templateId, Player* botInWorld, std::string& error);

        // Login and level-up of an AI-played bot. Returns what was done for the log ("" = nothing).
        std::string Maintain(Player* bot);
        // Dungeon roles (DungeonRoles.h): while a bot's second talent spec is active (a dungeon run) it plays by the
        // template of its role instead of the one it levels with. Not stored: the role service sets it again.
        void SetRunTemplate(ObjectGuid botGuid, uint32 templateId);
        void ClearRunTemplate(ObjectGuid botGuid);
        [[nodiscard]] bool HasRunTemplate(ObjectGuid botGuid) const;
        // Makes the bot's active talent spec follow the template now: its free points are spent along it, and
        // talents that do not fit it are reset first (free). Returns what was done for the log ("" = nothing).
        std::string ApplyTemplate(Player* bot, SpecTemplate const& spec);
        // True while a template was assigned and the bot's talents are not reset and learned again yet (it was in a
        // fight, dead or not in the world): Maintain does it at the next quiet moment.
        bool HasPendingReset(ObjectGuid botGuid) const;

        // Saves the talents the character has now as a template of the owner's guild (the order: tree by tree, the tree
        // with most points first, row by row). Returns its id, 0 with an error.
        uint32 SaveFrom(uint32 owner, Player* player, std::string const& name, uint8 purpose, std::string& error);
        bool Delete(uint32 owner, uint32 templateId, std::string& error);

        // Talent points per tree the character has now: "5/0/12".
        static std::string CurrentSummary(Player* player);

    private:
        SpecService() = default;

        std::string Apply(Player* bot, SpecTemplate const& spec, bool reset);

        std::unordered_map<uint32, SpecTemplate> _templates;
        std::unordered_map<uint8, uint32> _defaults;               // class -> template id
        std::unordered_map<ObjectGuid::LowType, uint32> _assigned; // bot -> ga_bot_spec.template_id
        std::unordered_map<ObjectGuid::LowType, bool> _pendingReset; // assigned while the bot was offline
        std::unordered_map<ObjectGuid::LowType, uint32> _runTemplate; // bot -> template of its role during a dungeon run
        uint32 _nextGuildId = FirstGuildSpecId;
    };
}

#define sSpecService GuildAdventure::SpecService::instance()

#endif
