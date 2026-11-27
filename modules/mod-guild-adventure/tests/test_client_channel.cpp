// Offline tests for src/ClientChannel.cpp, the module's end of core patch CP-5 (docs/take-control.md §5.10).
//
// The real ClientChannel.cpp is compiled against the stub headers in tests/stubs (no core, no database). From the
// module folder modules/mod-guild-adventure:
//
//     g++ -std=c++20 -Wall -Wextra -I tests/stubs tests/test_client_channel.cpp src/ClientChannel.cpp -o test_cc
//     ./test_cc
//
// Any C++20 compiler with <format> works (GCC 13, Clang 17, MSVC 2022: cl /std:c++20 /EHsc /I tests\stubs ...).
// The module's CMake build only collects src/, so nothing in tests/ ends up in the worldserver.

#include "../src/ClientChannel.h"
#include "../src/GuildAdventureConfig.h"
#include "Log.h"
#include "Opcodes.h"
#include "Random.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace GuildAdventure;

// ---- definitions the stubs and the module's config header expect ----

namespace
{
    bool sModuleEnabled = true;
    uint32 sRandomValue = 1000;
}

bool GuildAdventure::IsModuleEnabled()
{
    return sModuleEnabled;
}

uint32 rand32()
{
    return sRandomValue;
}

std::vector<TestLogLine>& TestLog()
{
    static std::vector<TestLogLine> lines;
    return lines;
}

WorldSessionMgr* WorldSessionMgr::instance()
{
    static WorldSessionMgr manager;
    return &manager;
}

// ---- helpers ----

namespace
{
    struct Failure
    {
        std::string message;
    };

    void Check(bool condition, std::string const& message)
    {
        if (!condition)
            throw Failure{ message };
    }

    WorldPacket ClientMessage(std::vector<uint32> const& fields, uint8 type)
    {
        WorldPacket packet(CMSG_GA_CLIENT_MESSAGE);
        packet << type;
        for (uint32 field : fields)
            packet << field;
        return packet;
    }

    WorldPacket Hello(uint32 version = 1, uint32 capabilities = CLIENT_CHANNEL_CAP_PING, uint32 build = 7)
    {
        return ClientMessage({ version, capabilities, build }, uint8(ClientToServerMessage::Hello));
    }

    WorldPacket Pong(uint32 nonce)
    {
        return ClientMessage({ nonce }, uint8(ClientToServerMessage::Pong));
    }

    bool Contains(std::string const& text, std::string const& part)
    {
        return text.find(part) != std::string::npos;
    }

    std::size_t CountLog(std::string const& level, std::string const& part)
    {
        std::size_t count = 0;
        for (TestLogLine const& line : TestLog())
            if (line.level == level && Contains(line.text, part))
                ++count;
        return count;
    }

    void Reset()
    {
        sModuleEnabled = true;
        sRandomValue = 1000;
        TestLog().clear();
        sWorldSessionMgr->sessions.clear();
    }

    // ---- cases ----

    void CharacterListAnnouncesTheChannel()
    {
        ClientChannel channel;
        WorldSession session(5);
        channel.OnCharacterList(&session);
        Check(session.sent.size() == 1, "no ANNOUNCE with the character list");
        WorldPacket const& announce = session.sent[0];
        Check(announce.GetOpcode() == SMSG_GA_SERVER_MESSAGE && announce.size() == 9, "ANNOUNCE packet");
        Check(announce.read<uint8>(0) == uint8(ServerToClientMessage::Announce), "ANNOUNCE type");
        Check(announce.read<uint32>(1) == ClientChannelProtocolVersion, "ANNOUNCE version");
        Check(announce.read<uint32>(5) == ClientChannelServerCapabilities, "ANNOUNCE capabilities");
        Check(!channel.GetClientDll(&session), "an ANNOUNCE alone recorded a DLL");
    }

    void NoAnnounceWhileDisabledOrForHomeSessions()
    {
        ClientChannel channel;
        WorldSession home(5, true);
        channel.OnCharacterList(&home);
        Check(home.sent.empty(), "ANNOUNCE to a home session");

        sModuleEnabled = false;
        WorldSession session(6);
        channel.OnCharacterList(&session);
        channel.Announce(&session);
        Check(session.sent.empty(), "ANNOUNCE while the module is disabled");
    }

