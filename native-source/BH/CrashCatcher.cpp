#include "CrashCatcher.h"
#include <psapi.h>
#include <deque>

#pragma comment(lib, "psapi.lib")

static const int MAX_CRASH_LOG = 100;

namespace {
    std::mutex g_mutex;
    std::deque<CrashCatcher::CrashRecord> g_crashLog;
    PVOID g_handler = nullptr;

    // Get the module (DLL) that contains a given address
    bool GetModuleForAddress(DWORD address, char* outName, int nameSize, DWORD* outBase) {
        HMODULE hMods[256];
        DWORD cbNeeded;
        HANDLE hProcess = GetCurrentProcess();

        if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
            int count = cbNeeded / sizeof(HMODULE);
            for (int i = 0; i < count; i++) {
                MODULEINFO mi;
                if (GetModuleInformation(hProcess, hMods[i], &mi, sizeof(mi))) {
                    DWORD base = (DWORD)mi.lpBaseOfDll;
                    DWORD end = base + mi.SizeOfImage;
                    if (address >= base && address < end) {
                        GetModuleBaseNameA(hProcess, hMods[i], outName, nameSize);
                        *outBase = base;
                        return true;
                    }
                }
            }
        }
        outName[0] = '\0';
        *outBase = 0;
        return false;
    }

    // Walk the stack via EBP chain
    int WalkStack(DWORD ebp, DWORD* trace, int maxDepth) {
        int depth = 0;
        for (int i = 0; i < maxDepth; i++) {
            // Validate EBP is readable
            if (IsBadReadPtr((void*)ebp, 8)) break;
            DWORD retAddr = *(DWORD*)(ebp + 4);
            DWORD nextEbp = *(DWORD*)ebp;
            if (retAddr == 0) break;
            trace[depth++] = retAddr;
            if (nextEbp <= ebp) break; // stack grows down, EBP should increase
            ebp = nextEbp;
        }
        return depth;
    }

    // The vectored exception handler
    LONG WINAPI VectoredHandler(PEXCEPTION_POINTERS pExInfo) {
        if (!pExInfo || !pExInfo->ExceptionRecord || !pExInfo->ContextRecord) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        DWORD code = pExInfo->ExceptionRecord->ExceptionCode;

        // Only catch "real" exceptions, not debugger/C++ exceptions
        switch (code) {
            case EXCEPTION_ACCESS_VIOLATION:       // 0xC0000005
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:  // 0xC000008C
            case EXCEPTION_DATATYPE_MISALIGNMENT:  // 0x80000002
            case EXCEPTION_FLT_DIVIDE_BY_ZERO:     // 0xC000008E
            case EXCEPTION_FLT_OVERFLOW:            // 0xC0000091
            case EXCEPTION_ILLEGAL_INSTRUCTION:     // 0xC000001D
            case EXCEPTION_INT_DIVIDE_BY_ZERO:      // 0xC0000094
            case EXCEPTION_INT_OVERFLOW:             // 0xC0000095
            case EXCEPTION_PRIV_INSTRUCTION:         // 0xC0000096
            case EXCEPTION_STACK_OVERFLOW:            // 0xC00000FD
                break;
            default:
                return EXCEPTION_CONTINUE_SEARCH; // not our problem
        }

        CrashCatcher::CrashRecord rec = {};
        rec.exceptionCode = code;
        rec.exceptionAddress = (DWORD)pExInfo->ExceptionRecord->ExceptionAddress;
        rec.timestamp = GetTickCount();
        rec.threadId = GetCurrentThreadId();

        // For access violations, capture the target address
        if (code == EXCEPTION_ACCESS_VIOLATION &&
            pExInfo->ExceptionRecord->NumberParameters >= 2) {
            rec.faultAddress = (DWORD)pExInfo->ExceptionRecord->ExceptionInformation[1];
        }

        // Capture registers
        CONTEXT* ctx = pExInfo->ContextRecord;
        rec.eax = ctx->Eax;
        rec.ebx = ctx->Ebx;
        rec.ecx = ctx->Ecx;
        rec.edx = ctx->Edx;
        rec.esi = ctx->Esi;
        rec.edi = ctx->Edi;
        rec.esp = ctx->Esp;
        rec.ebp = ctx->Ebp;
        rec.eip = ctx->Eip;
        rec.eflags = ctx->EFlags;

        // Walk stack
        rec.stackDepth = WalkStack(ctx->Ebp, rec.stackTrace, 16);

        // Get module name for crash address
        GetModuleForAddress(rec.exceptionAddress, rec.moduleName, sizeof(rec.moduleName), &rec.moduleBase);
        rec.moduleOffset = rec.exceptionAddress - rec.moduleBase;

        // Store in log
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_crashLog.push_back(rec);
            if ((int)g_crashLog.size() > MAX_CRASH_LOG) {
                g_crashLog.pop_front();
            }
        }

        // Let the default handler deal with it (crash normally)
        // To prevent crash: return EXCEPTION_CONTINUE_EXECUTION
        // But that could cause infinite loops on the same instruction
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

namespace CrashCatcher {

    void Init() {
        if (!g_handler) {
            // AddVectoredExceptionHandler(1, ...) = first in chain
            g_handler = AddVectoredExceptionHandler(1, VectoredHandler);
        }
    }

    void Shutdown() {
        if (g_handler) {
            RemoveVectoredExceptionHandler(g_handler);
            g_handler = nullptr;
        }
    }

    std::vector<CrashRecord> GetCrashLog() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return std::vector<CrashRecord>(g_crashLog.begin(), g_crashLog.end());
    }

    void ClearCrashLog() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_crashLog.clear();
    }

    int GetCrashCount() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return (int)g_crashLog.size();
    }

    const char* GetExceptionName(DWORD code) {
        switch (code) {
            case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS";
            case EXCEPTION_DATATYPE_MISALIGNMENT: return "MISALIGNMENT";
            case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIV_ZERO";
            case EXCEPTION_FLT_OVERFLOW:           return "FLT_OVERFLOW";
            case EXCEPTION_ILLEGAL_INSTRUCTION:    return "ILLEGAL_INSTR";
            case EXCEPTION_INT_DIVIDE_BY_ZERO:     return "INT_DIV_ZERO";
            case EXCEPTION_INT_OVERFLOW:            return "INT_OVERFLOW";
            case EXCEPTION_PRIV_INSTRUCTION:        return "PRIV_INSTR";
            case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
            default: return "UNKNOWN";
        }
    }
}
