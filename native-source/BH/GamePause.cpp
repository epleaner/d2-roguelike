#include "GamePause.h"
#include <windows.h>
#include <atomic>

namespace {
    std::atomic<bool> g_paused{false};
    std::atomic<bool> g_stepRequested{false};
    std::atomic<int> g_frameCount{0};
}

namespace GamePause {

    void CheckPause() {
        g_frameCount++;

        if (!g_paused) return;

        // If step was requested, allow one frame then re-pause
        if (g_stepRequested) {
            g_stepRequested = false;
            return; // let this frame execute
        }

        // Block the game loop while paused
        // Sleep in small intervals so we can respond to resume/step
        while (g_paused && !g_stepRequested) {
            Sleep(10);
        }

        // If we woke up from a step request, don't clear paused
        // The next call to CheckPause will handle it
    }

    void Pause() {
        g_paused = true;
    }

    void Resume() {
        g_paused = false;
        g_stepRequested = false;
    }

    void Step() {
        if (g_paused) {
            g_stepRequested = true;
        }
    }

    bool IsPaused() {
        return g_paused;
    }

    int GetFrameCount() {
        return g_frameCount;
    }
}