    void HelloIsAnsweredAndRecorded()
    {
        ClientChannel channel;
        WorldSession session(5);
        Check(!channel.OnClientMessage(&session, Hello(1, CLIENT_CHANNEL_CAP_PING, 42)), "HELLO not consumed");
        Check(session.sent.size() == 1, "no HELLO_ACK");
        WorldPacket const& ack = session.sent[0];
        Check(ack.GetOpcode() == SMSG_GA_SERVER_MESSAGE, "HELLO_ACK on the wrong opcode");
        Check(ack.size() == 9, "HELLO_ACK size");
        Check(ack.read<uint8>(0) == uint8(ServerToClientMessage::HelloAck), "HELLO_ACK type");
        Check(ack.read<uint32>(1) == ClientChannelProtocolVersion, "HELLO_ACK version");
        Check(ack.read<uint32>(5) == ClientChannelServerCapabilities, "HELLO_ACK capabilities");

        std::optional<ClientDllInfo> const dll = channel.GetClientDll(&session);
        Check(dll && dll->dllBuild == 42 && dll->protocolVersion == 1, "HELLO not recorded");
        Check(Contains(channel.DescribeState(&session), "DLL build 42"), "status: " + channel.DescribeState(&session));
        Check(CountLog("INFO", "has the client DLL (build 42") == 1, "HELLO not logged");
    }

    void DisabledModuleLeavesTheMessageToTheCore()
    {
        sModuleEnabled = false;
        ClientChannel channel;
        WorldSession session(5);
        Check(channel.OnClientMessage(&session, Hello()), "consumed although the module is disabled");
        Check(session.sent.empty(), "answered although the module is disabled");
        Check(!channel.GetClientDll(&session), "recorded although the module is disabled");
    }

    void MalformedMessagesAreDropped()
    {
        ClientChannel channel;
        WorldSession session(5);

        WorldPacket empty(CMSG_GA_CLIENT_MESSAGE);
        Check(!channel.OnClientMessage(&session, empty), "empty message not consumed");

        WorldPacket shortHello = ClientMessage({ 1, 1 }, uint8(ClientToServerMessage::Hello)); // 9 of 13 bytes
        Check(!channel.OnClientMessage(&session, shortHello), "short HELLO not consumed");

        Check(!channel.OnClientMessage(&session, Hello(0)), "HELLO version 0 not consumed");

        WorldPacket large = ClientMessage(std::vector<uint32>(16, 1), uint8(ClientToServerMessage::Hello)); // 65 bytes
        Check(!channel.OnClientMessage(&session, large), "oversized message not consumed");

        Check(!channel.OnClientMessage(&session, ClientMessage({}, 3)), "SWITCH_ACK not consumed");
        Check(!channel.OnClientMessage(&session, ClientMessage({ 1 }, 200)), "unknown type not consumed");

        Check(session.sent.empty(), "a malformed message was answered");
        Check(!channel.GetClientDll(&session), "a malformed HELLO was recorded");
    }

    void LongerHelloOfANewerDllIsAccepted()
    {
        ClientChannel channel;
        WorldSession session(5);
        WorldPacket hello = ClientMessage({ 3, CLIENT_CHANNEL_CAP_PING, 9, 1234 }, uint8(ClientToServerMessage::Hello));
        Check(!channel.OnClientMessage(&session, hello), "HELLO not consumed");
        std::optional<ClientDllInfo> const dll = channel.GetClientDll(&session);
        Check(dll && dll->protocolVersion == 3, "newer HELLO not recorded");
        Check(dll->EffectiveVersion() == ClientChannelProtocolVersion, "the lower version must apply");
    }

    void CapabilitiesNeedBothSides()
    {
        ClientChannel channel;
        WorldSession session(5);
        channel.OnClientMessage(&session, Hello(1, CLIENT_CHANNEL_CAP_PING | 0x100));
        Check(channel.HasCapability(&session, CLIENT_CHANNEL_CAP_PING), "ping not usable");
        Check(!channel.HasCapability(&session, CLIENT_CHANNEL_CAP_IN_WORLD_SWITCH),
            "in-world switch usable although the DLL does not offer it");
        Check(Contains(channel.DescribeState(&session), "(ping, unknown bits), usable here: ping"),
            "status: " + channel.DescribeState(&session));

        channel.OnClientMessage(&session, Hello(1, CLIENT_CHANNEL_CAP_PING | CLIENT_CHANNEL_CAP_IN_WORLD_SWITCH
            | CLIENT_CHANNEL_CAP_PLAYER_RECORD));
        Check(channel.HasCapability(&session, CLIENT_CHANNEL_CAP_IN_WORLD_SWITCH | CLIENT_CHANNEL_CAP_PLAYER_RECORD),
            "in-world switch and record not usable together");
        Check(Contains(channel.DescribeState(&session), "usable here: ping, in-world switch, player record"),
            "status: " + channel.DescribeState(&session));
    }

