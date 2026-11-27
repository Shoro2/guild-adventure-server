/*
 * mod-guild-adventure — guild management game on top of mod-playerbots.
 * See the design log in the repository root for the full design.
 */

void AddGuildAdventureWorldScripts();
void AddGuildAdventureControlSwapScripts();
void AddGuildAdventureCommandScripts();
void AddGuildAdventureRosterScripts();
void AddGuildAdventureManageCommandScripts();
void AddGuildAdventureAchievementScripts();
void AddGuildAdventurePerkScripts();
void AddGuildAdventureStorageScripts();
void AddGuildAdventureMilestoneScripts();
void AddGuildAdventureChatScripts();
void AddGuildAdventureObjectUpdateScripts();
void AddGuildAdventureQuestScripts();
void AddGuildAdventureGroundScripts();
void AddGuildAdventureQuestScanScripts();
void AddGuildAdventureSettingsScripts();
void AddGuildAdventureReadinessScripts();
void AddGuildAdventureCraftOrderScripts();
void AddGuildAdventureRoleScripts();
void AddGuildAdventureAmmoScripts();
void AddGuildAdventurePoisonsScripts();
void AddGuildAdventurePerfScripts();
void AddGuildAdventureRaidScripts();
void AddGuildAdventureFightProfileScripts();

// Entry point invoked by the AzerothCore module loader.
// The function name is the module folder name with '-' replaced by '_'.
void Addmod_guild_adventureScripts()
{
    AddGuildAdventureWorldScripts();
    AddGuildAdventureControlSwapScripts();
    AddGuildAdventureCommandScripts();
    // Guild management (roster, progression, offline window, GADV protocol). Registered after the take-control
    // scripts, so its world update runs after the guild bot holder's.
    AddGuildAdventureRosterScripts();
    AddGuildAdventureManageCommandScripts();
    // Guild-wide achievements; after the roster scripts, whose startup loads the guilds and whose login handling founds
    // a new leader's guild in the same tick.
    AddGuildAdventureAchievementScripts();
    // Guild storage, loot filter, auto loot, crafting from the storage; after the roster scripts, whose startup loads
    // the guilds.
    AddGuildAdventureStorageScripts();
    // Tiered guild achievements; after the roster scripts, whose startup loads the guilds.
    AddGuildAdventureMilestoneScripts();
    // Guild chat throttle for AI-played guild characters.
    AddGuildAdventureChatScripts();
    // Object updates for characters the AI plays on a client session (watch mode): stands in for playerbots' filter.
    AddGuildAdventureObjectUpdateScripts();
    // Guild perks; after the achievement and storage scripts, whose startup loads the achievements their points come
    // from.
    AddGuildAdventurePerkScripts();
    // Quests AI-played characters leave alone (CanTakeQuest veto, core patch CP-9).
    AddGuildAdventureQuestScripts();
    // AI-played characters stay on the ground (point movement check, core patch CP-10).
    AddGuildAdventureGroundScripts();
    // .ga admin questscan (quest interactions, docs/orders.md).
    AddGuildAdventureQuestScanScripts();
    AddGuildAdventureSettingsScripts();
    // Dungeon readiness of guild characters (.ga readiness; package K5).
    AddGuildAdventureReadinessScripts();
    // Craft orders of the guild (CraftOrders.h).
    AddGuildAdventureCraftOrderScripts();
    // Dungeon roles of the bots: role, dungeon set, role store (DungeonRoles.h).
    AddGuildAdventureRoleScripts();
    // Ammunition: no purchase beyond a character's stock (Ammunition.h).
    AddGuildAdventureAmmoScripts();
    AddGuildAdventurePoisonsScripts();
    // The planner's counters (.ga perf, docs/performance.md PF-0).
    AddGuildAdventurePerfScripts();
    // Raid mechanics the core leaves out (Poisonous Blood of Hakkar's Sons; OrderDungeons.cpp).
    AddGuildAdventureRaidScripts();
    // Fight profiles: the bots' combat logic as profiles (FightProfiles.h, docs/fight-profiles.md).
    AddGuildAdventureFightProfileScripts();
}
