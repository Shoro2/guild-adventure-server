/*
 * Bot behaviour settings: startup, the world update and the chat commands (BotSettings.h, docs/bot-settings.md).
 *
 *   .ga settings [character]                          what counts for a guild character (default: the character you
 *                                                     play), each setting with where its value comes from (GM; console
 *                                                     with a character)
 *   .ga setting <key> <value|default>                 your guild's value (GM, in game: the guild of your account)
 *   .ga setting <key> <value|default> <character>     that guild character's own value
 *   .ga admin setting <character> <key> <value|default>     the guild's value of that character's guild (console, SOAP)
 *   .ga admin botsetting <character> <key> <value|default>  that character's own value (console, SOAP)
 * The key "all" with "default" drops every value of the scope.
 */

#include "BotSettings.h"
#include "Chat.h"
#include "CommandScript.h"
#include "GadvProtocol.h"
#include "GuildAdventureConfig.h"
#include "GuildAdventureMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include <algorithm>

using namespace Acore::ChatCommands;
using namespace GuildAdventure;

namespace
{
    bool Enabled(ChatHandler* handler)
    {
        if (IsModuleEnabled())
            return true;
        handler->SendErrorMessage("Guild Adventure is disabled (GuildAdventure.Enable = 0).", false);
        return false;
    }

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    }

    bool IsDefaultWord(std::string const& text)
    {
        std::string const word = Lower(text);
        return word == "default" || word == "reset" || word == "-";
    }

    // Sets, clears or (key "all") clears every value of a scope, and tells the addon.
    bool Apply(ChatHandler* handler, AdventureGuild const& guild, ObjectGuid character, std::string const& characterName,
        std::string const& key, std::string const& valueText)
    {
        std::string const scope = character.IsEmpty() ? std::string("the guild") : characterName;
        if (Lower(key) == "all")
        {
            if (!IsDefaultWord(valueText))
            {
                handler->SendErrorMessage("\"all\" only takes \"default\": every value of the scope is dropped.", false);
                return false;
            }
            sBotSettings.ClearAll(guild.ownerAccount, character);
            sGadvProtocol.PushSettingValues(guild.ownerAccount, character);
            handler->PSendSysMessage("Every setting of {} is back to {}.", scope,
                character.IsEmpty() ? "the server's defaults" : "the guild's values");
            return true;
        }

        SettingDef const* def = BotSettings::Find(key);
        if (!def)
        {
            handler->SendErrorMessage(Acore::StringFormat("There is no setting \"{}\" (.ga settings lists them).", key), false);
            return false;
        }
        if (IsDefaultWord(valueText))
        {
            sBotSettings.Clear(guild.ownerAccount, character, def->id);
            sGadvProtocol.PushSettingValues(guild.ownerAccount, character);
            int32 const now = character.IsEmpty() ? sBotSettings.GetForGuild(guild.ownerAccount, def->id)
                : sBotSettings.Get(character, def->id);
            handler->PSendSysMessage("{} of {}: back to {} ({}).", def->key, scope,
                character.IsEmpty() ? "the default" : "the guild's value", BotSettings::Format(*def, now));
            return true;
        }

        int32 value = 0;
        if (!BotSettings::Parse(*def, valueText, value))
        {
            handler->SendErrorMessage(Acore::StringFormat("\"{}\" is no value for {} ({}).", valueText, def->key,
                ToToken(def->type)), false);
            return false;
        }
        std::string code;
        std::string text;
        if (!BotSettings::Check(def, !character.IsEmpty(), value, code, text))
        {
            handler->SendErrorMessage(text, false);
            return false;
        }
        sBotSettings.Set(guild.ownerAccount, character, def->id, value);
        sGadvProtocol.PushSettingValues(guild.ownerAccount, character);
        handler->PSendSysMessage("{} of {}: {} (default {}).", def->key, scope, BotSettings::Format(*def, value),
            BotSettings::Format(*def, BotSettings::Default(*def)));
        return true;
    }

    // A guild character by name: the guild and the character's guid.
    AdventureGuild* GuildOf(ChatHandler* handler, PlayerIdentifier const& target)
    {
        AdventureGuild* guild = sGuildAdventureMgr.FindGuildOfMember(target.GetGUID());
        if (!guild)
            handler->SendErrorMessage(Acore::StringFormat("{} is neither a guild leader nor a guild bot.", target.GetName()),
                false);
        return guild;
    }
}