    WorldPacket SwitchAck(uint32 nonce, uint32 result)
    {
        return ClientMessage({ nonce, result }, uint8(ClientToServerMessage::SwitchAck));
    }

    void SwitchNeedsTheCapability()
    {
        ClientChannel channel;
        WorldSession session(5);
        uint32 nonce = 0;
        Check(channel.SendSwitch(&session, 0, nonce) == SwitchSendResult::NoClientDll, "SWITCH without HELLO");
        channel.OnClientMessage(&session, Hello(1, CLIENT_CHANNEL_CAP_PING | CLIENT_CHANNEL_CAP_PLAYER_RECORD));
        session.sent.clear();
        Check(channel.SendSwitch(&session, 0, nonce) == SwitchSendResult::NotSupported, "SWITCH to a build-3 DLL");
        Check(session.sent.empty() && nonce == 0, "a packet went to a DLL without the capability");
    }

    void SwitchAndItsAck()
    {
        ClientChannel channel;
        WorldSession session(5);
        channel.OnClientMessage(&session, Hello(1, CLIENT_CHANNEL_CAP_PING | CLIENT_CHANNEL_CAP_IN_WORLD_SWITCH
            | CLIENT_CHANNEL_CAP_PLAYER_RECORD));
        session.sent.clear();

        uint32 nonce = 0;
        Check(channel.SendSwitch(&session, 571, nonce) == SwitchSendResult::Sent && nonce != 0, "SWITCH not sent");
        Check(session.sent.size() == 1 && session.sent[0].size() == 1 + 4 * ClientChannelSwitchFields, "SWITCH size");
        Check(session.sent[0].read<uint8>(0) == uint8(ServerToClientMessage::Switch), "SWITCH type");
        Check(session.sent[0].read<uint32>(1) == 571 && session.sent[0].read<uint32>(5) == nonce, "SWITCH fields");
        Check(!channel.GetSwitchAck(&session, nonce), "an ack before the DLL answered");
        Check(Contains(channel.DescribeState(&session), "in-world switch: 1 sent, 0 armed, last not answered"),
            "status: " + channel.DescribeState(&session));

        channel.OnClientMessage(&session, SwitchAck(nonce + 1, 1));
        Check(!channel.GetSwitchAck(&session, nonce), "an ack with the wrong nonce was taken");

        channel.OnClientMessage(&session, SwitchAck(nonce, uint32(SwitchAckResult::Armed)));
        std::optional<SwitchAckResult> const ack = channel.GetSwitchAck(&session, nonce);
        Check(ack && *ack == SwitchAckResult::Armed, "ack not recorded");
        Check(!channel.GetSwitchAck(&session, nonce + 1), "the ack answers another nonce");

        channel.OnClientMessage(&session, SwitchAck(nonce, uint32(SwitchAckResult::OtherMap)));
        Check(*channel.GetSwitchAck(&session, nonce) == SwitchAckResult::Armed, "a second ack overwrote the first");
        Check(Contains(channel.DescribeState(&session), "in-world switch: 1 sent, 1 armed, last armed"),
            "status: " + channel.DescribeState(&session));

        uint32 second = 0;
        channel.SendSwitch(&session, 0, second);
        Check(second != nonce, "the nonce repeats");
        channel.OnClientMessage(&session, SwitchAck(second, 99));
        Check(*channel.GetSwitchAck(&session, second) == SwitchAckResult::NotOffered, "an unknown result must count "
            "as not offered");
        Check(!channel.GetSwitchAck(&session, nonce), "the old nonce still answers");
    }

