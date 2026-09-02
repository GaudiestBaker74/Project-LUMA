// =============================================================================
// M9.1: compat/os thread layer — OSThread/OSMessageQueue/OSThreadQueue host
// emulation + JKRThread (the vendored wrapper) working end-to-end.
//
// Covers: create-suspended → resume → run → terminate, blocking message
// round-trip, jam + non-blocking semantics, sleep/wakeup thread queues,
// join with exit value, and a JKRThread subclass sending a message (the exact
// shape JASDvdThread/JASAudioThread/FunctionAsyncExecutor use).
// =============================================================================

#include "tests/test_runner.h"

#include "compat/os/OSCompat.h"

#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JKernel/JKRThread.hpp>

#include <revolution/os.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

namespace {

struct RawCtx {
    OSMessageQueue* q;
    std::atomic<int>* received;
};

void* rawReceiver(void* p) {
    RawCtx* ctx = static_cast<RawCtx*>(p);
    OSMessage m = nullptr;
    if (!OSReceiveMessage(ctx->q, &m, OS_MESSAGE_BLOCK)) {
        return reinterpret_cast<void*>(0xDEAD);
    }
    ctx->received->fetch_add(static_cast<int>(reinterpret_cast<intptr_t>(m)));
    return reinterpret_cast<void*>(0x77);
}

// A JKRThread subclass that sends one message and returns a value — the
// pattern of the game's driver threads.
class MsgThread : public JKRThread {
public:
    explicit MsgThread(OSMessageQueue* outQ)
        : JKRThread(0x4000, 4, 8 /* priority, stored only */), mOutQ(outQ) {}

    void* run() override {
        OSSendMessage(mOutQ, reinterpret_cast<void*>(0x1234), OS_MESSAGE_BLOCK);
        return reinterpret_cast<void*>(0x99);
    }

private:
    OSMessageQueue* mOutQ;
};

void ensureRootHeapForThreads() {
    if (JKRHeap::sRootHeap == nullptr) {
        JKRExpHeap::createRoot(1, true);
    }
    REQUIRE(JKRHeap::sRootHeap != nullptr);
}

} // namespace

TEST_CASE(os_thread_create_resume_join) {
    void* stack = ::operator new(0x8000);
    OSThread t;
    RawCtx ctx;
    OSMessage msgs[4];
    std::atomic<int> received{0};

    OSMessageQueue q;
    OSInitMessageQueue(&q, msgs, 4);
    ctx.q = &q;
    ctx.received = &received;

    // Created suspended, starts only on resume (PC_PORT: attr not used).
    CHECK(OSCreateThread(&t, rawReceiver, &ctx, stack, 0x8000, 0x20, 0));
    CHECK(!OSIsThreadTerminated(&t));
    CHECK(OSIsThreadSuspended(&t)); // suspend == 1 after create

    OSResumeThread(&t);

    // Blocking send → worker receives it.
    CHECK(OSSendMessage(&q, reinterpret_cast<void*>(42), OS_MESSAGE_BLOCK));
    void* ret = nullptr;
    CHECK(OSJoinThread(&t, &ret));
    CHECK(ret == reinterpret_cast<void*>(0x77));
    CHECK(received.load() == 42);
    CHECK(OSIsThreadTerminated(&t));
    ::operator delete(stack);
}

TEST_CASE(os_message_queue_poll_and_jam) {
    OSMessage msgs[2];
    OSMessageQueue q;
    OSInitMessageQueue(&q, msgs, 2);

    // Non-blocking receive on an empty queue fails.
    OSMessage m = nullptr;
    CHECK(!OSReceiveMessage(&q, &m, OS_MESSAGE_NOBLOCK));

    // Jam puts at the front; the second message goes behind it.
    CHECK(OSJamMessage(&q, reinterpret_cast<void*>(1), OS_MESSAGE_NOBLOCK));
    CHECK(OSJamMessage(&q, reinterpret_cast<void*>(2), OS_MESSAGE_NOBLOCK));

    CHECK(OSReceiveMessage(&q, &m, OS_MESSAGE_NOBLOCK));
    CHECK(m == reinterpret_cast<void*>(2));
    CHECK(OSReceiveMessage(&q, &m, OS_MESSAGE_NOBLOCK));
    CHECK(m == reinterpret_cast<void*>(1));

    // Full + non-blocking fails.
    CHECK(OSSendMessage(&q, reinterpret_cast<void*>(9), OS_MESSAGE_NOBLOCK));
    CHECK(OSSendMessage(&q, reinterpret_cast<void*>(8), OS_MESSAGE_NOBLOCK));
    CHECK(!OSSendMessage(&q, reinterpret_cast<void*>(7), OS_MESSAGE_NOBLOCK));
}

TEST_CASE(os_thread_sleep_wakeup) {
    void* stack = ::operator new(0x8000);
    OSThreadQueue q;
    OSInitThreadQueue(&q);

    struct SleepCtx {
        OSThreadQueue* queue;
        std::atomic<bool>* woken;
    };
    static SleepCtx sCtx;
    std::atomic<bool> woken{false};

    auto sleeperFn = [](void* p) -> void* {
        SleepCtx* c = static_cast<SleepCtx*>(p);
        OSSleepThread(c->queue); // blocks until OSWakeupThread
        c->woken->store(true);
        return nullptr;
    };

    sCtx.queue = &q;
    sCtx.woken = &woken;

    OSThread t;
    CHECK(OSCreateThread(&t, sleeperFn, &sCtx, stack, 0x8000, 0x20, 0));
    OSResumeThread(&t);

    // Wait (bounded, real sleep — not a spin) until the worker is parked in
    // the queue; REQUIRE so a timeout reports instead of hanging the suite.
    for (int i = 0; i < 2000 && q.head == nullptr; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(q.head == &t); // the worker is parked in the queue
    OSWakeupThread(&q);
    CHECK(OSJoinThread(&t, nullptr) == TRUE);
    CHECK(woken.load());
    ::operator delete(stack);
}

TEST_CASE(jkr_thread_sends_message) {
    ensureRootHeapForThreads();

    OSMessage msgs[4];
    OSMessageQueue q;
    OSInitMessageQueue(&q, msgs, 4);

    MsgThread* thread = new MsgThread(&q);
    thread->resume();

    OSMessage m = nullptr;
    CHECK(OSReceiveMessage(&q, &m, OS_MESSAGE_BLOCK));
    CHECK(m == reinterpret_cast<void*>(0x1234));
    CHECK(OSJoinThread(thread->mThread, nullptr) == TRUE);

    delete thread;
}
