/*
 * The GADV part of the bot behaviour settings (docs/protocol.md section 7.19, BotSettings.h). Members of GadvProtocol,
 * kept apart from GadvProtocol.cpp.
 *
 *   SETDEFS_BEGIN^sv ... SETCAT / SETDEF records ... SETDEFS_END   the schema; sent when the client's HELLO^sv differs
 *   SETVALS_BEGIN ... SETVAL^g^v ... SETVALS_END                  the values of the guild (g = 0) and of each character
 *                                                                  that has its own; always sent with the sync
 *   SETVAL^g^v                                                      after a change: the whole scope again
 *   SETTING_SET^g^v^del^all                                         the intent
 */

#include "BotSettings.h"
#include "GadvProtocol.h"
#include "GuildAdventureMgr.h"
#include "StringFormat.h"
#include <algorithm>
#include <cstdlib>
#include <vector>

using namespace GuildAdventure;
using GuildAdventure::Gadv::Message;

namespace
{
    std::string EncodeValues(SettingValues const& values)
    {
        std::string text;
        for (auto const& [id, value] : values)
        {
            if (!text.empty())
                text += ',';
            text += Acore::StringFormat("{}:{}", id, value);
        }
        return text;
    }

    // "101:65,102:40" -> pairs; false on a malformed item.
    bool DecodeValues(std::string const& text, std::vector<std::pair<int64, int64>>& out)
    {
        std::size_t start = 0;
        while (start < text.size())
        {
            std::size_t end = text.find(',', start);
            if (end == std::string::npos)
                end = text.size();
            std::string const item = text.substr(start, end - start);
            std::size_t const colon = item.find(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= item.size())
                return false;
            char* stop = nullptr;
            long long const id = std::strtoll(item.c_str(), &stop, 10);
            if (stop != item.c_str() + colon)
                return false;
            char const* valueText = item.c_str() + colon + 1;
            long long const value = std::strtoll(valueText, &stop, 10);
            if (stop != item.c_str() + item.size())
                return false;
            out.emplace_back(id, value);
            start = end + 1;
        }
        return true;
    }

    bool IsGuildCharacter(AdventureGuild const& guild, ObjectGuid guid)
    {
        return guid == guild.leader || guild.bots.count(guid) > 0;
    }
}

Message GadvProtocol::BuildSettingValues(uint32 ownerAccount, ObjectGuid character) const
{
    SettingValues const values = character.IsEmpty() ? sBotSettings.GuildValues(ownerAccount)
        : sBotSettings.CharacterValues(character);
    return Message("SETVAL")
        .Set("g", character.IsEmpty() ? 0u : character.GetCounter())
        .Set("v", EncodeValues(values));
}

void GadvProtocol::SendSettingDefs(uint32 accountId)
{
    std::vector<Message> records;
    uint32 order = 0;
    for (SettingCategory const& category : BotSettings::Categories())
        records.push_back(Message("SETCAT")
            .Set("k", category.key)
            .Set("n", category.label)
            .Set("h", category.help)
            .Set("o", ++order));
    for (SettingDef const& def : BotSettings::Defs())
        records.push_back(Message("SETDEF")
            .Set("id", uint32(def.id))
            .Set("k", def.key)
            .Set("cat", def.category)
            .Set("t", ToToken(def.type))
            .Set("min", def.min)
            .Set("max", def.type == SettingType::Enum ? int32(BotSettings::Options(def).size()) - 1 : def.max)
            .Set("st", std::max(def.step, 1))
            .Set("sc", std::max(def.scale, 1))
            .Set("u", def.unit)
            .Set("def", BotSettings::Default(def))
            .Set("pc", def.perCharacter)
            .Set("adv", def.advanced)
            .Set("opt", def.options)
            .Set("n", def.label)
            .Set("h", def.help));
    SendBlock(accountId, "SETDEFS", records, Message("SETDEFS_BEGIN").Set("sv", sBotSettings.GetSchemaVersion()));
}

void GadvProtocol::SendSettingValues(uint32 accountId, AdventureGuild const& guild)
{
    std::vector<Message> records;
    records.push_back(BuildSettingValues(guild.ownerAccount, ObjectGuid::Empty));
    for (ObjectGuid character : sBotSettings.CharactersWithValues(guild.ownerAccount))
        records.push_back(BuildSettingValues(guild.ownerAccount, character));
    SendBlock(accountId, "SETVALS", records, Message("SETVALS_BEGIN"));
}

void GadvProtocol::SendSettings(uint32 accountId, AdventureGuild const& guild, int64 clientSchemaVersion)
{
    if (clientSchemaVersion != int64(sBotSettings.GetSchemaVersion()))
        SendSettingDefs(accountId);
    SendSettingValues(accountId, guild);
}

void GadvProtocol::PushSettingValues(uint32 ownerAccount, ObjectGuid character)
{
    if (FindConnectedLink(ownerAccount))
        Send(ownerAccount, BuildSettingValues(ownerAccount, character));
}

void GadvProtocol::PushSettingDefs()
{
    std::vector<uint32> accounts;
    for (auto const& [accountId, link] : _links)
        if (link.connected)
            accounts.push_back(accountId);
    for (uint32 accountId : accounts)
        SendSettingDefs(accountId);
}

void GadvProtocol::OnSettingSet(uint32 accountId, AdventureGuild& guild, Message const& message)
{
    std::optional<int64> const r = message.GetInt("r");
    int64 const g = message.GetInt("g").value_or(0);
    ObjectGuid const character = g > 0 ? ObjectGuid::Create<HighGuid::Player>(uint32(g)) : ObjectGuid::Empty;
    if (!character.IsEmpty() && !IsGuildCharacter(guild, character))
    {
        Error(accountId, r, "SETTING_INVALID", "That character is not in your guild.", "NOT_MEMBER");
        return;
    }

    std::vector<std::pair<int64, int64>> sets;
    if (!DecodeValues(message.GetString("v"), sets))
    {
        Error(accountId, r, "BAD_REQUEST", "SETTING_SET: v must be id:value pairs.");
        return;
    }
    std::vector<int64> clears;
    if (message.Has("del"))
    {
        std::optional<std::vector<int64>> list = message.GetIntList("del");
        if (!list)
        {
            Error(accountId, r, "BAD_REQUEST", "SETTING_SET: del must be a list of setting ids.");
            return;
        }
        clears = *list;
    }
    bool const all = message.GetBool("all").value_or(false);

    // All or nothing: check everything first.
    for (auto const& [id, value] : sets)
    {
        std::string code;
        std::string text;
        if (!BotSettings::Check(BotSettings::Find(uint32(id)), !character.IsEmpty(), value, code, text))
        {
            Error(accountId, r, "SETTING_INVALID", text, code);
            return;
        }
    }
    for (int64 id : clears)
        if (!BotSettings::Find(uint32(id)))
        {
            Error(accountId, r, "SETTING_INVALID", "There is no such setting.", "UNKNOWN");
            return;
        }

    if (all)
        sBotSettings.ClearAll(guild.ownerAccount, character);
    for (int64 id : clears)
        sBotSettings.Clear(guild.ownerAccount, character, Setting(uint16(id)));
    for (auto const& [id, value] : sets)
        sBotSettings.Set(guild.ownerAccount, character, Setting(uint16(id)), int32(value));

    Send(accountId, BuildSettingValues(guild.ownerAccount, character));
    Ack(accountId, r);
}
