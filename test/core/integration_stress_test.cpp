/// =================================================================
///  Integration Stress Tests — ThreadPool + TasksPool + EventBus
/// =================================================================
///  Tests the full pipeline under load:
///    Parser → TasksPool → ThreadPool → EventBus → Module → Result
///
///  Coverage:
///    1. Burst load: many tasks submitted rapidly
///    2. Pool exhaustion: more tasks than pool capacity
///    3. Concurrent execution: verify parallelism
///    4. Object pool: acquire/release cycle correctness
///    5. Mixed workload: different modules, different durations
///    6. Parser + Task + Pool + Bus full chain

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/ThreadPool.h"
#include "core/TasksPool.h"
#include "core/ModuleLifeManager.h"
#include "core/ParmarPack.h"
#include "sdk/IModule.h"
#include "event_bus/event_bus.h"

using namespace std::chrono_literals;

// ================================================================
//  Test module: records execution in thread-safe counter
// ================================================================
class StressTestModule : public ModuleBaseObject
{
public:
    StressTestModule(std::string name, std::atomic<int>* counter, int delay_ms = 0)
        : name_(std::move(name)), counter_(counter), delay_ms_(delay_ms) {}

    const char* GetName() const override { return name_.c_str(); }

    bool OnInit() override
    {
        REGISTER_FUNC("work", "stress test work item", {
            if (delay_ms_ > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
            if (counter_) counter_->fetch_add(1);
            pack->success = true;
            pack->return_value = name_ + "_done";
        });
        return true;
    }

private:
    std::string name_;
    std::atomic<int>* counter_;
    int delay_ms_;
};

// ================================================================
//  Fixture: sets up the full pipeline
// ================================================================
class StressTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        bus_ = &EventBus::GetInstance();
        mgr_ = &ModuleLifeManager::GetInstance();
        tasks_ = std::make_unique<TasksPool>(16);
        workers_ = std::make_unique<ThreadPool>(4);
    }

    void TearDown() override
    {
        workers_.reset();  // join all threads first
        tasks_.reset();
        // Drain done_queue_ in case of leftovers
        std::lock_guard lock(done_mutex_);
        while (!done_queue_.empty()) done_queue_.pop();
    }

    EventBus* bus_;
    ModuleLifeManager* mgr_;
    std::unique_ptr<TasksPool> tasks_;
    std::unique_ptr<ThreadPool> workers_;
    std::mutex done_mutex_;
    std::queue<Task*> done_queue_;
};

