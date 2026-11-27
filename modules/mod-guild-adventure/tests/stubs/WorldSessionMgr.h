// Test stub of AzerothCore's WorldSessionMgr: FindSession over the sessions a test registers.
#ifndef GA_TEST_STUB_WORLD_SESSION_MGR_H
#define GA_TEST_STUB_WORLD_SESSION_MGR_H

#include "Define.h"
#include <unordered_map>

class WorldSession;

class WorldSessionMgr
{
public:
    static WorldSessionMgr* instance();

    WorldSession* FindSession(uint32 accountId) const
    {
        auto itr = sessions.find(accountId);
        return itr == sessions.end() ? nullptr : itr->second;
    }

    std::unordered_map<uint32, WorldSession*> sessions;
};

#define sWorldSessionMgr WorldSessionMgr::instance()

#endif
