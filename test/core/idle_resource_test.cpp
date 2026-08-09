/// =================================================================
///  Idle Resource Test — measure framework memory when idle
/// =================================================================
///
///  Checks:
///   1. Thread stack memory (each thread = ~1MB default on Windows)
///   2. Pre-allocated TasksPool memory
///   3. EventBus + ModuleLifeManager baseline
///   4. After stress → back to idle: memory returns to baseline

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

#include "core/ThreadPool.h"
#include "core/TasksPool.h"
#include "core/ModuleLifeManager.h"
#include "core/ParmarPack.h"
#include "sdk/IModule.h"
#include "event_bus/event_bus.h"

using namespace std::chrono_literals;

// ================================================================
//  Get process working set (physical RAM used)
// ================================================================
static size_t GetWorkingSetKB()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX mc{};
    mc.cb = sizeof(mc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mc), sizeof(mc)))
    {
        return mc.WorkingSetSize / 1024;
    }
#endif
    return 0;
}

// ================================================================
//  Measure thread stack overhead
// ================================================================
TEST(IdleResource, ThreadStackOverhead)
{
    size_t before = GetWorkingSetKB();

    // Create thread pool, measure, destroy
    {
        ThreadPool pool(4);
        std::this_thread::sleep_for(100ms);  // let threads start
        size_t with_pool = GetWorkingSetKB();
        std::cout << "  [4 workers] baseline: " << before << " KB → with pool: "
                  << with_pool << " KB (delta: " << (with_pool - before) << " KB)"
                  << std::endl;

        // Check: 4 threads × 1MB stack = ~4MB expected overhead
        long long delta = static_cast<long long>(with_pool) - static_cast<long long>(before);
        EXPECT_LT(delta, 8192)  // should be under 8MB for 4 threads
            << "Thread pool memory overhead too high: " << delta << " KB";
    }

    std::this_thread::sleep_for(100ms);
    size_t after = GetWorkingSetKB();
    std::cout << "  [destroyed] " << after << " KB (delta: "
              << (static_cast<long long>(after) - static_cast<long long>(before))
              << " KB)" << std::endl;

    // After pool destruction, memory should return close to baseline
    long long leak = static_cast<long long>(after) - static_cast<long long>(before);
    EXPECT_LT(leak, 2048)
        << "Memory not released after pool destruction: " << leak << " KB";
}

// ================================================================
//  TasksPool pre-allocation overhead
// ================================================================
TEST(IdleResource, TasksPoolOverhead)
{
    size_t before = GetWorkingSetKB();

    // Pool of 32 Task objects — each Task has vector<unique_ptr<ParmarPack>>
    // + atomic + callback. Should be small (~1-2KB total).
    {
        TasksPool pool(32);
        size_t with_pool = GetWorkingSetKB();
        std::cout << "  [32 tasks] baseline: " << before << " KB → with pool: "
                  << with_pool << " KB (delta: " << (with_pool - before) << " KB)"
                  << std::endl;

        long long delta = static_cast<long long>(with_pool) - static_cast<long long>(before);
        EXPECT_LT(delta, 256)  // 32 task objects should be well under 256KB
            << "TasksPool overhead too high: " << delta << " KB";
    }
}

// ================================================================
//  Full framework idle baseline
// ================================================================
TEST(IdleResource, FullFrameworkIdle)
{
    size_t before = GetWorkingSetKB();

    auto& mgr = ModuleLifeManager::GetInstance();
    auto& bus = EventBus::GetInstance();

    {
        TasksPool  tasks(16);
        ThreadPool workers(4);

        // Register a minimal module (no user data)
        class IdleMod : public ModuleBaseObject {
            const char* GetName() const override { return "Idle"; }
            bool OnInit() override {
                REGISTER_FUNC("nop", "", { pack->success = true; });
                return true;
            }
        };
        mgr.AddModule(std::make_unique<IdleMod>());

        std::this_thread::sleep_for(200ms);
        size_t idle = GetWorkingSetKB();
        std::cout << "  [framework idle] baseline: " << before << " KB → idle: "
                  << idle << " KB (delta: " << (idle - before) << " KB)"
                  << std::endl;

        // Full framework + 4 threads + 1 module + 16 tasks
        // Expected: ~4-8 MB (thread stacks are the main cost)
        long long delta = static_cast<long long>(idle) - static_cast<long long>(before);
        EXPECT_LT(delta, 10240)  // under 10 MB
            << "Framework idle overhead too high: " << delta << " KB";

        mgr.UnloadModule("Idle");
    }

    std::this_thread::sleep_for(200ms);
    size_t after = GetWorkingSetKB();
    std::cout << "  [all destroyed] " << after << " KB"
              << std::endl;
}

