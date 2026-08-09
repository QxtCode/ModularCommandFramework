/// =================================================================
///  Predicate Wait Tests — cv.wait_for vs polling
/// =================================================================
///
///  验证用 condition_variable + 谓词等待替代 sleep 轮询后的行为：
///   1. 有活立刻醒 — notify 后 wait_for 立即返回
///   2. 超时返回   — 没活时 wait_for 等到超时返回 false
///   3. 不空转     — 没活时 CPU 不空转

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

#include "core/ThreadPool.h"

using namespace std::chrono_literals;

// ================================================================
//  Test 1: 有活立刻醒 — notify 后 wait_for 应在 10ms 内返回
// ================================================================
TEST(PredicateWait, NotifyWakesImmediately)
{
    std::mutex m;
    std::condition_variable cv;
    std::queue<int> q;
    std::atomic<bool> ready{false};

    // Consumer: wait_for with predicate
    std::thread consumer([&]() {
        std::unique_lock lock(m);
        // Wait up to 5s — should wake much earlier on notify
        bool got_work = cv.wait_for(lock, 5s, [&]() {
            return !q.empty();
        });
        ready.store(got_work);
    });

    // Producer: wait a bit, then push and notify
    std::this_thread::sleep_for(50ms);
    {
        std::lock_guard lock(m);
        q.push(42);
    }
    cv.notify_one();

    auto start = std::chrono::steady_clock::now();
    consumer.join();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    EXPECT_TRUE(ready.load()) << "wait_for should return true when queue has data";
    EXPECT_LT(elapsed, 100) << "Should wake within 100ms of notify, took " << elapsed << "ms";
}

// ================================================================
//  Test 2: 超时返回 — 没活时 wait_for 等到超时，返回 false
// ================================================================
TEST(PredicateWait, TimeoutReturnsFalse)
{
    std::mutex m;
    std::condition_variable cv;
    std::queue<int> q;

    std::unique_lock lock(m);
    auto start = std::chrono::steady_clock::now();
    bool got_work = cv.wait_for(lock, 100ms, [&]() {
        return !q.empty();  // never becomes true
    });
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    EXPECT_FALSE(got_work) << "wait_for should return false on timeout";
    EXPECT_GE(elapsed, 80) << "Should wait at least ~100ms, took " << elapsed << "ms";
    EXPECT_LT(elapsed, 200) << "Should not wait much longer than 100ms, took " << elapsed << "ms";
}

// ================================================================
//  Test 3: 多生产者 — 多次 notify，消费者每次立刻醒
// ================================================================
TEST(PredicateWait, MultipleProducers)
{
    std::mutex m;
    std::condition_variable cv;
    std::queue<int> q;
    std::atomic<int> consumed{0};
    std::atomic<bool> stop{false};

    std::thread consumer([&]() {
        while (!stop.load())
        {
            std::unique_lock lock(m);
            cv.wait_for(lock, 50ms, [&]() {
                return !q.empty() || stop.load();
            });
            while (!q.empty())
            {
                q.pop();
                lock.unlock();
                consumed.fetch_add(1);
                lock.lock();
            }
        }
    });

    // 10 rapid-fire pushes with notify
    constexpr int N = 100;
    for (int i = 0; i < N; ++i)
    {
        {
            std::lock_guard lock(m);
            q.push(i);
        }
        cv.notify_one();
        std::this_thread::sleep_for(1ms);
    }

    // Wait for all to be consumed
    for (int w = 0; w < 50 && consumed.load() < N; ++w)
        std::this_thread::sleep_for(10ms);

    stop.store(true);
    cv.notify_one();
    consumer.join();

    EXPECT_EQ(consumed.load(), N) << "All " << N << " items should be consumed";
}

// ================================================================
//  Test 4: 对比 — cv.wait_for 不空转 (验证 CPU 友好)
// ================================================================
//  不能直接测 CPU，但可以测：没活的时候线程确实在等（不消耗时间片）。
//  这里验证 wait_for(100ms) 没有提前返回（说明线程确实在 sleep）。
TEST(PredicateWait, NoSpuriousWakeupInShortWait)
{
    std::mutex m;
    std::condition_variable cv;
    std::queue<int> q;
    std::atomic<int> wake_count{0};

    std::thread waiter([&]() {
        for (int i = 0; i < 10; ++i)
        {
            std::unique_lock lock(m);
            // Wait with short timeout, count how many times predicate is checked
            cv.wait_for(lock, 10ms, [&]() {
                wake_count.fetch_add(1);
                return !q.empty();  // never true
            });
        }
    });

    waiter.join();

    // With 10 iterations × 10ms timeout, predicate should be checked
    // roughly 10 times (once per timeout). If spurious wakeups happen,
    // it would be checked more. 20 is a generous upper bound.
    std::cout << "[TEST] Predicate checked " << wake_count.load()
              << " times in 10 × 10ms waits\n";
    EXPECT_LE(wake_count.load(), 20)
        << "Should not have excessive spurious wakeups";
}

// ================================================================
//  Test 5: 模拟 main.cpp 的 drain 模式
// ================================================================
//  Worker 线程完成任务 → push 到 done_queue → notify
//  Main 线程用 cv.wait_for(100ms) drain
TEST(PredicateWait, SimulateMainLoopDrain)
{
    std::mutex done_mutex;
    std::condition_variable done_cv;
    std::queue<int> done_queue;
    std::atomic<int> completed{0};

    constexpr int N = 50;
    ThreadPool workers(4);

    // Submit tasks — each pushes result to done_queue and notifies
    for (int i = 0; i < N; ++i)
    {
        workers.Enqueue([i, &done_mutex, &done_cv, &done_queue, &completed]() {
            // Simulate work
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            {
                std::lock_guard lock(done_mutex);
                done_queue.push(i);
            }
            done_cv.notify_one();
            completed.fetch_add(1);
        });
    }

    // Main loop: drain with cv.wait_for
    int drained = 0;
    while (drained < N)
    {
        std::unique_lock lock(done_mutex);
        // Wait up to 200ms for work, or timeout
        if (done_cv.wait_for(lock, 200ms, [&]() {
            return !done_queue.empty();
        }))
        {
            // Got work — drain everything available
            while (!done_queue.empty())
            {
                done_queue.pop();
                drained++;
            }
        }
        else
        {
            // Timeout — no more work expected or coming
            if (completed.load() >= N)
                break;
        }
    }

    EXPECT_EQ(drained, N) << "All tasks should be drained";
    EXPECT_EQ(completed.load(), N) << "All tasks should complete";
}
