/*
 * ClientChannel implementation. See ClientChannel.h and docs/take-control.md §5.10.
 */

#include "ClientChannel.h"
#include "Chat.h"
#include "GuildAdventureConfig.h"
#include "Log.h"
#include "Opcodes.h"
#include "Random.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <algorithm>
#include <vector>

using namespace GuildAdventure;

namespace
{
    // A HELLO is 13 bytes and a PONG 5; a newer DLL may append fields, but nothing needs more than this.
    constexpr std::size_t MaxPayloadSize = 64;
    // Per account and second. The DLL sends a handful of messages per character-screen visit.
    constexpr uint32 MaxMessagesPerSecond = 20;
    // After this a ping counts as lost and a new one may be sent.
    constexpr std::chrono::seconds PingTimeout{10};

    std::string CapabilityNames(uint32 capabilities)
    {
        std::string names;
        auto add = [&names](char const* name)
        {
            if (!names.empty())
                names += ", ";
            names += name;
        };

        if (capabilities & CLIENT_CHANNEL_CAP_PING)
            add("ping");
        if (capabilities & CLIENT_CHANNEL_CAP_IN_WORLD_SWITCH)
            add("in-world switch");
        if (capabilities & CLIENT_CHANNEL_CAP_PLAYER_RECORD)
            add("player record");
        if (capabilities & ~uint32(CLIENT_CHANNEL_CAP_PING | CLIENT_CHANNEL_CAP_IN_WORLD_SWITCH
            | CLIENT_CHANNEL_CAP_PLAYER_RECORD))
            add("unknown bits");

        return names.empty() ? std::string("none") : names;
    }

    template <typename Duration>
    long long WholeSeconds(Duration duration)
    {
        return std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    }
}

char const* GuildAdventure::ToString(SwitchAckResult result)
{
    switch (result)
    {
        case SwitchAckResult::NotOffered: return "not offered";
        case SwitchAckResult::Armed: return "armed";
        case SwitchAckResult::OtherMap: return "other map";
        case SwitchAckResult::Busy: return "busy";
    }
    return "unknown";
}

uint32 ClientDllInfo::EffectiveVersion() const
{
    return std::min(protocolVersion, ClientChannelProtocolVersion);
}

bool ClientDllInfo::Has(uint32 capability) const
{
    return (capabilities & ClientChannelServerCapabilities & capability) == capability;
}

ClientChannel& ClientChannel::Instance()
{
    static ClientChannel instance;
    return instance;
}

bool ClientChannel::OnClientMessage(WorldSession* session, WorldPacket const& packet)
{
    // While the module is disabled the core's fallback handler (CP-5) drops the message without a reply.
    if (!GuildAdventure::IsModuleEnabled())
        return true;

    // Home sessions have no client and never send packets; nothing to do for them.
    if (session->IsHeadless())
        return false;

    uint32 const accountId = session->GetAccountId();
    Record& record = _records[accountId];
    if (record.session != session)
    {
        // A new connection of this account: nothing of the old one carries over.
        record = Record();
        record.session = session;
    }

    if (!AcceptRate(record, accountId))
        return false;

    if (packet.size() == 0 || packet.size() > MaxPayloadSize)
    {
        LOG_DEBUG("module", "mod-guild-adventure: client message of {} bytes from account {} dropped (size)",
            packet.size(), accountId);
        return false;
    }

    uint8 const type = packet.read<uint8>(0);
    switch (static_cast<ClientToServerMessage>(type))
    {
        case ClientToServerMessage::Hello:
            HandleHello(session, record, packet);
            break;
        case ClientToServerMessage::Pong:
            HandlePong(session, record, packet);
            break;
        case ClientToServerMessage::PlayerRecordAck:
            HandlePlayerRecordAck(session, record, packet);
            break;
        case ClientToServerMessage::SwitchAck:
            HandleSwitchAck(session, record, packet);
            break;
        default:
            // WORLD_READY is reserved.
            LOG_DEBUG("module", "mod-guild-adventure: client message type {} from account {} dropped (not handled)",
                type, accountId);
            break;
    }

    return false;
}

void ClientChannel::OnCharEnumRequest(WorldSession* session)
{
    // The client is at the character screen: it may have been restarted without the DLL. The ANNOUNCE that comes
    // with the list asks the DLL for a new HELLO.
    if (!session->IsHeadless())
        _records.erase(session->GetAccountId());
}

void ClientChannel::OnCharacterList(WorldSession* session)
{
    Announce(session);
}

