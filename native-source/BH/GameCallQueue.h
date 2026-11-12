#pragma once

#include <functional>

// The action and everything it captures must own its data until it finishes: a
// timed-out invocation may already be running on the engine thread.
namespace GameCallQueue {
    enum class DispatchResult { Completed, Cancelled, Unconfirmed, Failed };

    DispatchResult Invoke(std::function<void()> action, int timeoutMs = 5000);
    void ProcessPending(); // game-loop or out-of-game draw thread only
    void Shutdown();       // reject new work and cancel work not yet running
}
