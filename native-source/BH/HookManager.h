#pragma once

// HookManager - Dynamic function hooking with configurable call logging.
// Uses Microsoft Detours for trampoline-based inline hooks.
// Hooks can be installed/removed at runtime via MCP or ImGui.

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>

namespace HookManager {

    // What to capture per hook call
    enum CaptureLevel {
        CAPTURE_LIGHT = 0,    // address, timestamp, thread ID
        CAPTURE_MEDIUM = 1,   // + first 4 stack args
        CAPTURE_FULL = 2      // + all args (configurable depth), return value, registers
    };

    // A single logged function call
    struct CallRecord {
        DWORD address;          // hooked function address
        DWORD timestamp;        // GetTickCount
        DWORD threadId;
        DWORD args[8];          // stack arguments (up to 8)
        int argCount;           // how many args were captured
        DWORD returnValue;      // EAX after function returns
        bool hasReturnValue;
        DWORD regEcx;           // for __fastcall first arg
        DWORD regEdx;           // for __fastcall second arg
    };

    // Configuration for a single hook
    struct HookConfig {
        DWORD address = 0;          // function address to hook
        std::string name;           // user-friendly name
        CaptureLevel capture = CAPTURE_LIGHT;
        int argCount = 0;           // number of args to capture (for MEDIUM/FULL)
        bool enabled = true;
        bool logCalls = true;       // whether to log calls to the ring buffer
    };

    // Hook status info
    struct HookInfo {
        HookConfig config;
        bool installed = false;
        int callCount = 0;
    };

    // Initialize the hook manager
    void Init();

    // Shutdown — remove all hooks
    void Shutdown();

    // Install a hook at the given address
    // Returns true on success
    bool InstallHook(const HookConfig& config);

    // Remove a hook by address
    bool RemoveHook(DWORD address);

    // Remove all hooks
    void RemoveAllHooks();

    // List all installed hooks
    std::vector<HookInfo> ListHooks();

    // Get recent call log entries
    // maxEntries: how many to return (0 = all available)
    // address: filter by hook address (0 = all hooks)
    std::vector<CallRecord> GetCallLog(int maxEntries = 100, DWORD address = 0);

    // Clear the call log
    void ClearCallLog();

    // Get total number of logged calls
    int GetCallLogSize();
}