void ClientChannel::Announce(WorldSession* session)
{
    if (!GuildAdventure::IsModuleEnabled() || session->IsHeadless())
        return;

    Send(session, ServerToClientMessage::Announce, ClientChannelProtocolVersion, ClientChannelServerCapabilities);
}

std::optional<ClientDllInfo> ClientChannel::GetClientDll(WorldSession* session) const
{
    Record const* record = FindRecord(session);
    return record ? record->dll : std::nullopt;
}

bool ClientChannel::HasCapability(WorldSession* session, uint32 capability) const
{
    std::optional<ClientDllInfo> const dll = GetClientDll(session);
    return dll && dll->Has(capability);
}

PingResult ClientChannel::SendPing(WorldSession* session, uint32 requesterAccountId)
{
    auto itr = _records.find(session->GetAccountId());
    if (itr == _records.end() || itr->second.session != session || !itr->second.dll)
        return PingResult::NoClientDll;

    Record& record = itr->second;
    if (!record.dll->Has(CLIENT_CHANNEL_CAP_PING))
        return PingResult::NotSupported;

    Clock::time_point const now = Clock::now();
    if (record.pingNonce)
    {
        if (now - record.pingSentAt < PingTimeout)
            return PingResult::Outstanding;

        LOG_INFO("module", "mod-guild-adventure: ping {} to account {} was not answered in {} s",
            record.pingNonce, session->GetAccountId(), PingTimeout.count());
    }

    record.pingNonce = NextNonce();
    record.pingSentAt = now;
    record.pingRequester = requesterAccountId;
    Send(session, ServerToClientMessage::Ping, record.pingNonce);
    return PingResult::Sent;
}

PlayerRecordResult ClientChannel::SendPlayerRecord(WorldSession* session, PlayerRecordInfo const& info)
{
    auto itr = _records.find(session->GetAccountId());
    if (itr == _records.end() || itr->second.session != session || !itr->second.dll)
        return PlayerRecordResult::NoClientDll;

    Record& record = itr->second;
    if (!record.dll->Has(CLIENT_CHANNEL_CAP_PLAYER_RECORD))
        return PlayerRecordResult::NotSupported;

    uint32 const guidLow = uint32(info.guid & 0xFFFFFFFF);
    uint32 const guidHigh = uint32(info.guid >> 32);
    bool const controlCharacter = std::any_of(info.name.begin(), info.name.end(),
        [](char c) { return static_cast<unsigned char>(c) < 0x20; });
    // The DLL checks the same (client/dll/src/ChannelProtocol.cpp, DecodePlayerRecord) and would refuse it.
    if (!guidLow || guidHigh || !info.race || !info.classId || info.gender > 2 || !info.level || info.name.empty()
        || info.name.size() >= ClientChannelPlayerRecordNameBytes || controlCharacter)
        return PlayerRecordResult::Invalid;

    std::vector<uint32> fields;
    fields.reserve(ClientChannelPlayerRecordFields);
    fields.push_back(guidLow);
    fields.push_back(guidHigh);
    fields.push_back(uint32(info.race) | (uint32(info.classId) << 8) | (uint32(info.gender) << 16)
        | (uint32(info.level) << 24));
    for (std::size_t word = 0; word < ClientChannelPlayerRecordNameBytes / 4; ++word)
    {
        uint32 value = 0;
        for (std::size_t byte = 0; byte < 4; ++byte)
        {
            std::size_t const index = word * 4 + byte;
            if (index < info.name.size())
                value |= uint32(static_cast<unsigned char>(info.name[index])) << (8 * byte);
        }
        fields.push_back(value);
    }

    record.recordGuidLow = guidLow;
    record.recordName = info.name;
    record.recordAcknowledged = false;
    record.recordApplied = false;
    Send(session, ServerToClientMessage::PlayerRecord, fields);
    return PlayerRecordResult::Sent;
}

uint32 ClientChannel::NextNonce()
{
    // Any value but 0 (= none outstanding); a counter from a random start is enough to tell answers apart.
    if (!_nextNonce)
        _nextNonce = rand32();
    if (!++_nextNonce)
        ++_nextNonce;
    return _nextNonce;
}

SwitchSendResult ClientChannel::SendSwitch(WorldSession* session, uint32 mapId, uint32& nonce)
{
    auto itr = _records.find(session->GetAccountId());
    if (itr == _records.end() || itr->second.session != session || !itr->second.dll)
        return SwitchSendResult::NoClientDll;

    Record& record = itr->second;
    if (!record.dll->Has(CLIENT_CHANNEL_CAP_IN_WORLD_SWITCH))
        return SwitchSendResult::NotSupported;

    nonce = NextNonce();
    record.switchNonce = nonce;
    record.switchMap = mapId;
    record.switchAck.reset();
    ++record.switches;
    Send(session, ServerToClientMessage::Switch, mapId, nonce);
    return SwitchSendResult::Sent;
}