    void PingRoundTrip()
    {
        ClientChannel channel;
        WorldSession client(5);
        WorldSession gm(9);
        sWorldSessionMgr->sessions[9] = &gm;

        Check(channel.SendPing(&client, 9) == PingResult::NoClientDll, "ping without HELLO");
        channel.OnClientMessage(&client, Hello());
        client.sent.clear();

        Check(channel.SendPing(&client, 9) == PingResult::Sent, "ping not sent");
        Check(client.sent.size() == 1 && client.sent[0].size() == 5, "PING packet");
        Check(client.sent[0].read<uint8>(0) == uint8(ServerToClientMessage::Ping), "PING type");
        uint32 const nonce = client.sent[0].read<uint32>(1);
        Check(nonce != 0, "nonce 0");
        Check(channel.SendPing(&client, 9) == PingResult::Outstanding, "second ping while the first is open");
        Check(Contains(channel.DescribeState(&client), "ping: unanswered"), "status: " + channel.DescribeState(&client));

        channel.OnClientMessage(&client, Pong(nonce + 1));
        Check(gm.systemMessages.empty(), "a wrong nonce was taken as the answer");

        channel.OnClientMessage(&client, Pong(nonce));
        Check(gm.systemMessages.size() == 1 && Contains(gm.systemMessages[0], "answered the ping in"),
            "the GM was not told");
        Check(Contains(channel.DescribeState(&client), "last round trip"), "status: " + channel.DescribeState(&client));

        channel.OnClientMessage(&client, Pong(nonce));
        Check(gm.systemMessages.size() == 1, "a repeated PONG counted twice");

        Check(channel.SendPing(&client, 9) == PingResult::Sent, "no new ping after the answer");
        Check(client.sent.back().read<uint32>(1) != nonce, "the nonce repeats");
    }

    void PingNeedsTheCapability()
    {
        ClientChannel channel;
        WorldSession session(5);
        channel.OnClientMessage(&session, Hello(1, 0));
        Check(channel.SendPing(&session, 0) == PingResult::NotSupported, "ping although the DLL did not offer it");
    }

    void PongForAnOfflineRequesterIsOnlyLogged()
    {
        ClientChannel channel;
        WorldSession session(5);
        channel.OnClientMessage(&session, Hello());
        channel.SendPing(&session, 77); // account 77 has no session
        channel.OnClientMessage(&session, Pong(session.sent.back().read<uint32>(1)));
        Check(CountLog("INFO", "answered ping") == 1, "answer not logged");
    }

    void NonceIsNeverZero()
    {
        sRandomValue = 0xFFFFFFFF; // the first increment wraps to 0
        ClientChannel channel;
        WorldSession session(5);
        channel.OnClientMessage(&session, Hello());
        channel.SendPing(&session, 0);
        Check(session.sent.back().read<uint32>(1) == 1, "nonce after the wrap");
    }

    void CharacterListRequestForgetsTheDll()
    {
        ClientChannel channel;
        WorldSession session(5);
        channel.OnClientMessage(&session, Hello());
        channel.SendPing(&session, 0);
        uint32 const nonce = session.sent.back().read<uint32>(1);

        channel.OnCharEnumRequest(&session);
        Check(!channel.GetClientDll(&session), "HELLO kept over a character-list request");
        Check(channel.SendPing(&session, 0) == PingResult::NoClientDll, "ping after the character-list request");
        channel.OnClientMessage(&session, Pong(nonce));
        Check(CountLog("INFO", "answered ping") == 0, "a PONG of the forgotten DLL was taken");

        channel.OnClientMessage(&session, Hello());
        Check(channel.GetClientDll(&session).has_value(), "HELLO after the list not recorded");
    }

    void ANewConnectionStartsOver()
    {
        ClientChannel channel;
        WorldSession first(5);
        channel.OnClientMessage(&first, Hello());

        WorldSession second(5); // the same account on a new connection, without a character-list request yet
        Check(!channel.GetClientDll(&second), "the new connection inherited the old HELLO");
        channel.OnClientMessage(&second, Hello(1, CLIENT_CHANNEL_CAP_PING, 8));
        Check(channel.GetClientDll(&second)->dllBuild == 8, "HELLO on the new connection");
        Check(!channel.GetClientDll(&first), "the old connection still counts");
    }

    void HomeSessionsAreIgnored()
    {
        ClientChannel channel;
        WorldSession home(5, true);
        Check(!channel.OnClientMessage(&home, Hello()), "message of a home session not consumed");
        Check(home.sent.empty() && !channel.GetClientDll(&home), "a home session was answered");
    }

    PlayerRecordInfo Testbot()
    {
        PlayerRecordInfo info;
        info.guid = 1;
        info.race = 1;
        info.classId = 2;
        info.gender = 0;
        info.level = 1;
        info.name = "Testbot";
        return info;
    }

    WorldPacket RecordAck(uint32 guidLow, uint32 applied)
    {
        return ClientMessage({ guidLow, applied }, uint8(ClientToServerMessage::PlayerRecordAck));
    }

