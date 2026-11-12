#include "StreamStats.h"
#include <cstring>

namespace {
    std::mutex g_mutex;
    StreamStats::Stats g_stats;
}

namespace StreamStats {

    void Init() {
        std::lock_guard<std::mutex> lock(g_mutex);
        memset(&g_stats, 0, sizeof(g_stats));
        g_stats.sessionStartTime = GetTickCount();
        strncpy_s(g_stats.status, "Starting up...", sizeof(g_stats.status));
        strncpy_s(g_stats.lastEvent, "Session began", sizeof(g_stats.lastEvent));
        strncpy_s(g_stats.funMessage, "Claude vs Hell. Place your bets.", sizeof(g_stats.funMessage));
    }

    Stats GetStats() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_stats;
    }

    void RecordDeath()        { std::lock_guard<std::mutex> lock(g_mutex); g_stats.deaths++; }
    void RecordGameEntered()  { std::lock_guard<std::mutex> lock(g_mutex); g_stats.gamesEntered++; }
    void RecordKill()         { std::lock_guard<std::mutex> lock(g_mutex); g_stats.monstersKilled++; }
    void RecordItemPickup()   { std::lock_guard<std::mutex> lock(g_mutex); g_stats.itemsPickedUp++; }
    void RecordItemVendored() { std::lock_guard<std::mutex> lock(g_mutex); g_stats.itemsVendored++; }
    void RecordRunComplete()  { std::lock_guard<std::mutex> lock(g_mutex); g_stats.runsCompleted++; }
    void RecordUniqueFound()  { std::lock_guard<std::mutex> lock(g_mutex); g_stats.uniquesFound++; }
    void RecordChicken()      { std::lock_guard<std::mutex> lock(g_mutex); g_stats.chickens++; }

    void SetStatus(const char* text) {
        std::lock_guard<std::mutex> lock(g_mutex);
        strncpy_s(g_stats.status, text, sizeof(g_stats.status) - 1);
    }

    void SetLastEvent(const char* text) {
        std::lock_guard<std::mutex> lock(g_mutex);
        strncpy_s(g_stats.lastEvent, text, sizeof(g_stats.lastEvent) - 1);
    }

    void SetFunMessage(const char* text) {
        std::lock_guard<std::mutex> lock(g_mutex);
        strncpy_s(g_stats.funMessage, text, sizeof(g_stats.funMessage) - 1);
    }

    void StartRun() {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_stats.currentRunStart > 0) {
            g_stats.lastRunSeconds = (GetTickCount() - g_stats.currentRunStart) / 1000.0f;
        }
        g_stats.currentRunStart = GetTickCount();
    }
}
