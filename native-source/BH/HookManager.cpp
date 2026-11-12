#include "HookManager.h"
#include <detours.h>
#include <map>
#include <deque>
#include <algorithm>

// Ring buffer size for call log
static const int MAX_CALL_LOG = 10000;

// Must be at file scope (not in anonymous namespace) for inline asm access
extern "C" DWORD g_hookSavedReturnValue = 0;

namespace {
    std::mutex g_mutex;

    // Per-hook runtime data
    struct HookEntry {
        HookManager::HookConfig config;
        void* pOriginal = nullptr;      // pointer to original function (trampoline target)
        void* pDetour = nullptr;        // our detour function
        bool installed = false;
        int callCount = 0;
    };

    std::map<DWORD, HookEntry> g_hooks;

    // Call log ring buffer
    std::deque<HookManager::CallRecord> g_callLog;

    // Thread-local storage for tracking hook context
    // When our detour runs, it needs to know which hook it belongs to
    __declspec(thread) DWORD t_currentHookAddr = 0;
    __declspec(thread) DWORD t_capturedArgs[8] = {};
    __declspec(thread) int t_capturedArgCount = 0;
    __declspec(thread) DWORD t_regEcx = 0;
    __declspec(thread) DWORD t_regEdx = 0;

    void LogCall(DWORD address, DWORD returnValue, bool hasReturn) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_hooks.find(address);
        if (it == g_hooks.end()) return;

        it->second.callCount++;

        if (!it->second.config.logCalls) return;

        HookManager::CallRecord rec = {};
        rec.address = address;
        rec.timestamp = GetTickCount();
        rec.threadId = GetCurrentThreadId();
        rec.returnValue = returnValue;
        rec.hasReturnValue = hasReturn;
        rec.regEcx = t_regEcx;
        rec.regEdx = t_regEdx;

        int nArgs = t_capturedArgCount;
        if (nArgs > 8) nArgs = 8;
        rec.argCount = nArgs;
        for (int i = 0; i < nArgs; i++) {
            rec.args[i] = t_capturedArgs[i];
        }

