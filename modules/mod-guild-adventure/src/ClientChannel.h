/*
 * ClientChannel: the module's end of the custom opcodes of core patch CP-5 (docs/core-patches.md), the channel to
 * the take-control client DLL of Path S (docs/take-control.md §5.10). It announces itself, records the DLL's HELLO,
 * measures a round trip with PING / PONG and, for a Path W world reload, sends the DLL the character the client is
 * about to play (PLAYER_RECORD, §5.8), because the client answers UnitName / UnitClass("player") from its record
 * of the character it logged in with, and asks the DLL to keep the loaded map during that reload (SWITCH, the
 * in-world switch of Path S, §5.11).
 *
 * Wire format, both directions: the opcode's payload starts with a uint8 message type, then the fields of that
 * type, little endian like every packet.
 *
 *   client -> server, CMSG_GA_CLIENT_MESSAGE (0x521)
 *     1 HELLO              uint32 protocolVersion, uint32 capabilities, uint32 dllBuild
 *     2 PONG               uint32 nonce
 *     3 SWITCH_ACK         uint32 nonce, uint32 result (SwitchAckResult)
 *     4 WORLD_READY        reserved
 *     5 PLAYER_RECORD_ACK  uint32 guidLow, uint32 applied (1) or refused (0)
 *   server -> client, SMSG_GA_SERVER_MESSAGE (0x522)
 *     1 ANNOUNCE           uint32 protocolVersion, uint32 capabilities
 *     2 HELLO_ACK          uint32 protocolVersion, uint32 capabilities
 *     3 PING               uint32 nonce
 *     4 SWITCH             uint32 mapId, uint32 nonce
 *     5 PLAYER_RECORD      uint32 guidLow, uint32 guidHigh, uint32 race | class << 8 | gender << 16 | level << 24,
 *                          uint32 name[12] (UTF-8, little endian, padded with 0, at most 47 bytes)
 *
 * Contract with the DLL:
 *   - The server speaks first: it sends ANNOUNCE with every character list (right before SMSG_CHAR_ENUM) and when
 *     a GM types ".ga client" without a recorded DLL. A client without the DLL drops the unknown opcode quietly
 *     (its handler table ends at 0x51E).
 *   - The DLL sends HELLO only in reply to an ANNOUNCE. A core without CP-5 never announces, and it would close the
 *     connection on 0x521 (WorldSocket::ReadHeaderHandler), so the DLL can never break a connection to one.
 *   - The module forgets an account's HELLO when that account asks for its character list again
 *     (CMSG_CHAR_ENUM); the ANNOUNCE that comes with the list asks for a new one. A client restarted without the
 *     DLL is therefore never taken for one with it.
 *   - The lower of the two protocol versions applies; a capability counts only when both sides announce it.
 *   - The DLL answers every PING with a PONG carrying the same nonce.
 *   - PLAYER_RECORD goes only to a DLL that announced CLIENT_CHANNEL_CAP_PLAYER_RECORD; the DLL writes the record
 *     and answers PLAYER_RECORD_ACK (refused when the message is malformed).
 *   - SWITCH goes only to a DLL that announced CLIENT_CHANNEL_CAP_IN_WORLD_SWITCH, right before the world reload of
 *     a swap (SMSG_TRANSFER_PENDING + SMSG_NEW_WORLD for the map it names). The DLL answers SWITCH_ACK at once:
 *     Armed means the reload keeps the loaded map and shows no loading screen; any other result means the client
 *     runs the reload as a normal Path W world reload. Either way the swap completes on MSG_MOVE_WORLDPORT_ACK.
 *
 * Everything runs on the world thread: both opcodes, CMSG_CHAR_ENUM, the character-list callback and the GM
 * commands are processed there.
 * Messages are limited in size and rate per account; anything else is dropped without a reply.
 */

#ifndef MOD_GUILD_ADVENTURE_CLIENT_CHANNEL_H
#define MOD_GUILD_ADVENTURE_CLIENT_CHANNEL_H

#include "Define.h"
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class WorldPacket;
class WorldSession;

