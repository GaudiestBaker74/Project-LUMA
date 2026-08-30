#include "tests/test_runner.h"

#include "platform/Threading/Threading.h"
#include "platform/Timing/Timing.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

TEST_CASE(threading_thread_wrapper) {
    std::atomic<int> counter{0};
    {
        Platform::Threading::Thread thread("worker", [&counter] { counter.fetch_add(1); });
        CHECK(thread.joinable());
        thread.join();
    }
    CHECK_EQ(counter.load(), 1);
}

TEST_CASE(threading_message_queue_fifo) {
    Platform::Threading::MessageQueue<int> queue(4);
    CHECK(queue.capacity() == 4);
    CHECK(queue.empty());

    CHECK(queue.send(1));
    CHECK(queue.send(2));
    CHECK(queue.send(3));
    CHECK_EQ(queue.size(), static_cast<size_t>(3));

    int out = 0;
    CHECK(queue.receive(out));
    CHECK_EQ(out, 1);
    CHECK(queue.receive(out));
    CHECK_EQ(out, 2);
    CHECK(queue.receive(out));
    CHECK_EQ(out, 3);
    CHECK(queue.empty());
}

TEST_CASE(threading_message_queue_jam) {
    Platform::Threading::MessageQueue<int> queue(4);
    queue.send(1);
    queue.send(2);
    queue.jam(0); // goes to the front

    int out = 0;
    queue.receive(out);
    CHECK_EQ(out, 0);
    queue.receive(out);
    CHECK_EQ(out, 1);
    queue.receive(out);
    CHECK_EQ(out, 2);
}

TEST_CASE(threading_message_queue_timeout) {
    Platform::Threading::MessageQueue<int> queue(1);
    int out = 0;

    const auto start = Platform::Timing::now();
    CHECK(!queue.receive(out, 20'000'000ull)); // 20 ms on an empty queue
    CHECK(Platform::Timing::secondsSince(start) >= 0.015);

    CHECK(queue.send(42));
    CHECK(queue.receive(out, 20'000'000ull));
    CHECK_EQ(out, 42);
    CHECK(!queue.receive(out, 1'000'000ull));

    // send() timeout on a full queue.
    CHECK(queue.send(1));
    CHECK(!queue.send(2, 20'000'000ull));
    int dummy = 0;
    queue.receive(dummy);
}

TEST_CASE(threading_message_queue_blocking) {
    Platform::Threading::MessageQueue<int> queue(2);
    std::atomic<bool> producerDone{false};

    Platform::Threading::Thread producer("producer", [&] {
        // The queue fills after 2 messages; the 3rd send blocks until the
        // consumer drains it.
        queue.send(10);
        queue.send(20);
        queue.send(30); // blocks
        producerDone.store(true);
    });

    Platform::Timing::sleepSeconds(0.02);
    CHECK(!producerDone.load()); // still blocked on the 3rd send

    int out = 0;
    CHECK(queue.receive(out));
    CHECK_EQ(out, 10);
    Platform::Timing::sleepSeconds(0.01);
    CHECK(producerDone.load());

    queue.receive(out);
    CHECK_EQ(out, 20);
    queue.receive(out);
    CHECK_EQ(out, 30);
    producer.join();
}

TEST_CASE(threading_message_queue_stress) {
    constexpr int kProducers = 4;
    constexpr int kMessagesPerProducer = 200;

    Platform::Threading::MessageQueue<int> queue(16);
    std::atomic<int> received{0};

    std::vector<std::unique_ptr<Platform::Threading::Thread>> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.push_back(std::make_unique<Platform::Threading::Thread>("producer", [&, p] {
            for (int i = 0; i < kMessagesPerProducer; ++i) {
                queue.send(p * 1000 + i);
            }
        }));
    }

    Platform::Threading::Thread consumer("consumer", [&] {
        int out = 0;
        int count = 0;
        while (count < kProducers * kMessagesPerProducer) {
            if (queue.receive(out, 200'000'000ull)) {
                ++count;
            } else {
                break;
            }
        }
        received.store(count);
    });

    for (auto& producer : producers) {
        producer->join();
    }
    consumer.join();

    CHECK_EQ(received.load(), kProducers * kMessagesPerProducer);
}

TEST_CASE(threading_recursive_mutex) {
    // Our Mutex alias is recursive — matches RVL OSMutex semantics.
    Platform::Threading::Mutex mutex;
    std::unique_lock<Platform::Threading::Mutex> lock1(mutex);
    std::unique_lock<Platform::Threading::Mutex> lock2(mutex); // would deadlock on a non-recursive mutex
    (void)lock1;
    (void)lock2;
    CHECK(true);
}

TEST_CASE(threading_thread_id) {
    const uint64_t mainId = Platform::Threading::currentThreadId();
    uint64_t workerId = 0;
    {
        Platform::Threading::Thread thread("idprobe", [&] { workerId = Platform::Threading::currentThreadId(); });
        thread.join();
    }
    CHECK(workerId != 0);
    CHECK(workerId != mainId);
}
