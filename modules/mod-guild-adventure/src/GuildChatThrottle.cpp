/*
 * mod-guild-adventure: guild chat throttle for AI-played guild characters.
 *
 * mod-playerbots lets bots talk in guild chat (loot, quests, kills, level-ups) and answer each other's lines, which
 * keeps a guild of a few bots talking without pause (the owner, 2026-11-12: [quote left out]). This script keeps the owner's client to one such line every
 * GuildAdventure.BotGuildChat.MinSeconds..MaxSeconds (a new random pause after each line shown); the others are not
 * sent to that client. The bots still hear every line (playerbots reads it in OnPacketSent, before this hook), so
 * nothing changes for their AI.
 *
 * Only guild and officer chat whose sender is a guild character the AI plays right now (state Bot in the guild bot
 * holder, or a watched character) is throttled. What the owner types, what a character he plays says, the module's own
 * messages (system lines, GADV whispers) and guild achievement lines pass untouched.
 *
 * Thread safety: CanPacketSend runs on the thread that sends, map threads included (bots talk during map updates), so
 * the per-client pauses sit behind a mutex. GuildBotHolder's entries change only on the world thread outside map
 * updates.
 */

#include "BotSettings.h"
#include "GuildAdventureConfig.h"
#include "GuildAdventureSettings.h"
#include "GuildBotHolder.h"
#include "GameTime.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>

using namespace GuildAdventure;

namespace
{
    std::mutex tThrottleLock;
    std::unordered_map<uint32, Milliseconds> tNextLine; // receiving client's account id -> time of its next bot line

    // uint8 type, int32 language, uint64 sender (ChatHandler::BuildChatPacket)
    bool IsAIGuildChat(WorldPacket const& packet)
    {
        if (packet.GetOpcode() != SMSG_MESSAGECHAT || packet.size() < 13)
            return false;
        uint8 const type = packet.read<uint8>(0);
        if (type != CHAT_MSG_GUILD && type != CHAT_MSG_OFFICER)
            return false;
        if (packet.read<uint32>(1) == uint32(LANG_ADDON))
            return false;
        ObjectGuid const sender(packet.read<uint64>(5));
        return sender.IsPlayer() && (sGuildBotHolder.IsAIDriven(sender) || sGuildBotHolder.IsWatched(sender));
    }

    bool AllowBotGuildLine(WorldSession* session)
    {
        // The owner's guild (the client's account owns it) decides how often its bots' lines come through.
        Settings settings = GetSettings();
        settings.botGuildChatMinSeconds = uint32(sBotSettings.GetForGuild(session->GetAccountId(), Setting::BotGuildChatMinSeconds));
        settings.botGuildChatMaxSeconds = uint32(sBotSettings.GetForGuild(session->GetAccountId(), Setting::BotGuildChatMaxSeconds));
        if (!settings.botGuildChatMinSeconds)
            return true;
        Milliseconds const now = GameTime::GetGameTimeMS();
        std::lock_guard<std::mutex> lock(tThrottleLock);
        Milliseconds& next = tNextLine[session->GetAccountId()];
        if (now < next)
            return false;
        uint32 const pause = urand(settings.botGuildChatMinSeconds,
            std::max(settings.botGuildChatMinSeconds, settings.botGuildChatMaxSeconds));
        next = now + Seconds(pause);
        return true;
    }
}

class GuildAdventureChatServerScript : public ServerScript
{
public:
    GuildAdventureChatServerScript() : ServerScript("GuildAdventureChatServerScript", {
        SERVERHOOK_CAN_PACKET_SEND
    }) { }

    bool CanPacketSend(WorldSession* session, WorldPacket const& packet) override
    {
        if (!session || !IsModuleEnabled() || !IsAIGuildChat(packet))
            return true;
        return AllowBotGuildLine(session);
    }
};

void AddGuildAdventureChatScripts()
{
    new GuildAdventureChatServerScript();
}
