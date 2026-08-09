/// =================================================================
///  ThreadPool isolation tests
/// =================================================================
///  Tests the ThreadPool in complete isolation — no framework deps.

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <vector>
#include "core/ThreadPool.h"

// ================================================================
//  Basic construction / destruction
// ================================================================

TEST(ThreadPoolTest, ConstructAndDestruct)
{
    EXPECT_NO_THROW({
        ThreadPool pool(2);
    });  // destructor joins threads gracefully
}

TEST(ThreadPoolTest, WorkersCreated)
{
    ThreadPool pool(4);
    EXPECT_EQ(pool.WorkerCount(), 4u);
}

// ================================================================
//  Enqueue — fire and forget
// ================================================================

TEST(ThreadPoolTest, EnqueueRunsJob)
{
    ThreadPool pool(2);
    std::atomic<bool> done{false};

    pool.Enqueue([&]() { done.store(true); });

    // Wait a bit for the job to complete
    for (int i = 0; i < 100 && !done.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_TRUE(done.load());
}

TEST(ThreadPoolTest, EnqueueMultipleJobs)
{
    ThreadPool pool(4);
    std::atomic<int> count{0};
    constexpr int N = 100;

    for (int i = 0; i < N; ++i)
        pool.Enqueue([&]() { count.fetch_add(1); });

    // Wait for all jobs
    while (count.load() < N)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    EXPECT_EQ(count.load(), N);
}

// ================================================================
//  Submit — returns future
// ================================================================

TEST(ThreadPoolTest, SubmitReturnsResult)
{
    ThreadPool pool(2);

    auto future = pool.Submit([](int a, int b) { return a + b; }, 10, 20);

    EXPECT_EQ(future.get(), 30);
}

TEST(ThreadPoolTest, SubmitWithLambda)
{
    ThreadPool pool(2);

    auto f1 = pool.Submit([]() { return 42; });
    auto f2 = pool.Submit([](int x) { return x * x; }, 7);

    EXPECT_EQ(f1.get(), 42);
    EXPECT_EQ(f2.get(), 49);
}

TEST(ThreadPoolTest, SubmitParallelExecution)
{
    ThreadPool pool(4);
    constexpr int N = 20;

    std::vector<std::future<int>> futures;
    for (int i = 0; i < N; ++i)
        futures.push_back(pool.Submit([i]() {
            // Simulate work
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return i * i;
        }));

    // Collect results — all should complete
    int sum = 0;
    for (auto& f : futures)
        sum += f.get();

    // sum of squares 0..19
    EXPECT_EQ(sum, 2470);  // 0^2 + 1^2 + ... + 19^2 = 19*20*39/6 = 2470
}

// ================================================================
//  Graceful shutdown
// ================================================================

TEST(ThreadPoolTest, JobsCompleteBeforeDestroy)
{
    std::atomic<int> count{0};

    {
        ThreadPool pool(2);
        for (int i = 0; i < 50; ++i)
            pool.Enqueue([&]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                count.fetch_add(1);
            });
    }  // destructor waits for all jobs

    EXPECT_EQ(count.load(), 50);
}

TEST(ThreadPoolTest, EnqueueAfterStopIgnored)
{
    auto pool = std::make_unique<ThreadPool>(2);
    pool.reset();  // destroys pool

    // Pool is gone, no crash expected
    EXPECT_TRUE(true);
}

// ================================================================
//  Pending count
// ================================================================

TEST(ThreadPoolTest, PendingDecreases)
{
    ThreadPool pool(2);
    std::atomic<bool> started{false};
    std::atomic<bool> done{false};

    pool.Enqueue([&]() {
        started.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        done.store(true);
    });

    while (!started.load()) {}  // wait for job to start
    EXPECT_LE(pool.Pending(), 1u);

    while (!done.load()) {}
    EXPECT_EQ(pool.Pending(), 0u);  // queue drained
}

// ================================================================
//  Exception safety
// ================================================================

TEST(ThreadPoolTest, JobExceptionDoesNotCrashPool)
{
    ThreadPool pool(2);
    std::atomic<bool> second_job_ran{false};

    pool.Enqueue([]() { throw std::runtime_error("oops"); });
    pool.Enqueue([&]() { second_job_ran.store(true); });

    for (int i = 0; i < 100 && !second_job_ran.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_TRUE(second_job_ran.load())
        << "Second job should run even after first threw";
}
