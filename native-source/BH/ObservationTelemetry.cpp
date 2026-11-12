#define NOMINMAX
#include "ObservationTelemetry.h"
#include "ServerMonsters.h"
#include "D2DataTables.h"
#include "D2Ptrs.h"
#include "LabProfile.h"

#include <Windows.h>
#include <wincrypt.h>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>

namespace {
using json = nlohmann::json;
std::mutex epochMutex;
std::string processNonce;
std::string activeEpoch;
void* activeGame = nullptr;
DWORD activePlayer = 0;
uint64_t generation = 0;
json cachedRuntimeIdentity;
DWORD identityFrame = 0;

bool Nonce() {
    if (!processNonce.empty()) return true;
    HCRYPTPROV provider = 0;
    BYTE bytes[16] = {};
    const bool ok = CryptAcquireContextA(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptGenRandom(provider, sizeof(bytes), bytes);
    if (provider) CryptReleaseContext(provider, 0);
    if (!ok) return false;
    constexpr char hex[] = "0123456789abcdef";
    processNonce.resize(sizeof(bytes) * 2);
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        processNonce[2 * i] = hex[bytes[i] >> 4];
        processNonce[2 * i + 1] = hex[bytes[i] & 15];
    }
    return true;
}
bool ReadableTableGlobal() {
    const auto* pointer = p_D2COMMON_sgptDataTable;
    if (!pointer) return false;
    MEMORY_BASIC_INFORMATION region = {};
    if (!VirtualQuery(pointer, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
    const uintptr_t end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    return start <= end && sizeof(*pointer) <= end - start;
}

bool ReadablePlayer(const UnitAny* player) {
    if (!player) return false;
    MEMORY_BASIC_INFORMATION region = {};
    if (!VirtualQuery(player, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(player);
    const uintptr_t end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    return start <= end && sizeof(UnitAny) <= end - start;
}

bool Frame(void* game, DWORD& frame) {
    if (!game) return false;
    uintptr_t address = reinterpret_cast<uintptr_t>(game);
    if (address > std::numeric_limits<uintptr_t>::max() - 0xA8 - sizeof(DWORD)) return false;
    const BYTE* frameAddress = reinterpret_cast<const BYTE*>(game) + 0xA8;
    MEMORY_BASIC_INFORMATION region = {};
    if (!VirtualQuery(frameAddress, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
        reinterpret_cast<uintptr_t>(frameAddress) + sizeof(DWORD) >
            reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize) return false;
    frame = *reinterpret_cast<const DWORD*>(frameAddress);
    return true;
}
}

namespace ObservationTelemetry {
void Invalidate() {
    std::lock_guard<std::mutex> lock(epochMutex);
    activeGame = nullptr;
    activePlayer = 0;
    activeEpoch.clear();
    cachedRuntimeIdentity = json();
}

bool MatchesEpoch(const std::string& expectedEpoch) {
    std::lock_guard<std::mutex> lock(epochMutex);
    return !expectedEpoch.empty() && !activeEpoch.empty() && activeGame &&
        activeEpoch == expectedEpoch;
}

bool MatchesContext(const std::string& expectedEpoch, void* game, UnitAny* serverPlayer) {
    std::lock_guard<std::mutex> lock(epochMutex);
    return !expectedEpoch.empty() && !activeEpoch.empty() && game &&
        activeGame == game && ReadablePlayer(serverPlayer) &&
        serverPlayer->dwType == 0 && activePlayer == serverPlayer->dwUnitId &&
        activeEpoch == expectedEpoch;
}

json Capture(void* game, UnitAny* serverPlayer, const ServerMonsters::Request& request) {
    std::lock_guard<std::mutex> lock(epochMutex);
    if (request.operation != ServerMonsters::Operation::Observe || request.unitIds.size() > 16 ||
        !game || !ReadablePlayer(serverPlayer) || serverPlayer->dwType != 0)
        return {{"ok", false}, {"error", "server observation context unavailable"}};
    DWORD start = 0, end = 0;
    if (!Frame(game, start)) return {{"ok", false}, {"error", "server frame unavailable"}};
    if (activeEpoch.empty() || activeGame != game || activePlayer != serverPlayer->dwUnitId) {
        if (!Nonce() || generation == std::numeric_limits<uint64_t>::max())
            return {{"ok", false}, {"error", "session epoch generation unavailable"}};
        activeEpoch = processNonce + "-" + std::to_string(++generation);
        activeGame = game;
        activePlayer = serverPlayer->dwUnitId;
        cachedRuntimeIdentity = json();
    }
    if (!request.expectedEpoch.empty() && request.expectedEpoch != activeEpoch)
        return {{"ok", false}, {"code", "stale_epoch"}, {"error", "session epoch changed"},
            {"session_epoch", activeEpoch}};
    // Validate before ObserveSnapshot's table hashing, not after computing a
    // digest over an incompatible or unreadable SDK record extent.
    const bool statLayoutValid = S13ItemStatCostLayoutValid(
        ReadableTableGlobal() ? *p_D2COMMON_sgptDataTable : nullptr);
    if (!statLayoutValid) cachedRuntimeIdentity = json();
    bool includeIdentity = cachedRuntimeIdentity.is_null() && statLayoutValid;
    json result = ServerMonsters::ObserveSnapshot(game, serverPlayer, request, includeIdentity);
    if (result.value("ok", false)) {
        if (!statLayoutValid) {
            result["runtime_identity"] = {{"tables", {{"item_stats",
                {{"sha256", nullptr}, {"rows", nullptr}, {"covered", false}}}}}};
            result["coverage"]["runtime_data"] = false;
            result["coverage"]["partial"] = true;
        }
        if (includeIdentity && result["coverage"].value("runtime_data", false)) {
            std::string backendDigest;
            if (LabProfile::BackendFileIdentity(backendDigest)) {
                result["runtime_identity"]["backend_file_sha256"] = backendDigest;
                result["runtime_identity"]["backend_identity_scope"] = "loaded_self_module_verified_path_file";
            }
            std::string modDigest;
            if (LabProfile::ModFileIdentity(modDigest)) {
                result["runtime_identity"]["verified_mod_file_sha256"] = modDigest;
                result["runtime_identity"]["mod_file_identity_scope"] =
                    "loaded_module_verified_path_file";
                result["runtime_identity"]["mod_layout_scope"] =
                    "loaded_table_compiler_and_getter_opcode_prefix";
                cachedRuntimeIdentity = result["runtime_identity"];
                identityFrame = start;
            } else {
                result["runtime_identity"]["tables"]["item_stats"] =
                    {{"sha256", nullptr}, {"rows", nullptr}, {"covered", false}};
                result["coverage"]["runtime_data"] = false;
                result["coverage"]["partial"] = true;
            }
        } else if (statLayoutValid && !includeIdentity) {
            result["runtime_identity"] = cachedRuntimeIdentity;
            result["coverage"]["runtime_data"] = true;
        }
        result["runtime_identity_provenance"] = {{"session_epoch", activeEpoch},
            {"observed_frame", includeIdentity || !statLayoutValid ? start : identityFrame},
            {"cached", statLayoutValid && !includeIdentity},
            {"scope", "loaded_static_tables_at_epoch_capture"}};
    }
    if (!Frame(game, end)) return {{"ok", false}, {"error", "server frame unavailable after observation"}};
    result["session_epoch"] = activeEpoch;
    result["frame_start"] = start;
    result["frame_end"] = end;
    result["coherent"] = result.value("ok", false) && start == end;
    if (result.value("ok", false) && start != end) result["coverage"]["partial"] = true;
    return result;
}
}
