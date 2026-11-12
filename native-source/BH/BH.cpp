#define _DEFINE_PTRS
#include "BH.h"
#include "D2Version.h"
#include "D2Intercepts.h"
#include "D2Helpers.h"
#include "D2Ptrs.h"
#include "GameCallQueue.h"
#include "LabProfile.h"
#include "McpServer.h"
#include "ObservationTelemetry.h"
#include "CombatTelemetry.h"

#include <cstring>
#include <cstdint>
#include <cwchar>

string BH::path;
HINSTANCE BH::instance = nullptr;
ModuleManager* BH::moduleManager = nullptr;
Config* BH::lootFilter = nullptr;
Drawing::UI* BH::settingsUI = nullptr;
Drawing::StatsDisplay* BH::statsDisplay = nullptr;
bool BH::initialized = false;
bool BH::cGuardLoaded = false;
WNDPROC BH::OldWNDPROC = nullptr;
Patch* BH::oogDraw = nullptr;
BHApp App;

namespace ServerSpawns { void ShutdownMarketPriceHook(); }
namespace {
    Patch* gameLoop = nullptr;
    Patch* runConsumableName = nullptr;
    bool startupApproved = false;

    // Measured from the SHA-256-pinned S13 patch2 D2Client.dll and D2Win.dll
    // disk images (1.13c first-column offsets), not from the S12 PD2 addon.
    constexpr BYTE kGameLoopSite[] = { 0x83, 0xEC, 0x20, 0x89, 0x4C, 0x24, 0x0C };
    constexpr BYTE kOogDrawSite[] = { 0x8B, 0x76, 0x3C, 0x85, 0xF6 };
    // Client inventory-name callsite: mov al,[ebp+12a], after stock name
    // construction. BH's full Item module is intentionally not initialized.
    constexpr BYTE kRunNameSite[] = { 0x8A, 0x85, 0x2A, 0x01, 0x00, 0x00 };

    void __fastcall FixRunConsumableName(wchar_t* name, UnitAny* item, int size) {
        if (!name || !item || item->dwType != UNIT_ITEM || size < 8)
            return;
        const ItemsTxt* text = D2COMMON_GetItemText(item->dwTxtFileNo);
        if (!text)
            return;
        const char* code = text->szCode;
        const bool runConsumable =
            (code[0] == 'r' &&
             (!std::memcmp(code, "rpr", 3) || !std::memcmp(code, "rrl", 3))) ||
            (code[0] == 'q' &&
             (!std::memcmp(code, "qbs", 3) || !std::memcmp(code, "qmt", 3) ||
              !std::memcmp(code, "qsh", 3) || !std::memcmp(code, "qch", 3) ||
              !std::memcmp(code, "qeq", 3) || !std::memcmp(code, "qso", 3) ||
              !std::memcmp(code, "qun", 3) || !std::memcmp(code, "qse", 3) ||
              !std::memcmp(code, "qjw", 3) || !std::memcmp(code, "qcm", 3)));
        if (!runConsumable ||
            (code[3] != ' ' && code[3] != '\0'))
            return;
        const wchar_t* localized = GetTblEntryByIndex(text->wnamestr, TBLOFFSET_STRING);
        if (localized && *localized)
            wcsncpy_s(name, static_cast<size_t>(size < 128 ? size : 128) - 4, localized, _TRUNCATE);
    }

    void __declspec(naked) RunConsumableNameIntercept() {
        __asm {
            mov ecx, edi
            mov edx, ebx
            push [esp + 0x1bec]
            call FixRunConsumableName
            mov al, [ebp + 0x12a]
            ret
        }
    }


    bool MatchesSite(const char* module, DWORD rva, const BYTE* signature, size_t length) {
        HMODULE image = GetModuleHandleA(module);
        if (!image) return false;
        BYTE actual[sizeof(kGameLoopSite)];
        SIZE_T read = 0;
        return ReadProcessMemory(GetCurrentProcess(),
                                 reinterpret_cast<const BYTE*>(image) + rva,
                                 actual, length, &read) && read == length &&
               std::memcmp(actual, signature, length) == 0;
    }

    bool InstalledAt(const char* module, DWORD rva, const void* target, size_t length) {
        HMODULE image = GetModuleHandleA(module);
        if (!image) return false;
        const BYTE* address = reinterpret_cast<const BYTE*>(image) + rva;
        BYTE actual[sizeof(kGameLoopSite)];
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), address, actual, length, &read) || read != length)
            return false;
        if (actual[0] != 0xE8 ||
            *reinterpret_cast<const DWORD*>(actual + 1) !=
                static_cast<DWORD>(reinterpret_cast<uintptr_t>(target) -
                                   reinterpret_cast<uintptr_t>(address + 5)))
            return false;
        for (size_t i = 5; i < length; ++i) if (actual[i] != 0x90) return false;
        return true;
    }
}

