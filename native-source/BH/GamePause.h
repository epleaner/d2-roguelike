#pragma once

// GamePause - Freeze the game loop for frame-by-frame debugging.
// When paused, GameLoop blocks until resumed or stepped.

namespace GamePause {
    // Call from GameLoop — blocks if paused
    void CheckPause();

    // Pause the game loop
    void Pause();

    // Resume normal game loop
    void Resume();

    // Advance one frame then re-pause
    void Step();

    // Is the game currently paused?
    bool IsPaused();

    // Get frame counter
    int GetFrameCount();
}
