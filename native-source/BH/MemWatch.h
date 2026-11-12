#pragma once

// MemWatch - Monitor memory addresses for value changes.
// Polls watched addresses each game frame and records changes.

#include <windows.h>
#include <string>
#include <vector>

namespace MemWatch {

    enum WatchType {
        WATCH_BYTE = 1,
        WATCH_WORD = 2,
        WATCH_DWORD = 4,
        WATCH_FLOAT = 5
    };

    struct WatchEntry {
        std::string name;
        DWORD address;
        WatchType type;
        DWORD currentValue;
        DWORD previousValue;
        bool changed;           // changed since last frame
        int changeCount;        // total changes since added
        DWORD lastChangeTime;
    };

    // Add a watch on an address
    bool AddWatch(const std::string& name, DWORD address, WatchType type = WATCH_DWORD);

    // Remove a watch by name
    bool RemoveWatch(const std::string& name);

    // Remove all watches
    void RemoveAllWatches();

    // Update all watches (call from game loop)
    void Update();

    // Get all watches with current values
    std::vector<WatchEntry> GetWatches();

    // Get count
    int GetWatchCount();
}
