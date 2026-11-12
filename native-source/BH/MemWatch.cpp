#include "MemWatch.h"
#include <map>
#include <mutex>

namespace {
    std::mutex g_mutex;
    std::map<std::string, MemWatch::WatchEntry> g_watches;

    DWORD SafeReadValue(DWORD address, MemWatch::WatchType type) {
        __try {
            switch (type) {
                case MemWatch::WATCH_BYTE:  return *(BYTE*)address;
                case MemWatch::WATCH_WORD:  return *(WORD*)address;
                case MemWatch::WATCH_DWORD: return *(DWORD*)address;
                case MemWatch::WATCH_FLOAT: {
                    float f = *(float*)address;
                    DWORD d; memcpy(&d, &f, 4);
                    return d;
                }
                default: return *(DWORD*)address;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return 0xDEADBEEF;
        }
    }
}

namespace MemWatch {

    bool AddWatch(const std::string& name, DWORD address, WatchType type) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_watches.find(name) != g_watches.end()) return false;

        WatchEntry w = {};
        w.name = name;
        w.address = address;
        w.type = type;
        w.currentValue = SafeReadValue(address, type);
        w.previousValue = w.currentValue;
        w.changed = false;
        w.changeCount = 0;
        w.lastChangeTime = 0;

        g_watches[name] = w;
        return true;
    }

    bool RemoveWatch(const std::string& name) {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_watches.erase(name) > 0;
    }

    void RemoveAllWatches() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_watches.clear();
    }

    void Update() {
        std::lock_guard<std::mutex> lock(g_mutex);
        DWORD now = GetTickCount();
        for (auto& kv : g_watches) {
            auto& w = kv.second;
            DWORD newVal = SafeReadValue(w.address, w.type);
            w.previousValue = w.currentValue;
            w.changed = (newVal != w.currentValue);
            if (w.changed) {
                w.changeCount++;
                w.lastChangeTime = now;
            }
            w.currentValue = newVal;
        }
    }

    std::vector<WatchEntry> GetWatches() {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<WatchEntry> result;
        for (auto& kv : g_watches) {
            result.push_back(kv.second);
        }
        return result;
    }

    int GetWatchCount() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return (int)g_watches.size();
    }
}
