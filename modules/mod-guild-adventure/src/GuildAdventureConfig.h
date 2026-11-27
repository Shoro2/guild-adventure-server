/*
 * Module-wide configuration (mod_guild_adventure.conf). Read on the world thread in WorldScript::OnAfterConfigLoad,
 * so a ".reload config" applies it too.
 */

#ifndef MOD_GUILD_ADVENTURE_CONFIG_H
#define MOD_GUILD_ADVENTURE_CONFIG_H

namespace GuildAdventure
{
    void LoadModuleConfig();
    [[nodiscard]] bool IsModuleEnabled();

    // Logs a warning for every mod-playerbots setting that breaks take-control (docs/core-patches.md, "Settings
    // the patches and the module rely on").
    void CheckPlayerbotsSettings();
}

#endif
