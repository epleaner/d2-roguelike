#pragma once

// DebugPanel - Separate ImGui+D3D9 debug window for d2-mod-toolkit
// Runs on its own thread with independent D3D9 device and message loop.
// Modeled after D2MOO's D2Debugger architecture.

namespace DebugPanel {
    // Start the debug panel on a background thread.
    // Call once from DllMain or BH init.
    void Init();

    // Signal the panel to shut down and wait for the thread to exit.
    // Call from DllMain DLL_PROCESS_DETACH or BH cleanup.
    void Shutdown();

    // Returns true if the panel window is open and running.
    bool IsRunning();
}
