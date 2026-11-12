#pragma once

// CrashCatcher - Captures crash/exception data instead of just dying.
// Uses Vectored Exception Handler to intercept access violations,
// stack overflows, and other fatal exceptions.

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>

namespace CrashCatcher {

    struct CrashRecord {
        DWORD exceptionCode;
        DWORD exceptionAddress;
        DWORD faultAddress;        // for access violations: the address accessed
        DWORD timestamp;
        DWORD threadId;
        // Registers
        DWORD eax, ebx, ecx, edx;
        DWORD esi, edi, esp, ebp;
        DWORD eip, eflags;
        // Stack trace (up to 16 return addresses)
        DWORD stackTrace[16];
        int stackDepth;
        // Module info
        char moduleName[64];       // which DLL the crash was in
        DWORD moduleBase;
        DWORD moduleOffset;        // crash address relative to module base
    };

    // Install the vectored exception handler
    void Init();

    // Remove the handler
    void Shutdown();

    // Get captured crash records
    std::vector<CrashRecord> GetCrashLog();

    // Clear the crash log
    void ClearCrashLog();

    // Get number of crashes captured
    int GetCrashCount();

    // Get exception code as readable string
    const char* GetExceptionName(DWORD code);
}
