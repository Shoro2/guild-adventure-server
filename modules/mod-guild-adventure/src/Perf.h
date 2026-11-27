/*
 * Perf: the planner's own counters (docs/performance.md, package PF-0).
 *
 * The order planner (OrderService) runs on the world thread, and while it works the map threads wait: its time is on
 * the critical path of every world tick. These counters say what it costs without a profiler: the time per world tick,
 * the route searches (RoadMap::FindRoute) and the path finder's walks (WalkReaches) under them, how many of those the
 * cache answered (PF-2), and the expensive searches (FindFlight, FindErrand, ChooseZone). OrderService keeps the last
 * minute of world ticks; ".ga perf" prints it (Perf.cpp).
 *
 * The counters only grow; readers take differences. Atomics, so code on a map thread may add as well.
 */

#ifndef MOD_GUILD_ADVENTURE_PERF_H
#define MOD_GUILD_ADVENTURE_PERF_H

#include "Define.h"
#include <atomic>
#include <chrono>

namespace GuildAdventure::Perf
{
    inline uint64 NowUs()
    {
        return uint64(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    inline uint64 NowMs() { return NowUs() / 1000; }

    struct Counters
    {
        std::atomic<uint64> routeSearches{ 0 };  // RoadMap::FindRoute calls
        std::atomic<uint64> routeUs{ 0 };        // ... and their time (the path finder's walks under them included)
        std::atomic<uint64> walkProbes{ 0 };     // WalkReaches answered by the path finder
        std::atomic<uint64> walkUs{ 0 };         // ... and their time
        std::atomic<uint64> walkHits{ 0 };       // WalkReaches answered from the cache (PF-2)
        std::atomic<uint64> flights{ 0 };        // OrderService::FindFlight
        std::atomic<uint64> errands{ 0 };        // OrderService::FindErrand
        std::atomic<uint64> zonePicks{ 0 };      // OrderService::ChooseZone

        [[nodiscard]] uint64 Expensive() const
        {
            return flights.load(std::memory_order_relaxed) + errands.load(std::memory_order_relaxed) +
                zonePicks.load(std::memory_order_relaxed);
        }
    };

    inline Counters& Get()
    {
        static Counters counters;
        return counters;
    }

    // Adds the microseconds of its scope to a counter.
    class ScopedUs
    {
    public:
        explicit ScopedUs(std::atomic<uint64>& sink) : _sink(sink), _start(NowUs()) { }
        ~ScopedUs() { _sink.fetch_add(NowUs() - _start, std::memory_order_relaxed); }
        ScopedUs(ScopedUs const&) = delete;
        ScopedUs& operator=(ScopedUs const&) = delete;

    private:
        std::atomic<uint64>& _sink;
        uint64 _start;
    };
}

#endif