class GuildAdventureSettingsCommandScript : public CommandScript
{
public:
    GuildAdventureSettingsCommandScript() : CommandScript("GuildAdventureSettingsCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable adminTable =
        {
            { "setting",    HandleAdminSetting,    SEC_ADMINISTRATOR, Console::Yes },
            { "botsetting", HandleAdminBotSetting, SEC_ADMINISTRATOR, Console::Yes },
        };
        static ChatCommandTable gaTable =
        {
            { "settings", HandleSettings, SEC_GAMEMASTER, Console::Yes },
            { "setting",  HandleSetting,  SEC_GAMEMASTER, Console::No },
            { "admin",    adminTable },
        };
        static ChatCommandTable commandTable =
        {
            { "ga", gaTable }
        };
        return commandTable;
    }

    static bool HandleSettings(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        if (!Enabled(handler))
            return false;
        if (!target)
            target = PlayerIdentifier::FromTargetOrSelf(handler);
        if (!target)
        {
            handler->SendErrorMessage("Name a guild character (.ga settings <character>).", false);
            return false;
        }
        AdventureGuild* guild = GuildOf(handler, *target);
        if (!guild)
            return false;

        ObjectGuid const character = target->GetGUID();
        SettingValues const own = sBotSettings.CharacterValues(character);
        SettingValues const guildValues = sBotSettings.GuildValues(guild->ownerAccount);
        handler->PSendSysMessage("Bot settings of {} ([own] = its own value, [guild] = the guild's, the rest are the "
            "server's defaults):", target->GetName());
        for (SettingCategory const& category : BotSettings::Categories())
        {
            handler->PSendSysMessage("{}:", category.label);
            for (SettingDef const& def : BotSettings::Defs())
            {
                if (std::string(def.category) != category.key)
                    continue;
                std::string source;
                if (def.perCharacter && own.count(uint16(def.id)))
                    source = " [own]";
                else if (guildValues.count(uint16(def.id)))
                    source = " [guild]";
                handler->PSendSysMessage("  {} = {}{}  ({}{})", def.key,
                    BotSettings::Format(def, sBotSettings.Get(character, def.id)), source, def.label,
                    def.perCharacter ? "" : ", guild only");
            }
        }
        return true;
    }

    static bool HandleSetting(ChatHandler* handler, std::string key, std::string value, Optional<PlayerIdentifier> target)
    {
        if (!Enabled(handler))
            return false;
        if (target)
        {
            AdventureGuild* guild = GuildOf(handler, *target);
            if (!guild)
                return false;
            return Apply(handler, *guild, target->GetGUID(), target->GetName(), key, value);
        }
        AdventureGuild* guild = sGuildAdventureMgr.FindGuild(handler->GetSession()->GetAccountId());
        if (!guild)
        {
            handler->SendErrorMessage("Your account has no adventure guild yet; it is founded when you enter the world.", false);
            return false;
        }
        return Apply(handler, *guild, ObjectGuid::Empty, {}, key, value);
    }

    static bool HandleAdminSetting(ChatHandler* handler, PlayerIdentifier target, std::string key, std::string value)
    {
        if (!Enabled(handler))
            return false;
        AdventureGuild* guild = GuildOf(handler, target);
        return guild && Apply(handler, *guild, ObjectGuid::Empty, {}, key, value);
    }

    static bool HandleAdminBotSetting(ChatHandler* handler, PlayerIdentifier target, std::string key, std::string value)
    {
        if (!Enabled(handler))
            return false;
        AdventureGuild* guild = GuildOf(handler, target);
        return guild && Apply(handler, *guild, target.GetGUID(), target.GetName(), key, value);
    }
};

class GuildAdventureSettingsWorldScript : public WorldScript
{
public:
    GuildAdventureSettingsWorldScript() : WorldScript("GuildAdventureSettingsWorldScript", {
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_UPDATE
    }) { }

    // Registered after the roster scripts (loader order), so the guilds are loaded when the settings index is built.
    void OnStartup() override
    {
        sBotSettings.Load();
        _started = true;
    }

    void OnUpdate(uint32 diff) override
    {
        if (_started && sBotSettings.Update(diff))
            sGadvProtocol.PushSettingDefs(); // a config reload changed a default: the addons get the schema again
    }

private:
    bool _started = false;
};

void AddGuildAdventureSettingsScripts()
{
    new GuildAdventureSettingsCommandScript();
    new GuildAdventureSettingsWorldScript();
}