namespace GuildAdventure
{
    enum class ClientToServerMessage : uint8
    {
        Hello           = 1,
        Pong            = 2,
        SwitchAck       = 3,
        WorldReady      = 4, // reserved
        PlayerRecordAck = 5
    };

    enum class ServerToClientMessage : uint8
    {
        Announce     = 1,
        HelloAck     = 2,
        Ping         = 3,
        Switch       = 4,
        PlayerRecord = 5
    };

    // Capability bits, the same for both sides: a feature is usable when both announce its bit.
    enum ClientChannelCapability : uint32
    {
        CLIENT_CHANNEL_CAP_PING            = 0x00000001, // PING / PONG
        CLIENT_CHANNEL_CAP_IN_WORLD_SWITCH = 0x00000002, // Path S: switch the active player in the world
        CLIENT_CHANNEL_CAP_PLAYER_RECORD   = 0x00000004  // PLAYER_RECORD: the DLL rewrites the character record
    };

    constexpr uint32 ClientChannelProtocolVersion = 1;
    // What this module build offers.
    constexpr uint32 ClientChannelServerCapabilities = CLIENT_CHANNEL_CAP_PING | CLIENT_CHANNEL_CAP_IN_WORLD_SWITCH
        | CLIENT_CHANNEL_CAP_PLAYER_RECORD;

    // PLAYER_RECORD: 3 fields and the name in 12 words.
    constexpr std::size_t ClientChannelPlayerRecordNameBytes = 48;
    constexpr std::size_t ClientChannelPlayerRecordFields = 3 + ClientChannelPlayerRecordNameBytes / 4;

    // SWITCH: map id and nonce.
    constexpr std::size_t ClientChannelSwitchFields = 2;

    // The result a SWITCH_ACK carries (client/dll/src/ChannelProtocol.h, SwitchResult).
    enum class SwitchAckResult : uint32
    {
        NotOffered = 0, // the DLL does not offer the in-world switch
        Armed      = 1, // the reload keeps the map: no loading screen
        OtherMap   = 2, // the client has another map loaded, or no world
        Busy       = 3  // the client part of an earlier switch is still running
    };
    char const* ToString(SwitchAckResult result);

    enum class SwitchSendResult : uint8
    {
        Sent,
        NoClientDll,  // no HELLO on this connection
        NotSupported  // the DLL did not announce CLIENT_CHANNEL_CAP_IN_WORLD_SWITCH
    };

    // The character a PLAYER_RECORD names; ControlSwapService fills it from the Player.
    struct PlayerRecordInfo
    {
        uint64 guid = 0; // raw GUID, high word 0 for players
        uint8 race = 0;
        uint8 classId = 0;
        uint8 gender = 0;
        uint8 level = 0;
        std::string name; // UTF-8
    };

    struct ClientDllInfo
    {
        uint32 protocolVersion = 0; // what the client announced
        uint32 capabilities = 0;    // what the client announced
        uint32 dllBuild = 0;
        [[nodiscard]] uint32 EffectiveVersion() const;
        [[nodiscard]] bool Has(uint32 capability) const; // announced by both sides
    };

    enum class PingResult : uint8
    {
        Sent,
        NoClientDll,  // no HELLO on this connection
        NotSupported, // the DLL did not announce CLIENT_CHANNEL_CAP_PING
        Outstanding   // the previous ping is still unanswered (less than PingTimeout ago)
    };

    enum class PlayerRecordResult : uint8
    {
        Sent,
        NoClientDll,  // no HELLO on this connection
        NotSupported, // the DLL did not announce CLIENT_CHANNEL_CAP_PLAYER_RECORD
        Invalid       // not a player GUID, a zero race, class or level, or a name of 0 or more than 47 bytes
    };
}

class ClientChannel
{
public:
    static ClientChannel& Instance();

    // Core hooks. OnClientMessage (ServerScript::CanPacketReceive) returns false when the module consumed the
    // message, true while the module is disabled, so the core's quiet fallback handler drops it.
    bool OnClientMessage(WorldSession* session, WorldPacket const& packet);
    void OnCharEnumRequest(WorldSession* session); // CMSG_CHAR_ENUM (ServerScript::CanPacketReceive)
    void OnCharacterList(WorldSession* session);   // SMSG_CHAR_ENUM is being built (CP-3, SessionScript)