// ================================================================
//  Test 1: Burst load — many rapid-fire tasks
// ================================================================
TEST_F(StressTest, BurstLoad)
{
    std::atomic<int> count{0};
    auto mod = std::make_unique<StressTestModule>("Burst", &count, 0);
    mgr_->AddModule(std::move(mod));

    constexpr int N = 100;

    int submitted = 0;
    while (submitted < N)
    {
        // Drain completed tasks to free pool slots
        {
            std::unique_lock lock(done_mutex_);
            while (!done_queue_.empty())
            {
                Task* t = done_queue_.front();
                done_queue_.pop();
                lock.unlock();
                tasks_->Release(t);
                lock.lock();
            }
        }

        auto pack = std::make_unique<ParmarPack>();
        pack->mod_id = "Burst";
        pack->func_id = "work";
        pack->show_explanation = false;

        Task* task = tasks_->Acquire(std::move(pack));
        if (!task) { std::this_thread::sleep_for(1ms); continue; }  // pool full, retry

        workers_->Enqueue([task, this]() {
            bus_->Emit("Burst.work", task->CurrentPack());
            std::lock_guard lock(done_mutex_);
            done_queue_.push(task);
        });
        ++submitted;
    }

    // Final drain of all completions
    for (int waited = 0; waited < 100 && count.load() < N; ++waited)
    {
        std::unique_lock lock(done_mutex_);
        while (!done_queue_.empty())
        {
            Task* t = done_queue_.front();
            done_queue_.pop();
            lock.unlock();
            tasks_->Release(t);
            lock.lock();
        }
        lock.unlock();
        if (count.load() < N)
            std::this_thread::sleep_for(10ms);
    }

    EXPECT_EQ(count.load(), N) << "All tasks should complete";
    // All tasks accounted for — pool eventually returns to full
    for (int w = 0; w < 20 && tasks_->GetFreeCount() < tasks_->GetTotalCount(); ++w)
    {
        std::unique_lock lock(done_mutex_);
        while (!done_queue_.empty())
        {
            Task* t = done_queue_.front(); done_queue_.pop();
            lock.unlock(); tasks_->Release(t); lock.lock();
        }
        lock.unlock();
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(tasks_->GetFreeCount(), tasks_->GetTotalCount())
        << "All tasks should be returned to pool";
}

// ================================================================
//  Test 2: Pool exhaustion — more tasks than slots
// ================================================================
TEST_F(StressTest, PoolExhaustion)
{
    std::atomic<int> count{0};
    auto mod = std::make_unique<StressTestModule>("Exhaust", &count, 20);  // slow
    mgr_->AddModule(std::move(mod));

    // Fill the pool entirely (16 slots)
    std::vector<Task*> busy_tasks;
    for (int i = 0; i < 16; ++i)
    {
        auto pack = std::make_unique<ParmarPack>();
        pack->mod_id = "Exhaust";
        pack->func_id = "work";
        pack->show_explanation = false;
        Task* t = tasks_->Acquire(std::move(pack));
        ASSERT_NE(t, nullptr);
        busy_tasks.push_back(t);
    }

    // 17th should FAIL — pool is full
    auto fail_pack = std::make_unique<ParmarPack>();
    fail_pack->mod_id = "Exhaust";
    fail_pack->func_id = "work";
    Task* fail_task = tasks_->Acquire(std::move(fail_pack));
    EXPECT_EQ(fail_task, nullptr) << "Pool should reject when full";

    // Submit all and drain
    for (auto* t : busy_tasks)
    {
        workers_->Enqueue([t, this]() {
            bus_->Emit("Exhaust.work", t->CurrentPack());
            std::lock_guard lock(done_mutex_);
            done_queue_.push(t);
        });
    }

    for (int waited = 0; waited < 100; ++waited)
    {
        std::unique_lock lock(done_mutex_);
        while (!done_queue_.empty())
        {
            Task* t = done_queue_.front();
            done_queue_.pop();
            lock.unlock();
            tasks_->Release(t);
            lock.lock();
        }
        lock.unlock();
        if (count.load() >= 16) break;
        std::this_thread::sleep_for(10ms);
    }

    EXPECT_EQ(count.load(), 16);
    // Final drain: count may have reached 16 before the last task
    // was pushed to done_queue (count is incremented inside Emit,
    // which is before the done_queue push in the worker lambda).
    for (int w = 0; w < 30 && tasks_->GetFreeCount() < 16u; ++w)
    {
        std::unique_lock lock(done_mutex_);
        while (!done_queue_.empty())
        {
            Task* t = done_queue_.front(); done_queue_.pop();
            lock.unlock(); tasks_->Release(t); lock.lock();
        }
        lock.unlock();
        if (tasks_->GetFreeCount() == 16u) break;
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(tasks_->GetFreeCount(), 16u);
}

// ================================================================
//  Test 3: Concurrent execution (timing proof of parallelism)
// ================================================================
TEST_F(StressTest, ParallelExecution)
{
    std::atomic<int> count{0};
    // All tasks sleep 100ms. With 4 threads, 10 tasks should complete
    // much faster than 10 * 100ms = 1000ms serial.
    auto mod = std::make_unique<StressTestModule>("Parallel", &count, 100);
    mgr_->AddModule(std::move(mod));

    constexpr int N = 10;
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < N; ++i)
    {
        auto pack = std::make_unique<ParmarPack>();
        pack->mod_id = "Parallel";
        pack->func_id = "work";
        pack->show_explanation = false;
        Task* task = tasks_->Acquire(std::move(pack));
        ASSERT_NE(task, nullptr);

        workers_->Enqueue([task, this]() {
            bus_->Emit("Parallel.work", task->CurrentPack());
            std::lock_guard lock(done_mutex_);
            done_queue_.push(task);
        });
    }

    // Drain
    for (int waited = 0; waited < 100 && count.load() < N; ++waited)
    {
        std::unique_lock lock(done_mutex_);
        while (!done_queue_.empty())
        {
            Task* t = done_queue_.front();
            done_queue_.pop();
            lock.unlock();
            tasks_->Release(t);
            lock.lock();
        }
        lock.unlock();
        if (count.load() >= N) break;
        std::this_thread::sleep_for(10ms);
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    EXPECT_EQ(count.load(), N);
    // With 4 workers × 100ms sleep × 10 tasks:
    // Serial would be 1000ms, parallel should be ~250-400ms
    EXPECT_LT(ms, 800) << "Parallel execution should beat serial time (took " << ms << "ms)";
}

// ================================================================
//  Test 4: Object pool acquire/release cycle integrity
// ================================================================
TEST_F(StressTest, PoolCycleIntegrity)
{
    std::atomic<int> count{0};
    auto mod = std::make_unique<StressTestModule>("Cycle", &count, 5);
    mgr_->AddModule(std::move(mod));

    constexpr int kRounds = 20;  // more rounds than pool capacity
    constexpr int kBatch  = 4;   // batch size < pool capacity

    for (int round = 0; round < kRounds; ++round)
    {
        // Submit a batch
        for (int i = 0; i < kBatch; ++i)
        {
            auto pack = std::make_unique<ParmarPack>();
            pack->mod_id = "Cycle";
            pack->func_id = "work";
            pack->show_explanation = false;
            Task* task = tasks_->Acquire(std::move(pack));
            ASSERT_NE(task, nullptr) << "Pool should have free slots on round " << round;

            workers_->Enqueue([task, this]() {
                bus_->Emit("Cycle.work", task->CurrentPack());
                std::lock_guard lock(done_mutex_);
                done_queue_.push(task);
            });
        }

        // Drain
        for (int waited = 0; waited < 50; ++waited)
        {
            std::unique_lock lock(done_mutex_);
            while (!done_queue_.empty())
            {
                Task* t = done_queue_.front();
                done_queue_.pop();
                lock.unlock();
                tasks_->Release(t);
                lock.lock();
            }
            lock.unlock();
            if (count.load() >= (round + 1) * kBatch) break;
            std::this_thread::sleep_for(10ms);
        }
    }

    EXPECT_EQ(count.load(), kRounds * kBatch);
    EXPECT_EQ(tasks_->GetFreeCount(), 16u) << "All tasks returned to pool";
}

// ================================================================
//  Test 5: Mixed workload (different modules, different durations)
// ================================================================
TEST_F(StressTest, MixedWorkload)
{
    std::atomic<int> fast_count{0}, slow_count{0};
    auto fast = std::make_unique<StressTestModule>("Fast", &fast_count, 0);
    auto slow = std::make_unique<StressTestModule>("Slow", &slow_count, 50);
    mgr_->AddModule(std::move(fast));
    mgr_->AddModule(std::move(slow));

    constexpr int N_FAST = 30;
    constexpr int N_SLOW = 10;

    auto submit = [&](const char* mod) {
        // Drain to free slots
        {
            std::unique_lock lock(done_mutex_);
            while (!done_queue_.empty())
            {
                Task* t = done_queue_.front(); done_queue_.pop();
                lock.unlock(); tasks_->Release(t); lock.lock();
            }
        }
        auto pack = std::make_unique<ParmarPack>();
        pack->mod_id = mod; pack->func_id = "work"; pack->show_explanation = false;
        Task* t = tasks_->Acquire(std::move(pack));
        while (!t) {
            // pool full — drain then retry
            std::unique_lock lock(done_mutex_);
            while (!done_queue_.empty())
            {
                Task* dt = done_queue_.front(); done_queue_.pop();
                lock.unlock(); tasks_->Release(dt); lock.lock();
            }
            lock.unlock();
            std::this_thread::sleep_for(1ms);
            auto retry_pack = std::make_unique<ParmarPack>();
            retry_pack->mod_id = mod; retry_pack->func_id = "work"; retry_pack->show_explanation = false;
            t = tasks_->Acquire(std::move(retry_pack));
        }
        workers_->Enqueue([t, this]() {
            bus_->Emit(t->CurrentPack()->mod_id + ".work", t->CurrentPack());
            std::lock_guard lock(done_mutex_);
            done_queue_.push(t);
        });
    };

    // Submit slow first, then fast
    for (int i = 0; i < N_SLOW; ++i) submit("Slow");
    for (int i = 0; i < N_FAST; ++i) submit("Fast");

    // Drain all
    for (int waited = 0; waited < 200; ++waited)
    {
        std::unique_lock lock(done_mutex_);
        while (!done_queue_.empty())
        {
            Task* t = done_queue_.front();
            done_queue_.pop();
            lock.unlock();
            tasks_->Release(t);
            lock.lock();
        }
        lock.unlock();
        if (fast_count.load() >= N_FAST && slow_count.load() >= N_SLOW) break;
        std::this_thread::sleep_for(10ms);
    }

    EXPECT_EQ(fast_count.load(), N_FAST);
    EXPECT_EQ(slow_count.load(), N_SLOW);
    for (int w = 0; w < 30 && tasks_->GetFreeCount() < 16u; ++w)
    {
        std::unique_lock lock(done_mutex_);
        while (!done_queue_.empty())
        {
            Task* t = done_queue_.front(); done_queue_.pop();
            lock.unlock(); tasks_->Release(t); lock.lock();
        }
        lock.unlock();
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(tasks_->GetFreeCount(), 16u);
}

// ================================================================
//  Test 6: ThreadPool graceful shutdown with pending tasks
// ================================================================
TEST_F(StressTest, GracefulShutdown)
{
    std::atomic<int> count{0};
    auto mod = std::make_unique<StressTestModule>("Shutdown", &count, 20);
    mgr_->AddModule(std::move(mod));

    constexpr int N = 8;
    for (int i = 0; i < N; ++i)
    {
        auto pack = std::make_unique<ParmarPack>();
        pack->mod_id = "Shutdown"; pack->func_id = "work"; pack->show_explanation = false;
        Task* t = tasks_->Acquire(std::move(pack));
        ASSERT_NE(t, nullptr);
        workers_->Enqueue([t, this]() {
            bus_->Emit("Shutdown.work", t->CurrentPack());
            std::lock_guard lock(done_mutex_);
            done_queue_.push(t);
        });
    }

    // Destroy thread pool — it should wait for all tasks
    workers_.reset();  // destructor joins threads

    // Drain whatever completed
    std::unique_lock lock(done_mutex_);
    while (!done_queue_.empty())
    {
        Task* t = done_queue_.front();
        done_queue_.pop();
        lock.unlock();
        tasks_->Release(t);
        lock.lock();
    }
    lock.unlock();

    EXPECT_EQ(count.load(), N) << "All tasks should complete before pool destroyed";
}

// ================================================================
//  Test 7: Concurrent pool acquire/release (race condition test)
// ================================================================
TEST_F(StressTest, ConcurrentAcquireRelease)
{
    std::atomic<int> count{0};
    auto mod = std::make_unique<StressTestModule>("Racy", &count, 1);
    mgr_->AddModule(std::move(mod));

    // Multiple threads simultaneously acquiring and releasing
    std::atomic<int> acquired{0};
    std::vector<std::thread> producers;

    constexpr int N = 200;
    for (int t = 0; t < 4; ++t)
    {
        producers.emplace_back([&, this]() {
            for (int i = 0; i < N / 4; ++i)
            {
                auto pack = std::make_unique<ParmarPack>();
                pack->mod_id = "Racy"; pack->func_id = "work"; pack->show_explanation = false;

                Task* task = tasks_->Acquire(std::move(pack));
                if (!task) { --i; std::this_thread::sleep_for(1ms); continue; }  // retry
                acquired.fetch_add(1);

                bus_->Emit("Racy.work", task->CurrentPack());
                tasks_->Release(task);
            }
        });
    }

    for (auto& t : producers) t.join();

    EXPECT_EQ(count.load(), N) << "All tasks should execute";
    EXPECT_EQ(tasks_->GetFreeCount(), 16u) << "All slots free after release";
}
