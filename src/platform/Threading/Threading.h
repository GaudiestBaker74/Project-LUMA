#pragma once
// =============================================================================
// Platform::Threading — threads, mutexes, semaphores and message queues.
//
// MessageQueue<T> mirrors the Wii OSMessageQueue semantics (FIFO, optional
// capacity, blocking send/receive with timeouts, jam = insert at front). The
// compat/os layer maps OSMutex/OSMessageQueue/OSThread onto this module.
// =============================================================================

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>

namespace Platform::Threading {

// Stable numeric id for the calling thread (for logs and debug).
uint64_t currentThreadId();

// Sets a human-readable name for the current thread (best effort).
void setCurrentThreadName(const char* name);

// A named wrapper around std::thread (the name is a debug aid; on Linux it is
// propagated to the OS thread name via pthread_setname_np where available).
class Thread {
public:
    template <typename F>
    explicit Thread(const char* name, F&& fn) : mName(name ? name : "thread") {
        mThread = std::thread([this, fn = std::forward<F>(fn)]() mutable {
            Platform::Threading::setCurrentThreadName(mName.c_str());
            fn();
        });
    }

    ~Thread() {
        // std::thread destructor terminates if still joinable — that is a
        // programmer error we want to surface loudly.
        if (joinable()) {
            std::terminate();
        }
    }

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    void join() {
        mThread.join();
    }

    void detach() {
        mThread.detach();
    }

    bool joinable() const {
        return mThread.joinable();
    }

    const char* name() const {
        return mName.c_str();
    }

private:
    std::thread mThread;
    std::string mName;
};

// Recursive mutex — RVL's OSMutex is count-based recursive, so this matches
// the semantics the game relies on.
using Mutex = std::recursive_mutex;

// Counting semaphore (C++20).
using Semaphore = std::counting_semaphore<>;

// OSMessageQueue-compatible bounded message queue.
template <typename T>
class MessageQueue {
public:
    explicit MessageQueue(size_t capacity) : mCapacity(capacity) {}

    // Blocks while the queue is full. Returns true on success (the queue can
    // only grow via receive() in another thread; no cancellation for now).
    bool send(const T& message) {
        std::unique_lock<std::mutex> lock(mMutex);
        mNotFull.wait(lock, [this] { return mQueue.size() < mCapacity; });
        mQueue.push_back(message);
        mNotEmpty.notify_one();
        return true;
    }

    // send() with a timeout (nanoseconds). Returns false on timeout.
    bool send(const T& message, uint64_t timeoutNs) {
        std::unique_lock<std::mutex> lock(mMutex);
        if (!mNotFull.wait_for(lock, std::chrono::nanoseconds(timeoutNs), [this] { return mQueue.size() < mCapacity; })) {
            return false;
        }
        mQueue.push_back(message);
        mNotEmpty.notify_one();
        return true;
    }

    // Inserts at the front (like OSJamMessage); blocks while full.
    bool jam(const T& message) {
        std::unique_lock<std::mutex> lock(mMutex);
        mNotFull.wait(lock, [this] { return mQueue.size() < mCapacity; });
        mQueue.push_front(message);
        mNotEmpty.notify_one();
        return true;
    }

    // Non-blocking variants.
    bool trySend(const T& message) {
        std::unique_lock<std::mutex> lock(mMutex);
        if (mQueue.size() >= mCapacity) {
            return false;
        }
        mQueue.push_back(message);
        mNotEmpty.notify_one();
        return true;
    }

    // Blocks while empty. Receives the oldest message (FIFO).
    bool receive(T& out) {
        std::unique_lock<std::mutex> lock(mMutex);
        mNotEmpty.wait(lock, [this] { return !mQueue.empty(); });
        out = mQueue.front();
        mQueue.pop_front();
        mNotFull.notify_one();
        return true;
    }

    bool receive(T& out, uint64_t timeoutNs) {
        std::unique_lock<std::mutex> lock(mMutex);
        if (!mNotEmpty.wait_for(lock, std::chrono::nanoseconds(timeoutNs), [this] { return !mQueue.empty(); })) {
            return false;
        }
        out = mQueue.front();
        mQueue.pop_front();
        mNotFull.notify_one();
        return true;
    }

    bool tryReceive(T& out) {
        std::unique_lock<std::mutex> lock(mMutex);
        if (mQueue.empty()) {
            return false;
        }
        out = mQueue.front();
        mQueue.pop_front();
        mNotFull.notify_one();
        return true;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mQueue.size();
    }

    size_t capacity() const {
        return mCapacity;
    }

    bool empty() const {
        return size() == 0;
    }

private:
    mutable std::mutex mMutex;
    std::condition_variable mNotFull;
    std::condition_variable mNotEmpty;
    std::deque<T> mQueue;
    size_t mCapacity;
};

} // namespace Platform::Threading
