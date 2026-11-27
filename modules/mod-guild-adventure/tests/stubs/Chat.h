// Test stub of AzerothCore's ChatHandler: system messages land in the session's systemMessages.
#ifndef GA_TEST_STUB_CHAT_H
#define GA_TEST_STUB_CHAT_H

#include "StringFormat.h"
#include "WorldSession.h"
#include <utility>

class ChatHandler
{
public:
    explicit ChatHandler(WorldSession* session) : _session(session) { }

    template <typename... Args>
    void PSendSysMessage(std::format_string<Args...> fmt, Args&&... args)
    {
        _session->systemMessages.push_back(std::format(fmt, std::forward<Args>(args)...));
    }

private:
    WorldSession* _session;
};

#endif
