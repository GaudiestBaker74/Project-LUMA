// compat/os — OSThread/OSMessageQueue host layer (M9.1).
//
// The game's threading needs the real SDK surface: JKRThread (and everything
// on top: JASDvdThread, JASAudioThread, FunctionAsyncExecutor, thread-switch)
// calls OSCreateThread/OSResumeThread/OSMessageQueue* on real OSThread objects
// with real block/resume semantics. This file implements that surface on top
// of std::thread.
//
// Faithful parts: object model (OSThread/OSThreadQueue/OSMessageQueue structs
// from the vendored headers), start/resume semantics (a thread is created
// suspended; OSResumeThread starts it — every game caller does resume()),
// blocking/unblocking message queues (OS_MESSAGE_BLOCK/NOBLOCK), sleep/wakeup
// thread queues, exit/join/cancel/detach.
//
// Documented approximations (TODO(PC_PORT)):
//   - OSSuspendThread cannot preempt a running host thread mid-execution; it
//     takes effect at the thread's next blocking/OS call (PC semantics).
//   - Priorities are stored, not enforced (std::thread has no portable
//     priority); the game only uses them for naming/order hints.
//   - `attr` is stored but not used: no auto-start (all callers resume()).
//
// Uses the Meyers-singleton registry pattern of OSMutex.cpp for the side
// tables (no static-init-order hazards).
#include "platform/Threading/Threading.h"
#include "platform/Timing/Timing.h"

#include <revolution/os.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {

// --- host side of one OSThread ------------------------------------------------
struct HostThread {
    ~HostThread() {
        // A joinable std::thread aborts the process when destroyed; join when
        // the OS thread is done, otherwise detach (exit-time safety only —
        // the game joins every thread at shutdown).
        if (th.joinable()) {
            if (terminated) {
                th.join();
            } else {
                th.detach();
            }
        }
    }
    std::thread th;                 // valid once `started`
    std::mutex m;
    std::condition_variable cv;
    bool started = false;
    bool terminated = false;        // func returned or OSExitThread
    bool sleeping = false;
    bool wakeRequested = false;
    bool cancelRequested = false;
    void* (*func)(void*) = nullptr;
    void* arg = nullptr;
    void* exitValue = nullptr;
};

std::mutex& registryMutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<OSThread*, std::unique_ptr<HostThread>>& threadRegistry() {
    static std::unordered_map<OSThread*, std::unique_ptr<HostThread>> r;
    return r;
}

// Caller must hold registryMutex(). Kept separate from getHost(): most OS
// entry points already lock the registry around their whole body.
HostThread* findHostUnlocked(OSThread* t) {
    auto it = threadRegistry().find(t);
    return it == threadRegistry().end() ? nullptr : it->second.get();
}

// PC_PORT (M9.5.3d): locking lookup. The unlocked reads (thread trampolines,
// OSJoinThread/OSDetachThread) raced OSCreateThread's insert — an unordered_map
// find during a rehash returns garbage HostThread* whose writes then corrupt
// arbitrary heap (the "Bad Block"/pipeline-cache crashes at the Logo->Title
// transition; TSAN: 37 reports on this registry).
HostThread* getHost(OSThread* t) {
    std::lock_guard<std::mutex> lock(registryMutex());
    return findHostUnlocked(t);
}

// Run the trampoline (marks termination, never throws across the boundary).
thread_local OSThread* sCurrentTLS = nullptr; // single TLS: trampoline + getter

void runTrampoline(OSThread* t) {
    sCurrentTLS = t; // OSGetCurrentThread()
    HostThread* h = getHost(t);
    if (h == nullptr) {
        return;
    }
    void* ret = nullptr;
    if (h->func != nullptr) {
        try {
            ret = h->func(h->arg);
        } catch (...) {
            ret = nullptr; // never propagate across a C boundary
        }
    }
    {
        std::lock_guard<std::mutex> lock(h->m);
        h->exitValue = ret;
        h->terminated = true;
    }
    h->cv.notify_all();
}

