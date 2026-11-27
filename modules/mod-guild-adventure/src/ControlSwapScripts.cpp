/*
 * Core hooks of the take-control feature (docs/take-control.md §5.2, §5.6). The logic lives in
 * ControlSwapService; these scripts only forward.
 *
 * Hooks used:
 *   SessionScript  OnSessionLogoutDetach (core patch CP-1), OnSessionCharEnum (core patch CP-3; also sends the
 *                  client channel's ANNOUNCE)
 *   ServerScript   CanPacketReceive: CMSG_PLAYER_LOGIN at the character screen, CMSG_UPDATE_ACCOUNT_DATA,
 *                  CMSG_CHAR_ENUM and the client DLL's CMSG_GA_CLIENT_MESSAGE (core patch CP-5, ClientChannel);
 *                  all STATUS_AUTHED, PROCESS_THREADUNSAFE: world thread; and the client's movement opcodes of a
 *                  watched character (dropped; map threads: GuildBotHolder::IsWatchedAnyThread)
 *   PlayerScript   OnPlayerLogin, OnPlayerBeforeLogout, OnPlayerCanMarkAccountOffline (GuildBotHolder: several guild
 *                  bots share one bot account, a logout must not mark the others offline)
 */

#include "ClientChannel.h"
#include "ControlSwapService.h"
#include "GuildAdventureConfig.h"
#include "GuildBotHolder.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

class GuildAdventureSessionScript : public SessionScript
{
public:
    GuildAdventureSessionScript() : SessionScript("GuildAdventureSessionScript", {
        SESSIONHOOK_ON_LOGOUT_DETACH,
        SESSIONHOOK_ON_CHAR_ENUM
    }) { }

    bool OnSessionLogoutDetach(WorldSession* session, Player* player) override
    {
        // Not gated by GuildAdventure.Enable: a controlled guild bot must go back to its AI even after the module
        // was disabled by a config reload, otherwise it would log out.
        return sControlSwapService.OnLogoutDetach(session, player);
    }

    void OnSessionCharEnum(WorldSession* session, WorldPacket& data, uint8& characterCount) override
    {
        sControlSwapService.OnCharEnum(session, data, characterCount);
        // Goes out right before the list; a client without the take-control DLL drops it quietly.
        sClientChannel.OnCharacterList(session);
    }
};

class GuildAdventureServerScript : public ServerScript
{
public:
    GuildAdventureServerScript() : ServerScript("GuildAdventureServerScript", {
        SERVERHOOK_CAN_PACKET_RECEIVE
    }) { }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        if (!session)
            return true;

        switch (packet.GetOpcode())
        {
            case CMSG_PLAYER_LOGIN:
                // Returns false when the module attaches a guild character itself (or refuses the login); the
                // core handler then does not run.
                return sControlSwapService.OnPlayerLoginPacket(session, packet);
            case CMSG_UPDATE_ACCOUNT_DATA:
                // Returns false for per-character UI data that belongs to another character after a Path W swap.
                return sControlSwapService.OnAccountDataUpload(session, packet);
            case CMSG_CHAR_ENUM:
                // The client may have been restarted without the DLL: forget its HELLO.
                sClientChannel.OnCharEnumRequest(session);
                return true;
            case MSG_MOVE_START_FORWARD:
            case MSG_MOVE_START_BACKWARD:
            case MSG_MOVE_STOP:
            case MSG_MOVE_START_STRAFE_LEFT:
            case MSG_MOVE_START_STRAFE_RIGHT:
            case MSG_MOVE_STOP_STRAFE:
            case MSG_MOVE_JUMP:
            case MSG_MOVE_START_TURN_LEFT:
            case MSG_MOVE_START_TURN_RIGHT:
            case MSG_MOVE_STOP_TURN:
            case MSG_MOVE_START_PITCH_UP:
            case MSG_MOVE_START_PITCH_DOWN:
            case MSG_MOVE_STOP_PITCH:
            case MSG_MOVE_SET_RUN_MODE:
            case MSG_MOVE_SET_WALK_MODE:
            case MSG_MOVE_FALL_LAND:
            case MSG_MOVE_START_SWIM:
            case MSG_MOVE_STOP_SWIM:
            case MSG_MOVE_SET_FACING:
            case MSG_MOVE_SET_PITCH:
            case MSG_MOVE_HEARTBEAT:
            case CMSG_MOVE_FALL_RESET:
            case CMSG_MOVE_SET_FLY:
            case MSG_MOVE_START_ASCEND:
            case MSG_MOVE_STOP_ASCEND:
            case CMSG_MOVE_CHNG_TRANSPORT:
            case MSG_MOVE_START_DESCEND:
                // A watched character is moved by the AI (watch mode, docs/take-control.md §5.12): its client's own
                // movement is not taken. The client gets its controls back after every teleport and taxi landing until
                // the lock is sent again, and on a mount after an instant flight its own fall moved the leader 87 yd into
                // the hill above Splintertree Post, where it stood five minutes, "falling, mounted", until a rescue
                // (2026-11-25). The core takes no client movement while a spline runs; between two splines it did.
                if (Player* player = session->GetPlayer())
                    if (sGuildBotHolder.IsWatchedAnyThread(player->GetGUID()))
                        return false;
                return true;
            case CMSG_GA_CLIENT_MESSAGE:
                // The module owns this opcode (CP-5); returns true only while the module is disabled, and the
                // core's fallback handler then drops the message.
                return sClientChannel.OnClientMessage(session, packet);
            default:
                return true;
        }
    }
};

class GuildAdventurePlayerScript : public PlayerScript
{
public:
    GuildAdventurePlayerScript() : PlayerScript("GuildAdventurePlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_BEFORE_LOGOUT,
        PLAYERHOOK_CAN_MARK_ACCOUNT_OFFLINE
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (GuildAdventure::IsModuleEnabled())
            sControlSwapService.OnPlayerLogin(player);
    }

    void OnPlayerBeforeLogout(Player* player) override
    {
        sControlSwapService.OnPlayerBeforeLogout(player);
    }

    bool OnPlayerCanMarkAccountOffline(ObjectGuid guid, uint32 accountId) override
    {
        // Not gated by GuildAdventure.Enable, like the logout detach: the bots of a disabled module are still in
        // the world until they log out.
        return sGuildBotHolder.OnCanMarkAccountOffline(guid, accountId);
    }
};

void AddGuildAdventureControlSwapScripts()
{
    new GuildAdventureSessionScript();
    new GuildAdventureServerScript();
    new GuildAdventurePlayerScript();
}
