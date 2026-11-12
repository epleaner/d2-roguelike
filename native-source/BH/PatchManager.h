#pragma once

// PatchManager - Track, toggle, and manage binary patches.
// Integrates with BH's existing Patch system and supports
// runtime patches and JSON patch file imports.

#include <windows.h>
#include <string>
#include <vector>

namespace PatchManager {

    struct PatchInfo {
        std::string name;
        std::string source;     // "bh", "mcp", "file"
        DWORD address;
        int size;
        bool active;
        std::string originalHex;
        std::string patchedHex;
    };

    // Initialize — scans BH's existing patches
    void Init();

    // Apply a named patch at an address
    // Returns true on success
    bool ApplyPatch(const std::string& name, DWORD address, const BYTE* bytes, int size);

    // Revert a patch by name (restore original bytes)
    bool RevertPatch(const std::string& name);

    // Toggle a patch on/off
    bool TogglePatch(const std::string& name);

    // List all known patches
    std::vector<PatchInfo> ListPatches();

    // Import patches from a JSON string
    // Format: [{"name":"...", "address":"0x...", "bytes":"90 90 90"}]
    int ImportPatches(const std::string& jsonStr);

    // Get patch count
    int GetPatchCount();
}