// --- message-queue side state -------------------------------------------------
struct HostMsgQueue {
    std::mutex m;
    std::condition_variable cv;
};

std::mutex& msgQueueMutex() {
    static std::mutex m;
    return m;
}

// The HostMsgQueue objects are intentionally NEVER destroyed: the game's
// worker threads (FunctionAsyncExecutor, audio driver) can still be blocked
// in OSReceiveMessage when the process exits. Destroying a
// std::condition_variable that still has waiters is undefined behaviour and
// hangs at exit (observed: the registry's static destructor stalled in
// pthread_cond_destroy after the boot test). The map itself is torn down with
// the process; the few-byte queue records leak, which is fine at exit time.
std::unordered_map<OSMessageQueue*, HostMsgQueue*>& msgQueueRegistry() {
    static std::unordered_map<OSMessageQueue*, HostMsgQueue*> r;
    return r;
}

HostMsgQueue* getMsgHost(OSMessageQueue* q) {
    auto it = msgQueueRegistry().find(q);
    return it == msgQueueRegistry().end() ? nullptr : it->second;
}

} // namespace

extern "C" {

// --- thread lifecycle ---------------------------------------------------------

void __OSThreadInit(void) {}

BOOL OSCreateThread(OSThread* thread, void* (*func)(void*), void* arg, void* stackBase,
                    u32 stackSize, OSPriority priority, u16 attr) {
    if (thread == nullptr || func == nullptr) {
        return FALSE;
    }
    std::lock_guard<std::mutex> lock(registryMutex());
    if (findHostUnlocked(thread) != nullptr) {
        return FALSE; // already created
    }
    auto host = std::make_unique<HostThread>();
    host->func = func;
    host->arg = arg;
    threadRegistry()[thread] = std::move(host);

    // Object model (like the SDK): state/suspend/stack bookkeeping. The host
    // thread itself is created on the first OSResumeThread (PC_PORT: attr is
    // stored but has no auto-start semantics — every game caller resumes()).
    thread->state = OS_THREAD_STATE_READY;
    thread->attr = attr;
    thread->suspend = 1;
    thread->priority = priority;
    thread->base = priority;
    thread->stackBase = static_cast<u8*>(stackBase);
    thread->stackEnd = reinterpret_cast<u32*>(static_cast<u8*>(stackBase) + stackSize);
    thread->queue = nullptr;
    thread->queueJoin.head = nullptr;
    thread->queueJoin.tail = nullptr;
    return TRUE;
}

void OSExitThread(void* value) {
    OSThread* t = OSGetCurrentThread();
    if (t == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(registryMutex());
    HostThread* h = findHostUnlocked(t);
    if (h != nullptr) {
        h->exitValue = value;
        h->terminated = true;
    }
    // Cannot destroy the std::thread from inside itself: the trampoline
    // finishes after this returns (end-of-run cleanup handles the join).
}

s32 OSResumeThread(OSThread* thread) {
    if (thread == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(registryMutex());
    HostThread* h = findHostUnlocked(thread);
    if (h == nullptr) {
        return -1;
    }
    if (h->started) {
        // Already running: just clear the suspend request (PC: takes effect
        // at the next blocking point, see the header comment).
        if (thread->suspend > 0) {
            thread->suspend--;
        }
        if (!h->sleeping) {
            return 0;
        }
        h->wakeRequested = true;
        h->cv.notify_all();
        return 0;
    }
    h->started = true;
    thread->suspend = 0;
    thread->state = OS_THREAD_STATE_READY;
    h->th = std::thread(runTrampoline, thread);
    return 0;
}

s32 OSSuspendThread(OSThread* thread) {
    if (thread == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(registryMutex());
    HostThread* h = findHostUnlocked(thread);
    if (h == nullptr) {
        return -1;
    }
    thread->suspend++;
    thread->state = OS_THREAD_STATE_WAITING;
    return 0;
}

BOOL OSIsThreadSuspended(OSThread* thread) {
    return thread != nullptr && thread->suspend > 0;
}

BOOL OSIsThreadTerminated(OSThread* thread) {
    if (thread == nullptr) {
        return TRUE;
    }
    std::lock_guard<std::mutex> lock(registryMutex());
    HostThread* h = findHostUnlocked(thread);
    return h == nullptr || h->terminated;
}

BOOL OSJoinThread(OSThread* thread, void** value) {
    if (thread == nullptr) {
        return FALSE;
    }
    HostThread* h = getHost(thread);
    if (h == nullptr) {
        return FALSE;
    }
    std::unique_lock<std::mutex> lock(h->m);
    h->cv.wait(lock, [h] { return h->terminated; });
    if (value != nullptr) {
        *value = h->exitValue;
    }
    lock.unlock();
    if (h->th.joinable()) {
        h->th.join(); // real join (the OS thread is done)
    }
    {
        std::lock_guard<std::mutex> guard(registryMutex());
        if (threadRegistry().count(thread) != 0) {
            threadRegistry().erase(thread); // done: the record can go (the
                                            // stack slot may be reused by the
                                            // next thread object)
        }
    }
    return TRUE;
}

void OSDetachThread(OSThread* thread) {
    if (thread == nullptr) {
        return;
    }
    HostThread* h = getHost(thread);
    if (h != nullptr && h->th.joinable()) {
        h->th.detach();
    }
}

void OSCancelThread(OSThread* thread) {
    if (thread == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(registryMutex());
    HostThread* h = findHostUnlocked(thread);
    if (h == nullptr) {
        return;
    }
    h->cancelRequested = true;
    h->wakeRequested = true;
    h->cv.notify_all();
    // The thread itself sees the flag at its next OS call / blocking point
    // (checked by the queue wait predicates) and unwinds.
    if (h->terminated) {
        threadRegistry().erase(thread); // already done: release the record
    }
}

OSThread* OSGetCurrentThread(void) {
    return sCurrentTLS;
}

OSPriority OSGetThreadPriority(OSThread* thread) {
    return thread != nullptr ? thread->priority : 0;
}

BOOL OSSetThreadPriority(OSThread* thread, OSPriority priority) {
    if (thread == nullptr) {
        return FALSE;
    }
    thread->priority = priority; // stored only (PC_PORT: not enforced)
    return TRUE;
}

void OSSleepTicks(OSTime ticks) {
    // A blocking sleep, as the OS would do for the current thread.
    const double ns = static_cast<double>(ticks) * (1e9 / 60750000.0); // OS_TIMER_CLOCK
    Platform::Timing::sleepMicroseconds(static_cast<uint64_t>(ns / 1000.0));
}

// --- thread queues (sleep/wakeup) --------------------------------------------

void OSInitThreadQueue(OSThreadQueue* queue) {
    if (queue == nullptr) {
        return;
    }
    queue->head = nullptr;
    queue->tail = nullptr;
}

void OSSleepThread(OSThreadQueue* queue) {
    if (queue == nullptr) {
        return;
    }
    OSThread* t = OSGetCurrentThread();
    if (t == nullptr) {
        return;
    }
    HostThread* h = nullptr;
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        h = findHostUnlocked(t);
        if (h == nullptr) {
            return;
        }
        // Intrusive list, like the SDK (OSThreadLink inside OSThread).
        t->link.next = nullptr;
        t->link.prev = queue->tail;
        if (queue->tail != nullptr) {
            queue->tail->link.next = t;
        } else {
            queue->head = t;
        }
        queue->tail = t;
        t->queue = queue;
        h->sleeping = true;
        h->wakeRequested = h->cancelRequested;
        t->state = OS_THREAD_STATE_WAITING;
    }

    // Wait on the thread's own cv WITHOUT holding the registry mutex
    // (OSWakeupThread/OSCancelThread need it to deliver the notification).
    std::unique_lock<std::mutex> lock2(h->m);
    h->cv.wait(lock2, [h] { return h->wakeRequested || h->cancelRequested; });
    h->sleeping = false;

    // Unlink from the queue.
    std::lock_guard<std::mutex> guard(registryMutex());
    if (t->link.prev != nullptr) {
        t->link.prev->link.next = t->link.next;
    } else {
        queue->head = t->link.next;
    }
    if (t->link.next != nullptr) {
        t->link.next->link.prev = t->link.prev;
    } else {
        queue->tail = t->link.prev;
    }
    t->link.prev = nullptr;
    t->link.next = nullptr;
    t->queue = nullptr;
    t->state = OS_THREAD_STATE_READY;
}

void OSWakeupThread(OSThreadQueue* queue) {
    if (queue == nullptr || queue->head == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(registryMutex());
    OSThread* t = queue->head;
    HostThread* h = findHostUnlocked(t);
    if (h != nullptr) {
        h->wakeRequested = true;
        h->cv.notify_all();
    }
}

// --- message queues -----------------------------------------------------------

void OSInitMessageQueue(OSMessageQueue* queue, OSMessage* msgArray, s32 count) {
    if (queue == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(msgQueueMutex());
    queue->msgArray = msgArray;
    queue->msgCount = count;
    queue->firstIndex = 0;
    queue->usedCount = 0;
    if (msgQueueRegistry().find(queue) == msgQueueRegistry().end()) {
        msgQueueRegistry()[queue] = new HostMsgQueue();
    }
}

BOOL OSSendMessage(OSMessageQueue* queue, OSMessage msg, s32 flags) {
    if (queue == nullptr) {
        return FALSE;
    }
    HostMsgQueue* host = getMsgHost(queue);
    if (host == nullptr) {
        return FALSE;
    }
    std::unique_lock<std::mutex> lock(host->m);
    if ((flags & OS_MESSAGE_BLOCK) != 0) {
        host->cv.wait(lock, [queue] { return queue->usedCount < queue->msgCount; });
    } else if (queue->usedCount >= queue->msgCount) {
        return FALSE; // full + non-blocking
    }
    queue->msgArray[(queue->firstIndex + queue->usedCount) % queue->msgCount] = msg;
    queue->usedCount++;
    host->cv.notify_all();
    return TRUE;
}

BOOL OSJamMessage(OSMessageQueue* queue, OSMessage msg, s32 flags) {
    if (queue == nullptr) {
        return FALSE;
    }
    HostMsgQueue* host = getMsgHost(queue);
    if (host == nullptr) {
        return FALSE;
    }
    std::unique_lock<std::mutex> lock(host->m);
    if ((flags & OS_MESSAGE_BLOCK) != 0) {
        host->cv.wait(lock, [queue] { return queue->usedCount < queue->msgCount; });
    } else if (queue->usedCount >= queue->msgCount) {
        return FALSE;
    }
    // Insert at the front (like the SDK's jam).
    queue->firstIndex = (queue->firstIndex + queue->msgCount - 1) % queue->msgCount;
    queue->msgArray[queue->firstIndex] = msg;
    queue->usedCount++;
    host->cv.notify_all();
    return TRUE;
}

BOOL OSReceiveMessage(OSMessageQueue* queue, OSMessage* msg, s32 flags) {
    if (queue == nullptr || msg == nullptr) {
        return FALSE;
    }
    HostMsgQueue* host = getMsgHost(queue);
    if (host == nullptr) {
        return FALSE;
    }
    std::unique_lock<std::mutex> lock(host->m);
    if ((flags & OS_MESSAGE_BLOCK) != 0) {
        host->cv.wait(lock, [queue] { return queue->usedCount > 0; });
    } else if (queue->usedCount == 0) {
        return FALSE; // empty + non-blocking
    }
    *msg = queue->msgArray[queue->firstIndex];
    queue->firstIndex = (queue->firstIndex + 1) % queue->msgCount;
    queue->usedCount--;
    host->cv.notify_all();
    return TRUE;
}

// --- scheduler -----------------------------------------------------------------

s32 OSDisableScheduler(void) { return 0; } // PC_PORT: no preemptive scheduler
s32 OSEnableScheduler(void) { return 0; }

void OSYieldThread(void) { std::this_thread::yield(); }

} // extern "C"