        g_callLog.push_back(rec);
        if ((int)g_callLog.size() > MAX_CALL_LOG) {
            g_callLog.pop_front();
        }
    }

    // Generic detour function generator
    // We use a single __declspec(naked) detour per hook that captures
    // registers and stack args, calls the original, then logs.
    //
    // Since each hook needs its own detour function address, we generate
    // small thunks in executable memory. Each thunk:
    // 1. Saves registers (ECX, EDX for fastcall)
    // 2. Captures stack arguments
    // 3. Calls the original function via the trampoline
    // 4. Logs the call with return value
    // 5. Returns to caller
    //
    // For simplicity and safety, we'll use a different approach:
    // A fixed set of detour slots (max 64 simultaneous hooks).

    static const int MAX_HOOKS = 64;

    // Forward declarations for detour slots
    struct DetourSlot {
        DWORD hookAddress;
        void** ppOriginal;
        HookManager::CaptureLevel capture;
        int argCount;
        bool active;
    };

    DetourSlot g_slots[MAX_HOOKS] = {};

    // Find a free slot
    int FindFreeSlot() {
        for (int i = 0; i < MAX_HOOKS; i++) {
            if (!g_slots[i].active) return i;
        }
        return -1;
    }

    int FindSlotByAddress(DWORD addr) {
        for (int i = 0; i < MAX_HOOKS; i++) {
            if (g_slots[i].active && g_slots[i].hookAddress == addr) return i;
        }
        return -1;
    }

    // Generic function pointer type for Detours
    typedef void (__cdecl *GenericFunc)();

    // C helper for logging — called from naked asm
    void __cdecl DoLogEntry(int slotIdx, DWORD retVal) {
        if (slotIdx < 0 || slotIdx >= MAX_HOOKS) return;
        DetourSlot& slot = g_slots[slotIdx];
        if (!slot.active) return;
        LogCall(slot.hookAddress, retVal, true);
    }

    // Naked detour per slot.
    // Detours redirects the target function's callers to our detour.
    // The stack is set up exactly as if the caller called us instead of the target.
    // We call the original (trampoline), capture EAX, log, and return.

    // Per-slot original function pointers and log helpers.
    // Written as individual functions to avoid MSVC inline asm macro issues.

    static GenericFunc g_pOriginal_0 = nullptr;
    static void __cdecl DoLog_0(DWORD rv) { DoLogEntry(0, rv); }
    static __declspec(naked) void Detour_0() {
        __asm call [g_pOriginal_0]
        __asm push eax
        __asm pushad
        __asm push eax
        __asm call DoLog_0
        __asm add esp, 4
        __asm popad
        __asm pop eax
        __asm ret
    }

    // Generate remaining slots with the same pattern via a helper macro
    // that uses single-line __asm statements
    #define SLOT_FUNCS(N) \
    static GenericFunc g_pOriginal_##N = nullptr; \
    static void __cdecl DoLog_##N(DWORD rv) { DoLogEntry(N, rv); }

    #define SLOT_DETOUR(N) \
    static __declspec(naked) void Detour_##N() { \
        __asm call [g_pOriginal_##N] \
        __asm push eax \
        __asm pushad \
        __asm push eax \
        __asm call DoLog_##N \
        __asm add esp, 4 \
        __asm popad \
        __asm pop eax \
        __asm ret \
    }

    SLOT_FUNCS(1)  SLOT_DETOUR(1)
    SLOT_FUNCS(2)  SLOT_DETOUR(2)
    SLOT_FUNCS(3)  SLOT_DETOUR(3)
    SLOT_FUNCS(4)  SLOT_DETOUR(4)
    SLOT_FUNCS(5)  SLOT_DETOUR(5)
    SLOT_FUNCS(6)  SLOT_DETOUR(6)
    SLOT_FUNCS(7)  SLOT_DETOUR(7)
    SLOT_FUNCS(8)  SLOT_DETOUR(8)
    SLOT_FUNCS(9)  SLOT_DETOUR(9)
    SLOT_FUNCS(10) SLOT_DETOUR(10)
    SLOT_FUNCS(11) SLOT_DETOUR(11)
    SLOT_FUNCS(12) SLOT_DETOUR(12)
    SLOT_FUNCS(13) SLOT_DETOUR(13)
    SLOT_FUNCS(14) SLOT_DETOUR(14)
    SLOT_FUNCS(15) SLOT_DETOUR(15)
    SLOT_FUNCS(16) SLOT_DETOUR(16)
    SLOT_FUNCS(17) SLOT_DETOUR(17)
    SLOT_FUNCS(18) SLOT_DETOUR(18)
    SLOT_FUNCS(19) SLOT_DETOUR(19)
    SLOT_FUNCS(20) SLOT_DETOUR(20)
    SLOT_FUNCS(21) SLOT_DETOUR(21)
    SLOT_FUNCS(22) SLOT_DETOUR(22)
    SLOT_FUNCS(23) SLOT_DETOUR(23)
    SLOT_FUNCS(24) SLOT_DETOUR(24)
    SLOT_FUNCS(25) SLOT_DETOUR(25)
    SLOT_FUNCS(26) SLOT_DETOUR(26)
    SLOT_FUNCS(27) SLOT_DETOUR(27)
    SLOT_FUNCS(28) SLOT_DETOUR(28)
    SLOT_FUNCS(29) SLOT_DETOUR(29)
    SLOT_FUNCS(30) SLOT_DETOUR(30)
    SLOT_FUNCS(31) SLOT_DETOUR(31)

    // Lookup tables for slot functions
    GenericFunc* g_pOriginals[32] = {
        &g_pOriginal_0,  &g_pOriginal_1,  &g_pOriginal_2,  &g_pOriginal_3,
        &g_pOriginal_4,  &g_pOriginal_5,  &g_pOriginal_6,  &g_pOriginal_7,
        &g_pOriginal_8,  &g_pOriginal_9,  &g_pOriginal_10, &g_pOriginal_11,
        &g_pOriginal_12, &g_pOriginal_13, &g_pOriginal_14, &g_pOriginal_15,
        &g_pOriginal_16, &g_pOriginal_17, &g_pOriginal_18, &g_pOriginal_19,
        &g_pOriginal_20, &g_pOriginal_21, &g_pOriginal_22, &g_pOriginal_23,
        &g_pOriginal_24, &g_pOriginal_25, &g_pOriginal_26, &g_pOriginal_27,
        &g_pOriginal_28, &g_pOriginal_29, &g_pOriginal_30, &g_pOriginal_31,
    };

    GenericFunc g_detours[32] = {
        (GenericFunc)Detour_0,  (GenericFunc)Detour_1,  (GenericFunc)Detour_2,  (GenericFunc)Detour_3,
        (GenericFunc)Detour_4,  (GenericFunc)Detour_5,  (GenericFunc)Detour_6,  (GenericFunc)Detour_7,
        (GenericFunc)Detour_8,  (GenericFunc)Detour_9,  (GenericFunc)Detour_10, (GenericFunc)Detour_11,
        (GenericFunc)Detour_12, (GenericFunc)Detour_13, (GenericFunc)Detour_14, (GenericFunc)Detour_15,
        (GenericFunc)Detour_16, (GenericFunc)Detour_17, (GenericFunc)Detour_18, (GenericFunc)Detour_19,
        (GenericFunc)Detour_20, (GenericFunc)Detour_21, (GenericFunc)Detour_22, (GenericFunc)Detour_23,
        (GenericFunc)Detour_24, (GenericFunc)Detour_25, (GenericFunc)Detour_26, (GenericFunc)Detour_27,
        (GenericFunc)Detour_28, (GenericFunc)Detour_29, (GenericFunc)Detour_30, (GenericFunc)Detour_31,
    };
}

