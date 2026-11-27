/*
 * ".ga perf": the planner's counters of the last minute (Perf.h, docs/performance.md PF-0).
 */

#include "Chat.h"
#include "CommandScript.h"
#include "GuildAdventureConfig.h"
#include "OrderService.h"
#include "PathProbe.h"
#include "Perf.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include <optional>

using namespace Acore::ChatCommands;
using namespace GuildAdventure;

namespace
{
    class GuildAdventurePerfCommandScript : public CommandScript
    {
    public:
        GuildAdventurePerfCommandScript() : CommandScript("GuildAdventurePerfCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable gaTable =
            {
                { "perf", HandlePerf, SEC_GAMEMASTER, Console::Yes }
            };
            static ChatCommandTable commandTable =
            {
                { "ga", gaTable }
            };
            return commandTable;
        }

        // .ga perf [clear]: the lines of OrderService::DescribePerf; "clear" empties the walk cache (a test aid: the next
        // route searches ask the path finder again).
        static bool HandlePerf(ChatHandler* handler, Optional<std::string> what)
        {
            if (!IsModuleEnabled())
            {
                handler->SendErrorMessage("Guild Adventure is disabled (GuildAdventure.Enable = 0).", false);
                return false;
            }
            if (what && *what == "clear")
            {
                std::size_t const size = WalkCacheSize();
                ClearWalkCache();
                handler->SendSysMessage(Acore::StringFormat("The walk cache is empty again ({} walks forgotten).", size));
                return true;
            }
            if (what && !what->empty())
            {
                handler->SendErrorMessage("Usage: .ga perf [clear]", false);
                return false;
            }
            for (std::string const& line : sOrderService.DescribePerf())
                handler->SendSysMessage(line);
            return true;
        }
    };
}

void AddGuildAdventurePerfScripts()
{
    new GuildAdventurePerfCommandScript();
}