    // Sends ANNOUNCE: a DLL on this connection answers with HELLO. Nothing while the module is disabled.
    void Announce(WorldSession* session);

    // The DLL that said HELLO on this session's connection, nullopt for a client without it.
    [[nodiscard]] std::optional<GuildAdventure::ClientDllInfo> GetClientDll(WorldSession* session) const;
    [[nodiscard]] bool HasCapability(WorldSession* session, uint32 capability) const;

    // Sends a PING; the answer is reported to 'requesterAccountId' (a GM's account, 0 = log only) when it arrives.
    GuildAdventure::PingResult SendPing(WorldSession* session, uint32 requesterAccountId);

    // Sends PLAYER_RECORD (Path W, before the world reload): the DLL writes 'record' into the client's record of the
    // logged-in character, so UnitName / UnitClass("player") name that character after the reload.
    GuildAdventure::PlayerRecordResult SendPlayerRecord(WorldSession* session,
        GuildAdventure::PlayerRecordInfo const& record);
    // Sends SWITCH (Path S, right before the world reload of a swap to a target on 'mapId'); 'nonce' receives the
    // number its SWITCH_ACK will carry.
    GuildAdventure::SwitchSendResult SendSwitch(WorldSession* session, uint32 mapId, uint32& nonce);
    // The SWITCH_ACK for 'nonce' on this session's connection; nullopt while it has not arrived.
    [[nodiscard]] std::optional<GuildAdventure::SwitchAckResult> GetSwitchAck(WorldSession* session, uint32 nonce) const;

    [[nodiscard]] std::string DescribeState(WorldSession* session) const;

private:
    using Clock = std::chrono::steady_clock;

    struct Record
    {
        WorldSession* session = nullptr; // the connection that said HELLO; a new connection must say it again
        std::optional<GuildAdventure::ClientDllInfo> dll;
        Clock::time_point helloAt;

        // ping
        uint32 pingNonce = 0; // 0 = none outstanding
        Clock::time_point pingSentAt;
        uint32 pingRequester = 0;
        std::optional<std::chrono::milliseconds> lastRoundTrip;
        uint32 pongs = 0;

        // character record (PLAYER_RECORD)
        uint32 recordGuidLow = 0; // the last one sent; 0 = none
        std::string recordName;
        bool recordAcknowledged = false;
        bool recordApplied = false;

        // in-world switch (SWITCH)
        uint32 switchNonce = 0; // the last one sent; 0 = none
        uint32 switchMap = 0;
        std::optional<GuildAdventure::SwitchAckResult> switchAck;
        uint32 switches = 0;     // SWITCH messages sent on this connection
        uint32 switchesArmed = 0;

        // rate limit
        Clock::time_point windowStart;
        uint32 windowCount = 0;
        bool windowWarned = false;
    };

    bool AcceptRate(Record& record, uint32 accountId);
    void HandleHello(WorldSession* session, Record& record, WorldPacket const& packet);
    void HandlePong(WorldSession* session, Record& record, WorldPacket const& packet);
    void HandlePlayerRecordAck(WorldSession* session, Record& record, WorldPacket const& packet);
    void HandleSwitchAck(WorldSession* session, Record& record, WorldPacket const& packet);
    uint32 NextNonce();
    [[nodiscard]] Record const* FindRecord(WorldSession* session) const;
    static void Send(WorldSession* session, GuildAdventure::ServerToClientMessage type, uint32 first, uint32 second);
    static void Send(WorldSession* session, GuildAdventure::ServerToClientMessage type, uint32 value);
    static void Send(WorldSession* session, GuildAdventure::ServerToClientMessage type,
        std::vector<uint32> const& fields);

    std::unordered_map<uint32, Record> _records; // by the account of the client connection
    uint32 _nextNonce = 0;
};

#define sClientChannel ClientChannel::Instance()

#endif
