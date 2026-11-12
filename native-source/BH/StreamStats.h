#pragma once

// StreamStats — Live session stats for the stream overlay debug tab.
// Updated from game thread (deaths, kills) and MCP (status messages).
// Thread-safe: game thread writes counters, MCP writes text, ImGui reads both.

#include <windows.h>
#include <string>
#include <mutex>

namespace StreamStats {

    struct Stats {
        // Counters (game thread updates kills; GameState updates deaths/games)
        int deaths = 0;
        int gamesEntered = 0;
        int monstersKilled = 0;
        int itemsPickedUp = 0;
        int itemsVendored = 0;
        int runsCompleted = 0;
        int uniquesFound = 0;
        int chickens = 0;          // emergency exits

        // Timing
        DWORD sessionStartTime = 0;    // GetTickCount at DLL load
        DWORD currentRunStart = 0;     // GetTickCount at run start
        float lastRunSeconds = 0;

        // Text (set via MCP)
        char status[128] = {};         // "Farming WSK2" / "Vendoring" / "Loading..."
        char lastEvent[128] = {};      // "Found Shako!" / "Died to Lightning Enchanted"
        char funMessage[128] = {};     // rotating audience message
    };

    // Initialize (call once at DLL load)
    void Init();

    // Thread-safe read
    Stats GetStats();

    // Increment counters (call from any thread)
    void RecordDeath();
    void RecordGameEntered();
    void RecordKill();
    void RecordItemPickup();
    void RecordItemVendored();
    void RecordRunComplete();
    void RecordUniqueFound();
    void RecordChicken();

    // Set text fields (call from MCP thread)
    void SetStatus(const char* text);
    void SetLastEvent(const char* text);
    void SetFunMessage(const char* text);

    // Run timing
    void StartRun();
}
