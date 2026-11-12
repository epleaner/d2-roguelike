#include "BH.h"
#include "McpServer.h"
#include <Windows.h>

namespace {
    DWORD WINAPI InitializeLab(LPVOID module) {
        BH::Startup(static_cast<HMODULE>(module), nullptr);
        return 0;
    }
}

BOOL WINAPI DllMain(HMODULE instance, DWORD reason, VOID* reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE thread = CreateThread(nullptr, 0, InitializeLab, instance, 0, nullptr);
        if (!thread) return FALSE;
        CloseHandle(thread);
    }
    if (reason == DLL_PROCESS_DETACH && reserved) {
        // Process termination: do not join HTTP workers under loader lock.
        // Release the C++ thread handle before CRT static destruction.
        McpServer::AbandonOnProcessExit();
    }
    return TRUE;
}