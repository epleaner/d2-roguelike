#include "GameCallQueue.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>

namespace {
    struct Call {
        explicit Call(std::function<void()> fn) : action(std::move(fn)) {}
        std::function<void()> action;
        std::mutex mutex;
        std::condition_variable done;
        bool started = false;
        bool finished = false;
        GameCallQueue::DispatchResult result = GameCallQueue::DispatchResult::Cancelled;
    };

    std::mutex queueMutex;
    std::mutex dispatchMutex;
    std::deque<std::shared_ptr<Call>> pending;
    bool stopped = false;
}

namespace GameCallQueue {
    DispatchResult Invoke(std::function<void()> action, int timeoutMs) {
        if (!action || timeoutMs < 0) return DispatchResult::Failed;
        auto call = std::make_shared<Call>(std::move(action));
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (stopped) return DispatchResult::Cancelled;
            pending.push_back(call);
        }

        std::unique_lock<std::mutex> lock(call->mutex);
        if (!call->done.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                 [&] { return call->finished; })) {
            // Start and cancellation use the same mutex. A queued call that times
            // out can never run; an already-started call has an unknown outcome.
            if (call->started) return DispatchResult::Unconfirmed;
            call->finished = true;
            call->result = DispatchResult::Cancelled;
        }
        return call->result;
    }

    void ProcessPending() {
        std::unique_lock<std::mutex> dispatch(dispatchMutex, std::try_to_lock);
        if (!dispatch.owns_lock()) return;
        for (;;) {
            std::shared_ptr<Call> call;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (stopped || pending.empty()) return;
                call = std::move(pending.front());
                pending.pop_front();
            }
            {
                std::lock_guard<std::mutex> lock(call->mutex);
                if (call->finished) continue;
                call->started = true;
            }
            DispatchResult result = DispatchResult::Completed;
            try { call->action(); }
            catch (...) { result = DispatchResult::Failed; }
            {
                std::lock_guard<std::mutex> lock(call->mutex);
                call->result = result;
                call->finished = true;
            }
            call->done.notify_all();
        }
    }

    void Shutdown() {
        std::deque<std::shared_ptr<Call>> abandoned;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stopped = true;
            abandoned.swap(pending);
        }
        for (const auto& call : abandoned) {
            {
                std::lock_guard<std::mutex> lock(call->mutex);
                call->result = DispatchResult::Cancelled;
                call->finished = true;
            }
            call->done.notify_all();
        }
        // The current engine action, if any, must finish before hook removal.
        std::lock_guard<std::mutex> dispatch(dispatchMutex);
    }
}
