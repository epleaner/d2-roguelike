#include "PatchManager.h"
#include "Patch.h"
#include "D2Version.h"

#include <map>
#include <mutex>
#include <sstream>
#include <iomanip>

// Access to nlohmann json — use the ordered version from BH
#include <nlohmann/json.hpp>
using pjson = nlohmann::json;

namespace {
    std::mutex g_mutex;

    struct ManagedPatch {
        std::string name;
        std::string source;    // "bh", "mcp", "file"
        DWORD address;
        int size;
        bool active;
        std::vector<BYTE> originalBytes;
        std::vector<BYTE> patchedBytes;
    };

    std::map<std::string, ManagedPatch> g_patches;

    // Convert bytes to hex string
    std::string BytesToHex(const BYTE* data, int len) {
        std::ostringstream ss;
        for (int i = 0; i < len; i++) {
            if (i > 0) ss << " ";
            ss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (int)data[i];
        }
        return ss.str();
    }

    std::string BytesToHex(const std::vector<BYTE>& data) {
        return BytesToHex(data.data(), (int)data.size());
    }

    // Parse hex string to bytes
    std::vector<BYTE> HexToBytes(const std::string& hex) {
        std::vector<BYTE> bytes;
        for (size_t i = 0; i < hex.size(); i++) {
            if (isspace(hex[i])) continue;
            if (i + 1 < hex.size() && isxdigit(hex[i]) && isxdigit(hex[i + 1])) {
                char tmp[3] = { hex[i], hex[i + 1], 0 };
                bytes.push_back((BYTE)strtoul(tmp, nullptr, 16));
                i++;
            }
        }
        return bytes;
    }

    // Read bytes from address with protection
    bool SafeRead(DWORD addr, BYTE* buf, int len) {
        __try {
            memcpy(buf, (void*)addr, len);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Write bytes to address with VirtualProtect
    bool SafeWrite(DWORD addr, const BYTE* buf, int len) {
        DWORD oldProt;
        if (!VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &oldProt))
            return false;
        memcpy((void*)addr, buf, len);
        VirtualProtect((void*)addr, len, oldProt, &oldProt);
        return true;
    }

    // DLL name table (matches Patch.h Dll enum)
    const char* DllNames[] = {
        "D2CLIENT.dll", "D2COMMON.dll", "D2GFX.dll", "D2LANG.dll",
        "D2WIN.dll", "D2NET.dll", "D2GAME.dll", "D2LAUNCH.dll",
        "FOG.dll", "BNCLIENT.dll", "STORM.dll", "D2CMP.dll",
        "D2MULTI.dll", "D2MCPCLIENT.dll", "D2SOUND.dll"
    };
}

namespace PatchManager {

    void Init() {
        // BH patches are tracked via the static Patch::Patches vector
        // but it's private. We can't access it directly.
        // Instead, we'll just track patches applied through our API.
        // BH's patches are already managed by the Patch class itself.
    }

    bool ApplyPatch(const std::string& name, DWORD address, const BYTE* bytes, int size) {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (g_patches.find(name) != g_patches.end()) {
            return false; // already exists
        }

        ManagedPatch mp;
        mp.name = name;
        mp.source = "mcp";
        mp.address = address;
        mp.size = size;
        mp.originalBytes.resize(size);
        mp.patchedBytes.assign(bytes, bytes + size);

        // Read original bytes
        if (!SafeRead(address, mp.originalBytes.data(), size)) {
            return false;
        }

        // Write patch
        if (!SafeWrite(address, bytes, size)) {
            return false;
        }

        mp.active = true;
        g_patches[name] = mp;
        return true;
    }

    bool RevertPatch(const std::string& name) {
        std::lock_guard<std::mutex> lock(g_mutex);

        auto it = g_patches.find(name);
        if (it == g_patches.end()) return false;

        auto& mp = it->second;
        if (!mp.active) return true; // already reverted

        if (!SafeWrite(mp.address, mp.originalBytes.data(), mp.size)) {
            return false;
        }

        mp.active = false;
        return true;
    }

    bool TogglePatch(const std::string& name) {
        std::lock_guard<std::mutex> lock(g_mutex);

        auto it = g_patches.find(name);
        if (it == g_patches.end()) return false;

        auto& mp = it->second;
        if (mp.active) {
            // Revert
            if (!SafeWrite(mp.address, mp.originalBytes.data(), mp.size))
                return false;
            mp.active = false;
        } else {
            // Re-apply
            if (!SafeWrite(mp.address, mp.patchedBytes.data(), mp.size))
                return false;
            mp.active = true;
        }
        return true;
    }

    std::vector<PatchInfo> ListPatches() {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<PatchInfo> result;

        for (auto& kv : g_patches) {
            auto& mp = kv.second;
            PatchInfo pi;
            pi.name = mp.name;
            pi.source = mp.source;
            pi.address = mp.address;
            pi.size = mp.size;
            pi.active = mp.active;
            pi.originalHex = BytesToHex(mp.originalBytes);
            pi.patchedHex = BytesToHex(mp.patchedBytes);
            result.push_back(pi);
        }
        return result;
    }

    int ImportPatches(const std::string& jsonStr) {
        int count = 0;
        try {
            pjson patches = pjson::parse(jsonStr);
            if (!patches.is_array()) return 0;

            for (auto& p : patches) {
                std::string name = p.value("name", "");
                std::string addrStr = p.value("address", "");
                std::string bytesStr = p.value("bytes", "");

                if (name.empty() || addrStr.empty() || bytesStr.empty()) continue;

                DWORD addr = (DWORD)strtoul(addrStr.c_str(), nullptr, 16);
                std::vector<BYTE> bytes = HexToBytes(bytesStr);

                if (addr == 0 || bytes.empty()) continue;

                // Mark as file source
                if (ApplyPatch(name, addr, bytes.data(), (int)bytes.size())) {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_patches[name].source = "file";
                    count++;
                }
            }
        } catch (...) {}
        return count;
    }

    int GetPatchCount() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return (int)g_patches.size();
    }
}