namespace HookManager {

    void Init() {
        // Nothing to initialize — slots are zeroed
    }

    void Shutdown() {
        RemoveAllHooks();
    }

    bool InstallHook(const HookConfig& config) {
        std::lock_guard<std::mutex> lock(g_mutex);

        // Check if already hooked
        if (g_hooks.find(config.address) != g_hooks.end()) {
            return false; // already hooked
        }

        int slot = FindFreeSlot();
        if (slot < 0 || slot >= 32) return false; // no free slots

        // Set up the slot
        g_slots[slot].hookAddress = config.address;
        g_slots[slot].ppOriginal = (void**)g_pOriginals[slot];
        g_slots[slot].capture = config.capture;
        g_slots[slot].argCount = config.argCount;
        g_slots[slot].active = true;

        // Point the original function pointer to the target
        *g_pOriginals[slot] = (GenericFunc)config.address;

        // Install detour
        LONG error;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        error = DetourAttach((PVOID*)g_pOriginals[slot], (PVOID)g_detours[slot]);
        if (error != NO_ERROR) {
            DetourTransactionAbort();
            g_slots[slot].active = false;
            return false;
        }
        error = DetourTransactionCommit();
        if (error != NO_ERROR) {
            g_slots[slot].active = false;
            return false;
        }

        // Record the hook
        HookEntry entry;
        entry.config = config;
        entry.pOriginal = *g_pOriginals[slot];
        entry.pDetour = (void*)g_detours[slot];
        entry.installed = true;
        g_hooks[config.address] = entry;

        return true;
    }

    bool RemoveHook(DWORD address) {
        std::lock_guard<std::mutex> lock(g_mutex);

        auto it = g_hooks.find(address);
        if (it == g_hooks.end()) return false;

        int slot = FindSlotByAddress(address);
        if (slot < 0) return false;

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach((PVOID*)g_pOriginals[slot], (PVOID)g_detours[slot]);
        DetourTransactionCommit();

        g_slots[slot].active = false;
        g_hooks.erase(it);

        return true;
    }

    void RemoveAllHooks() {
        // Copy addresses to avoid iterator invalidation
        std::vector<DWORD> addresses;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto& kv : g_hooks) {
                addresses.push_back(kv.first);
            }
        }
        for (DWORD addr : addresses) {
            RemoveHook(addr);
        }
    }

    std::vector<HookInfo> ListHooks() {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<HookInfo> result;
        for (auto& kv : g_hooks) {
            HookInfo info;
            info.config = kv.second.config;
            info.installed = kv.second.installed;
            info.callCount = kv.second.callCount;
            result.push_back(info);
        }
        return result;
    }

    std::vector<CallRecord> GetCallLog(int maxEntries, DWORD address) {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<CallRecord> result;

        int start = 0;
        if (maxEntries > 0 && (int)g_callLog.size() > maxEntries) {
            start = (int)g_callLog.size() - maxEntries;
        }

        for (int i = start; i < (int)g_callLog.size(); i++) {
            if (address == 0 || g_callLog[i].address == address) {
                result.push_back(g_callLog[i]);
            }
        }
        return result;
    }

    void ClearCallLog() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_callLog.clear();
    }

    int GetCallLogSize() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return (int)g_callLog.size();
    }
}