bool BH::Startup(HINSTANCE module, VOID* reserved) {
    if (reserved) return false;
    instance = module;
    char dllPath[MAX_PATH];
    DWORD length = GetModuleFileNameA(module, dllPath, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    path.assign(dllPath, length);
    const size_t slash = path.find_last_of("\\/");
    if (slash == string::npos) return false;
    path.resize(slash + 1);

    // Defer startup out of DllMain's loader lock. The game may still be loading
    // the DLLs when our bootstrap thread first runs; fail closed after 30 sec.
    constexpr const char* requiredModules[] = {
        "D2Client.dll", "D2Common.dll", "D2Win.dll", "D2Launch.dll",
        "D2Gfx.dll", "D2Lang.dll", "ProjectDiablo.dll"
    };
    for (int attempt = 0; attempt < 300; ++attempt) {
        bool loaded = true;
        for (const char* name : requiredModules)
            if (!GetModuleHandleA(name)) { loaded = false; break; }
        if (loaded) break;
        Sleep(100);
    }

    std::string error;
    if (!LabProfile::LoadAndVerify(error)) {
        OutputDebugStringA(("Native lab refused startup: " + error + "\n").c_str());
        return false;
    }
    if (!MatchesSite("D2Client.dll", 0x44230, kGameLoopSite, sizeof(kGameLoopSite)) ||
        !MatchesSite("D2Client.dll", 0x92366, kRunNameSite, sizeof(kRunNameSite)) ||
        !MatchesSite("D2Win.dll", 0x18911, kOogDrawSite, sizeof(kOogDrawSite))) {
        OutputDebugStringA("Native lab refused unrecognized hook bytes\n");
        return false;
    }

    // Hashes and both executable sites have passed. Pin this DLL for the
    // process lifetime: an external FreeLibrary must not unload active hooks.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN |
                            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCSTR>(&BH::Initialize), &pinned)) return false;
    startupApproved = true;
    Initialize();
    return initialized;
}

void BH::Initialize() {
    if (!startupApproved || initialized) return;
    // Patch::Install selects offsets using this ID; the checked game is 1.13c.
    // Avoid D2Version::Init's relative Game.exe lookup and exit(0) fallback.
    D2Version::versionID = VERSION_113c;
    gameLoop = new Patch(Call, D2CLIENT, { 0x44230, 0x45280 },
                         reinterpret_cast<int>(GameLoop_Interception), sizeof(kGameLoopSite));
    oogDraw = new Patch(Call, D2WIN, { 0x18911, 0xEC61 },
                        reinterpret_cast<int>(OOGDraw_Interception), sizeof(kOogDrawSite));
    runConsumableName = new Patch(Call, D2CLIENT, { 0x92366, 0x96736 },
        reinterpret_cast<int>(RunConsumableNameIntercept), sizeof(kRunNameSite));
    if (!gameLoop->Install() ||
        !InstalledAt("D2Client.dll", 0x44230,
                     reinterpret_cast<const void*>(GameLoop_Interception), sizeof(kGameLoopSite)) ||
        !oogDraw->Install() ||
        !InstalledAt("D2Win.dll", 0x18911,
                     reinterpret_cast<const void*>(OOGDraw_Interception), sizeof(kOogDrawSite)) ||
        !runConsumableName->Install() ||
        !InstalledAt("D2Client.dll", 0x92366,
                     reinterpret_cast<const void*>(RunConsumableNameIntercept), sizeof(kRunNameSite))) {
        runConsumableName->Remove();
        oogDraw->Remove();
        gameLoop->Remove();
        startupApproved = false;
        OutputDebugStringA("Native lab could not install checked engine hooks\n");
        return;
    }
    initialized = true;
}

bool BH::Shutdown() {
    if (!initialized) return true;
    // Only an explicit call outside loader lock may perform this shutdown.
    CombatTelemetry::Shutdown();
    ObservationTelemetry::Invalidate();
    ServerSpawns::ShutdownMarketPriceHook();
    GameCallQueue::Shutdown();
    if (oogDraw) oogDraw->Remove();
    if (runConsumableName) runConsumableName->Remove();
    if (gameLoop) gameLoop->Remove();
    startupApproved = false;
    initialized = false;
    return true;
}

extern "C" __declspec(dllexport) int __cdecl BHIsReady() {
    return BH::initialized;
}
