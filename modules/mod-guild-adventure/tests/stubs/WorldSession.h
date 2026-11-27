// Test stub of AzerothCore's WorldSession: an account, the headless flag of a home session, and what was sent.
#ifndef GA_TEST_STUB_WORLD_SESSION_H
#define GA_TEST_STUB_WORLD_SESSION_H

#include "Define.h"
#include "WorldPacket.h"
#include <string>
#include <vector>

class WorldSession
{
public:
    explicit WorldSession(uint32 accountId, bool headless = false) : _accountId(accountId), _headless(headless) { }

    [[nodiscard]] uint32 GetAccountId() const { return _accountId; }
    [[nodiscard]] bool IsHeadless() const { return _headless; }
    void SendPacket(WorldPacket const* packet) { sent.push_back(*packet); }

    std::vector<WorldPacket> sent;
    std::vector<std::string> systemMessages; // filled by the ChatHandler stub

private:
    uint32 _accountId;
    bool _headless;
};

#endif