std::optional<SwitchAckResult> ClientChannel::GetSwitchAck(WorldSession* session, uint32 nonce) const
{
    Record const* record = FindRecord(session);
    if (!record || !nonce || record->switchNonce != nonce)
        return std::nullopt;
    return record->switchAck;
}

std::string ClientChannel::DescribeState(WorldSession* session) const
{
    Record const* record = FindRecord(session);
    if (!record || !record->dll)
        return "no client DLL on this connection (no HELLO since the character list was last requested)";

    ClientDllInfo const& dll = *record->dll;
    Clock::time_point const now = Clock::now();

    std::string ping;
    if (record->pingNonce)
        ping = Acore::StringFormat("unanswered for {} s", WholeSeconds(now - record->pingSentAt));
    else if (record->lastRoundTrip)
        ping = Acore::StringFormat("last round trip {} ms, {} answered", record->lastRoundTrip->count(), record->pongs);
    else
        ping = "none sent";

    std::string characterRecord;
    if (!record->recordGuidLow)
        characterRecord = "none sent";
    else if (!record->recordAcknowledged)
        characterRecord = Acore::StringFormat("{} sent, not confirmed", record->recordName);
    else
        characterRecord = Acore::StringFormat("{} {}", record->recordName,
            record->recordApplied ? "written by the DLL" : "refused by the DLL");

    std::string inWorldSwitch;
    if (!record->switches)
        inWorldSwitch = "none sent";
    else
        inWorldSwitch = Acore::StringFormat("{} sent, {} armed, last {}", record->switches, record->switchesArmed,
            record->switchAck ? ToString(*record->switchAck) : "not answered");

    return Acore::StringFormat("DLL build {}, protocol {} (server {}, in use {}), capabilities 0x{:08X} ({}), "
        "usable here: {}; HELLO {} s ago; ping: {}; character record: {}; in-world switch: {}", dll.dllBuild,
        dll.protocolVersion, ClientChannelProtocolVersion, dll.EffectiveVersion(), dll.capabilities,
        CapabilityNames(dll.capabilities), CapabilityNames(dll.capabilities & ClientChannelServerCapabilities),
        WholeSeconds(now - record->helloAt), ping, characterRecord, inWorldSwitch);
}

bool ClientChannel::AcceptRate(Record& record, uint32 accountId)
{
    Clock::time_point const now = Clock::now();
    if (now - record.windowStart >= std::chrono::seconds(1))
    {
        record.windowStart = now;
        record.windowCount = 0;
        record.windowWarned = false;
    }

    if (++record.windowCount <= MaxMessagesPerSecond)
        return true;

    if (!record.windowWarned)
    {
        record.windowWarned = true;
        LOG_WARN("module", "mod-guild-adventure: account {} sends more than {} client messages per second; "
            "the rest of this second is dropped", accountId, MaxMessagesPerSecond);
    }
    return false;
}

void ClientChannel::HandleHello(WorldSession* session, Record& record, WorldPacket const& packet)
{
    if (packet.size() < 1 + 3 * sizeof(uint32))
    {
        LOG_DEBUG("module", "mod-guild-adventure: short HELLO ({} bytes) from account {} dropped", packet.size(),
            session->GetAccountId());
        return;
    }

    ClientDllInfo dll;
    dll.protocolVersion = packet.read<uint32>(1);
    dll.capabilities = packet.read<uint32>(5);
    dll.dllBuild = packet.read<uint32>(9);
    if (!dll.protocolVersion)
    {
        LOG_DEBUG("module", "mod-guild-adventure: HELLO with protocol version 0 from account {} dropped",
            session->GetAccountId());
        return;
    }

    record.dll = dll;
    record.helloAt = Clock::now();
    // A HELLO starts over: an earlier ping cannot be answered by this DLL instance any more.
    record.pingNonce = 0;

    LOG_INFO("module", "mod-guild-adventure: account {} has the client DLL (build {}, protocol {}, "
        "capabilities 0x{:08X})", session->GetAccountId(), dll.dllBuild, dll.protocolVersion, dll.capabilities);

    Send(session, ServerToClientMessage::HelloAck, ClientChannelProtocolVersion, ClientChannelServerCapabilities);
}