    void PlayerRecordLayout()
    {
        ClientChannel channel;
        WorldSession session(5);
        channel.OnClientMessage(&session, Hello(1, CLIENT_CHANNEL_CAP_PING | CLIENT_CHANNEL_CAP_PLAYER_RECORD));
        session.sent.clear();

        PlayerRecordInfo info = Testbot();
        info.level = 80;
        info.gender = 1;
        Check(channel.SendPlayerRecord(&session, info) == PlayerRecordResult::Sent, "PLAYER_RECORD not sent");
        Check(session.sent.size() == 1, "no packet");
        WorldPacket const& packet = session.sent[0];
        Check(packet.GetOpcode() == SMSG_GA_SERVER_MESSAGE && packet.size() == 1 + 15 * 4, "PLAYER_RECORD size");
        Check(packet.read<uint8>(0) == uint8(ServerToClientMessage::PlayerRecord), "PLAYER_RECORD type");
        Check(packet.read<uint32>(1) == 1 && packet.read<uint32>(5) == 0, "PLAYER_RECORD guid");
        Check(packet.read<uint32>(9) == (1u | (2u << 8) | (1u << 16) | (80u << 24)), "PLAYER_RECORD identity");
        Check(packet.read<uint8>(13) == 'T' && packet.read<uint8>(19) == 't' && packet.read<uint8>(20) == 0,
            "PLAYER_RECORD name bytes");
        for (std::size_t i = 13 + 7; i < packet.size(); ++i)
            Check(packet.read<uint8>(i) == 0, "PLAYER_RECORD name padding at " + std::to_string(i));
        Check(Contains(channel.DescribeState(&session), "character record: Testbot sent, not confirmed"),
            "status: " + channel.DescribeState(&session));
    }

    void PlayerRecordNeedsTheCapability()
    {
        ClientChannel channel;
        WorldSession session(5);
        Check(channel.SendPlayerRecord(&session, Testbot()) == PlayerRecordResult::NoClientDll, "record without HELLO");
        channel.OnClientMessage(&session, Hello(1, CLIENT_CHANNEL_CAP_PING)); // a build-1 DLL
        session.sent.clear();
        Check(channel.SendPlayerRecord(&session, Testbot()) == PlayerRecordResult::NotSupported,
            "record to a DLL without the capability");
        Check(session.sent.empty(), "a packet went to a DLL without the capability");
        Check(!channel.HasCapability(&session, CLIENT_CHANNEL_CAP_PLAYER_RECORD), "capability without the DLL's bit");
    }

    void InvalidPlayerRecordsAreNotSent()
    {
        ClientChannel channel;
        WorldSession session(5);
        channel.OnClientMessage(&session, Hello(1, CLIENT_CHANNEL_CAP_PING | CLIENT_CHANNEL_CAP_PLAYER_RECORD));
        session.sent.clear();

        std::vector<std::pair<char const*, PlayerRecordInfo>> cases;
        auto add = [&cases](char const* what, auto change)
        {
            PlayerRecordInfo info = Testbot();
            change(info);
            cases.emplace_back(what, info);
        };
        add("guid 0", [](PlayerRecordInfo& info) { info.guid = 0; });
        add("creature guid", [](PlayerRecordInfo& info) { info.guid = 0xF130000000001234ull; });
        add("race 0", [](PlayerRecordInfo& info) { info.race = 0; });
        add("class 0", [](PlayerRecordInfo& info) { info.classId = 0; });
        add("gender 3", [](PlayerRecordInfo& info) { info.gender = 3; });
        add("level 0", [](PlayerRecordInfo& info) { info.level = 0; });
        add("empty name", [](PlayerRecordInfo& info) { info.name.clear(); });
        add("48-byte name", [](PlayerRecordInfo& info) { info.name.assign(48, 'a'); });
        add("control character", [](PlayerRecordInfo& info) { info.name = "Bo\tb"; });

        for (auto const& [what, info] : cases)
            Check(channel.SendPlayerRecord(&session, info) == PlayerRecordResult::Invalid,
                std::string("sent: ") + what);
        Check(session.sent.empty(), "an invalid record was sent");

        PlayerRecordInfo longest = Testbot();
        longest.name.assign(47, 'a');
        Check(channel.SendPlayerRecord(&session, longest) == PlayerRecordResult::Sent, "47-byte name not sent");
    }