// ================================================================
//  Stress → idle: memory returns to baseline
// ================================================================
class IdleMod2 : public ModuleBaseObject {
public:
    IdleMod2(std::string n, std::atomic<int>* c)
        : name_(std::move(n)), count_(c) {}
    const char* GetName() const override { return name_.c_str(); }
    bool OnInit() override {
        REGISTER_FUNC("w", "", {
            if (count_) count_->fetch_add(1);
            pack->success = true;
        });
        return true;
    }
private:
    std::string name_;
    std::atomic<int>* count_;
};

TEST(IdleResource, StressThenIdleMemoryReturn)
{
    auto& mgr = ModuleLifeManager::GetInstance();
    auto& bus = EventBus::GetInstance();

    size_t before = GetWorkingSetKB();
    std::atomic<int> count{0};

    // ---- Phase 1: Load 5 modules, run stress for 10 seconds ----
    {
        for (int i = 0; i < 5; ++i)
            mgr.AddModule(std::make_unique<IdleMod2>(
                "IM_" + std::to_string(i), &count));

        ThreadPool workers(4);
        TasksPool tasks(16);
        std::mutex done_mutex;
        std::queue<Task*> done_queue;
        std::condition_variable done_cv;
        std::atomic<bool> stop{false};

        std::thread submitter([&]() {
            int seq = 0;
            while (!stop.load()) {
                std::unique_lock lk(done_mutex);
                while (!done_queue.empty()) {
                    Task* t = done_queue.front(); done_queue.pop();
                    lk.unlock(); tasks.Release(t); lk.lock();
                }
                lk.unlock();

                auto pk = std::make_unique<ParmarPack>();
                pk->mod_id = "IM_" + std::to_string(seq % 5);
                pk->func_id = "w"; pk->show_explanation = false;
                Task* t = tasks.Acquire(std::move(pk));
                if (!t) { std::this_thread::sleep_for(1ms); continue; }
                workers.Enqueue([t, &bus, &done_mutex, &done_queue, &done_cv]() {
                    bus.Emit(t->CurrentPack()->mod_id + ".w",
                             t->CurrentPack());
                    { std::lock_guard lk(done_mutex); done_queue.push(t); }
                    done_cv.notify_one();
                });
                seq++;
            }
        });

        std::this_thread::sleep_for(10s);
        stop.store(true);
        submitter.join();

        // Drain
        for (int w = 0; w < 100; ++w) {
            std::unique_lock lk(done_mutex);
            done_cv.wait_for(lk, 20ms, [&]{ return !done_queue.empty(); });
            while (!done_queue.empty()) {
                Task* t = done_queue.front(); done_queue.pop();
                lk.unlock(); tasks.Release(t); lk.lock();
            }
            if (tasks.GetFreeCount() == tasks.GetTotalCount()) break;
        }

        std::cout << "  [after 10s stress] exec=" << count.load()
                  << " pool=" << tasks.GetFreeCount() << "/" << tasks.GetTotalCount()
                  << std::endl;
    }  // workers + tasks destroyed

    // ---- Phase 2: Unload all modules ----
    for (int i = 0; i < 5; ++i)
        mgr.UnloadModule("IM_" + std::to_string(i));
    for (auto& sig : bus.GetSignalNames())
        if (sig.find("IM_") == 0) bus.RemoveSignal(sig);

    std::this_thread::sleep_for(500ms);  // let OS reclaim memory

    // ---- Phase 3: Measure ----
    size_t after = GetWorkingSetKB();
    long long delta = static_cast<long long>(after) - static_cast<long long>(before);

    std::cout << "  [idle after stress] before: " << before
              << " KB → after: " << after << " KB"
              << " (delta: " << delta << " KB)" << std::endl;

    // After everything is destroyed and modules unloaded, the only
    // persistent things are the EventBus and ModuleLifeManager singletons.
    // These should be < 100KB combined (empty maps + mutex).
    // The OS may not reclaim all memory immediately (working set is lazy),
    // so we use a generous threshold.
    EXPECT_LT(delta, 5120)
        << "Memory not released after stress → idle: " << delta << " KB"
        << " (possible leak)";
}

// ================================================================
//  Summarize: print thread stack default on this platform
// ================================================================
TEST(IdleResource, ReportThreadStackSize)
{
#ifdef _WIN32
    // Get default thread stack size by creating a thread and querying
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    size_t stack_size = (high - low) / 1024;
    std::cout << "\n  === Platform Info ===" << std::endl;
    std::cout << "  CPU cores:      " << std::thread::hardware_concurrency() << std::endl;
    std::cout << "  Main thread stack: ~" << stack_size << " KB" << std::endl;
    std::cout << "  Default new thread stack: 1024 KB (Windows)" << std::endl;
    std::cout << "  Each worker thread costs:  ~1024 KB virtual + working set on first touch" << std::endl;
#endif
    SUCCEED();
}