void ClientChannel::HandlePong(WorldSession* session, Record& record, WorldPacket const& packet)
{
    if (packet.size() < 1 + sizeof(uint32))
        return;

    uint32 const nonce = packet.read<uint32>(1);
    if (!record.dll || !record.pingNonce || nonce != record.pingNonce)
    {
        LOG_DEBUG("module", "mod-guild-adventure: unexpected PONG {} from account {} dropped", nonce,
            session->GetAccountId());
        return;
    }

    auto const roundTrip = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - record.pingSentAt);
    record.lastRoundTrip = roundTrip;
    record.pingNonce = 0;
    ++record.pongs;

    LOG_INFO("module", "mod-guild-adventure: client DLL of account {} answered ping {} in {} ms",
        session->GetAccountId(), nonce, roundTrip.count());

    if (!record.pingRequester)
        return;

    if (WorldSession* requester = sWorldSessionMgr->FindSession(record.pingRequester))
        ChatHandler(requester).PSendSysMessage("Client DLL of account {} answered the ping in {} ms.",
            session->GetAccountId(), roundTrip.count());
}

void ClientChannel::HandlePlayerRecordAck(WorldSession* session, Record& record, WorldPacket const& packet)
{
    if (packet.size() < 1 + 2 * sizeof(uint32))
        return;

    uint32 const guidLow = packet.read<uint32>(1);
    bool const applied = packet.read<uint32>(5) != 0;
    if (!record.dll || !record.recordGuidLow || guidLow != record.recordGuidLow)
    {
        LOG_DEBUG("module", "mod-guild-adventure: unexpected PLAYER_RECORD_ACK for guid {} from account {} dropped",
            guidLow, session->GetAccountId());
        return;
    }

    record.recordAcknowledged = true;
    record.recordApplied = applied;
    if (applied)
        LOG_INFO("module", "mod-guild-adventure: client DLL of account {} wrote the character record of {} (guid {})",
            session->GetAccountId(), record.recordName, guidLow);
    else
        LOG_WARN("module", "mod-guild-adventure: client DLL of account {} refused the character record of {} "
            "(guid {}); the client keeps the old name and class", session->GetAccountId(), record.recordName, guidLow);
}

void ClientChannel::HandleSwitchAck(WorldSession* session, Record& record, WorldPacket const& packet)
{
    if (packet.size() < 1 + 2 * sizeof(uint32))
        return;

    uint32 const nonce = packet.read<uint32>(1);
    uint32 const rawResult = packet.read<uint32>(5);
    if (!record.dll || !record.switchNonce || nonce != record.switchNonce || record.switchAck)
    {
        LOG_DEBUG("module", "mod-guild-adventure: unexpected SWITCH_ACK {} from account {} dropped", nonce,
            session->GetAccountId());
        return;
    }

    SwitchAckResult const result = rawResult <= uint32(SwitchAckResult::Busy) ? SwitchAckResult(rawResult)
        : SwitchAckResult::NotOffered;
    record.switchAck = result;
    if (result == SwitchAckResult::Armed)
    {
        ++record.switchesArmed;
        LOG_INFO("module", "mod-guild-adventure: client DLL of account {} keeps map {} loaded for the world reload "
            "(in-world switch {})", session->GetAccountId(), record.switchMap, nonce);
    }
    else
        LOG_INFO("module", "mod-guild-adventure: client DLL of account {} cannot keep map {} loaded ({}, result {}); "
            "the world reload shows a loading screen", session->GetAccountId(), record.switchMap, ToString(result),
            rawResult);
}

ClientChannel::Record const* ClientChannel::FindRecord(WorldSession* session) const
{
    auto itr = _records.find(session->GetAccountId());
    if (itr == _records.end() || itr->second.session != session)
        return nullptr;

    return &itr->second;
}

void ClientChannel::Send(WorldSession* session, ServerToClientMessage type, uint32 first, uint32 second)
{
    WorldPacket data(SMSG_GA_SERVER_MESSAGE, 1 + 2 * sizeof(uint32));
    data << uint8(type) << first << second;
    session->SendPacket(&data);
}

void ClientChannel::Send(WorldSession* session, ServerToClientMessage type, uint32 value)
{
    WorldPacket data(SMSG_GA_SERVER_MESSAGE, 1 + sizeof(uint32));
    data << uint8(type) << value;
    session->SendPacket(&data);
}

void ClientChannel::Send(WorldSession* session, ServerToClientMessage type, std::vector<uint32> const& fields)
{
    WorldPacket data(SMSG_GA_SERVER_MESSAGE, 1 + fields.size() * sizeof(uint32));
    data << uint8(type);
    for (uint32 field : fields)
        data << field;
    session->SendPacket(&data);
}
