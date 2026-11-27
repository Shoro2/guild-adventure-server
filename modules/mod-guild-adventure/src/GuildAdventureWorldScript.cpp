/*
 * World-level hooks: config load, startup, the per-tick driver of the guild bot holder and the control swap
 * service, and shutdown.
 *
 * WorldScript::OnUpdate runs on the world thread after the sessions and the maps were updated
 * (World::Update), which is the only place the module rebinds players between sessions (invariant I5,
 * docs/take-control.md §4.3).
 */

#include "ControlSwapService.h"
#include "GuildAdventureConfig.h"
#include "GuildBotHolder.h"
#include "Log.h"
#include "ScriptMgr.h"

class GuildAdventureWorldScript : public WorldScript
{
public:
    GuildAdventureWorldScript() : WorldScript("GuildAdventureWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_UPDATE,
        WORLDHOOK_ON_SHUTDOWN
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        GuildAdventure::LoadModuleConfig();
    }

    void OnStartup() override
    {
        if (GuildAdventure::IsModuleEnabled())
        {
            LOG_INFO("module", "mod-guild-adventure: loaded (guild bot holder, take control via Path R).");
            GuildAdventure::CheckPlayerbotsSettings();
        }
        else
            LOG_INFO("module", "mod-guild-adventure: disabled (GuildAdventure.Enable = 0).");
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        // Bots that are already in the world keep being driven even if the module was disabled by a config
        // reload; only new requests are refused (commands and hooks check IsModuleEnabled).
        sGuildBotHolder.Update();
        sControlSwapService.Update();
    }

    void OnShutdown() override
    {
        // Runs after the world loop stopped and before the client sessions are kicked (worldserver Main.cpp):
        // the AI-driven bots log out through their home sessions; controlled characters log out with their
        // client sessions afterwards, as normal players.
        sControlSwapService.OnShutdown();
        sGuildBotHolder.LogoutAll();
    }
};

void AddGuildAdventureWorldScripts()
{
    new GuildAdventureWorldScript();
}