    void PlayerRecordAckIsRecorded()
    {
        ClientChannel channel;
        WorldSession session(5);
        channel.OnClientMessage(&session, Hello(1, CLIENT_CHANNEL_CAP_PING | CLIENT_CHANNEL_CAP_PLAYER_RECORD));

        Check(!channel.OnClientMessage(&session, RecordAck(1, 1)), "ack not consumed");
        Check(CountLog("INFO", "wrote the character record") == 0, "an ack without a record was taken");

        channel.SendPlayerRecord(&session, Testbot());
        channel.OnClientMessage(&session, RecordAck(2, 1));
        Check(CountLog("INFO", "wrote the character record") == 0, "an ack for another guid was taken");

        channel.OnClientMessage(&session, RecordAck(1, 1));
        Check(CountLog("INFO", "wrote the character record of Testbot (guid 1)") == 1, "ack not logged");
        Check(Contains(channel.DescribeState(&session), "character record: Testbot written by the DLL"),
            "status: " + channel.DescribeState(&session));

        channel.SendPlayerRecord(&session, Testbot());
        channel.OnClientMessage(&session, RecordAck(1, 0));
        Check(CountLog("WARN", "refused the character record of Testbot") == 1, "refusal not logged");
        Check(Contains(channel.DescribeState(&session), "refused by the DLL"),
            "status: " + channel.DescribeState(&session));

        Check(!channel.OnClientMessage(&session, ClientMessage({ 1 }, uint8(ClientToServerMessage::PlayerRecordAck))),
            "short ack not consumed");
    }

    void RateLimitPerAccount()
    {
        ClientChannel channel;
        WorldSession session(5);
        WorldSession other(6);
        for (int i = 0; i < 25; ++i)
            channel.OnClientMessage(&session, Hello());
        channel.OnClientMessage(&other, Hello());

        Check(session.sent.size() == 20, "answers within one second: " + std::to_string(session.sent.size()));
        Check(CountLog("WARN", "more than 20 client messages per second") == 1, "the rate warning is not logged once");
        Check(other.sent.size() == 1, "another account was limited too");
    }
}

int main()
{
    std::vector<std::pair<char const*, std::function<void()>>> const cases =
    {
        { "the character list announces the channel", CharacterListAnnouncesTheChannel },
        { "no ANNOUNCE while disabled or to home sessions", NoAnnounceWhileDisabledOrForHomeSessions },
        { "HELLO is answered with HELLO_ACK and recorded", HelloIsAnsweredAndRecorded },
        { "disabled module: the message is left to the core's fallback", DisabledModuleLeavesTheMessageToTheCore },
        { "malformed, oversized, reserved and unknown messages are dropped", MalformedMessagesAreDropped },
        { "a longer HELLO of a newer DLL is accepted; the lower version applies", LongerHelloOfANewerDllIsAccepted },
        { "a capability counts only when both sides announce it", CapabilitiesNeedBothSides },
        { "PING / PONG round trip, wrong and repeated nonces", PingRoundTrip },
        { "no ping without the ping capability", PingNeedsTheCapability },
        { "an answer for an offline requester is only logged", PongForAnOfflineRequesterIsOnlyLogged },
        { "the nonce is never 0", NonceIsNeverZero },
        { "a character-list request forgets the DLL", CharacterListRequestForgetsTheDll },
        { "a new connection of the same account starts over", ANewConnectionStartsOver },
        { "home sessions are ignored", HomeSessionsAreIgnored },
        { "rate limit per account", RateLimitPerAccount },
        { "PLAYER_RECORD layout: guid, identity, the name in 12 words", PlayerRecordLayout },
        { "PLAYER_RECORD only to a DLL that announced the capability", PlayerRecordNeedsTheCapability },
        { "SWITCH only to a DLL that announced the in-world switch", SwitchNeedsTheCapability },
        { "SWITCH and its SWITCH_ACK", SwitchAndItsAck },
        { "invalid character records are not sent", InvalidPlayerRecordsAreNotSent },
        { "PLAYER_RECORD_ACK is matched, logged and shown", PlayerRecordAckIsRecorded },
    };

    int passed = 0;
    int failed = 0;
    for (auto const& [name, run] : cases)
    {
        Reset();
        try
        {
            run();
            ++passed;
            std::printf("ok   %s\n", name);
        }
        catch (Failure const& failure)
        {
            ++failed;
            std::printf("FAIL %s: %s\n", name, failure.message.c_str());
        }
        catch (std::exception const& exception)
        {
            ++failed;
            std::printf("FAIL %s: exception %s\n", name, exception.what());
        }
    }

    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
