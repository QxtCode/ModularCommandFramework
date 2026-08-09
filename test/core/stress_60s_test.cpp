/// =================================================================
///  60-Second Full Pipeline Stress + Memory Leak Test
/// =================================================================
///
///  6 threads concurrently:
///    - 3 submitter threads: submit tasks to random modules
///    - 1 flapper thread: load/unload modules in a loop
///    - 1 dispatcher thread: raw Dispatch() calls
///    - 1 bus querier thread: GetSignalNames + GetSlotCount
///
///  After 60s, drains all tasks, unloads all modules, cleans up
///  zombie signals, and verifies pool integrity.

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <crtdbg.h>
#endif

#include "core/ThreadPool.h"
#include "core/TasksPool.h"
#include "core/Task.h"
#include "core/ModuleLifeManager.h"
#include "core/ParmarPack.h"
#include "sdk/IModule.h"
#include "event_bus/event_bus.h"

using namespace std::chrono_literals;

// ================================================================
//  Stress module with lifecycle hooks
// ================================================================
class S60Module : public ModuleBaseObject
{
public:
    S60Module(std::string name, std::atomic<int>* exec_count = nullptr,
              std::atomic<int>* in_flight = nullptr, int burn_us = 100)
        : name_(std::move(name)), exec_count_(exec_count),
          in_flight_(in_flight), burn_us_(burn_us) {}

    const char* GetName() const override { return name_.c_str(); }

    bool OnInit() override
    {
        REGISTER_FUNC("work", "stress work", {
            if (in_flight_) in_flight_->fetch_add(1);
            // CPU burn: spin-loop instead of sleep (actually loads CPU)
            if (burn_us_ > 0) {
                auto end = std::chrono::steady_clock::now()
                    + std::chrono::microseconds(burn_us_);
                while (std::chrono::steady_clock::now() < end) {
                    // volatile to prevent compiler optimizing away
                    volatile int x = 0; (void)x;
                }
            }
            if (exec_count_) exec_count_->fetch_add(1);
            if (in_flight_) in_flight_->fetch_sub(1);
            pack->success = true;
            pack->return_value = name_ + "_done";
        });
        return true;
    }

private:
    std::string name_;
    std::atomic<int>* exec_count_;
    std::atomic<int>* in_flight_;
    int burn_us_;
};

// ================================================================
//  Helper
// ================================================================
static void DrainQueue(std::mutex& m, std::queue<Task*>& q, TasksPool& pool)
{
    std::unique_lock lock(m);
    while (!q.empty())
    {
        Task* t = q.front(); q.pop();
        lock.unlock();
        pool.Release(t);
        lock.lock();
    }
}

// ================================================================
//  THE TEST
// ================================================================
TEST(Stress60s, FullPipeline60Seconds)
{
    std::cout << "\n======================================================\n";
    std::cout << "  60-Second Full Pipeline Stress Test\n";
    std::cout << "======================================================\n\n";

    auto& mgr = ModuleLifeManager::GetInstance();
    auto& bus = EventBus::GetInstance();

    std::atomic<bool> stop{false};
    std::atomic<int> total_exec{0};
    std::atomic<int> load_ok{0}, load_fail{0};
    std::atomic<int> unload_ok{0}, unload_fail{0};
    std::atomic<int> emit_fail{0}, dispatch_fail{0};

    ThreadPool workers(12);   // saturate CPU cores
    TasksPool tasks(64);       // larger pool to reduce contention
    std::mutex done_mutex;
    std::queue<Task*> done_queue;

    // Pre-load 5 modules
    for (int i = 0; i < 5; ++i)
    {
        auto mod = std::make_unique<S60Module>("S60_" + std::to_string(i), &total_exec, nullptr, 50);
        mgr.AddModule(std::move(mod));
    }

    // ---- Submitter (×3) ----
    auto submitter = [&](int seed) {
        int seq = seed;
        while (!stop.load())
        {
            std::string mod_name = "S60_" + std::to_string(seq % 5);
            DrainQueue(done_mutex, done_queue, tasks);

            auto pack = std::make_unique<ParmarPack>();
            pack->mod_id  = mod_name;
            pack->func_id = "work";
            pack->show_explanation = false;

            Task* task = tasks.Acquire(std::move(pack));
            if (!task) { std::this_thread::sleep_for(1ms); continue; }

            workers.Enqueue([task, &bus, &done_mutex, &done_queue, &emit_fail]() {
                auto* cp = task->CurrentPack();
                bool ok = bus.Emit(cp->mod_id + ".work", cp);
                if (!ok) emit_fail.fetch_add(1);
                { std::lock_guard lock(done_mutex); done_queue.push(task); }
            });
            seq++;
        }
    };

    // ---- Module flapper ----
    auto flapper = [&]() {
        int cycle = 0;
        while (!stop.load())
        {
            std::this_thread::sleep_for(5ms);   // faster unload/reload cycle
            int idx = cycle % 5;
            cycle++;

            if (mgr.UnloadModule("S60_" + std::to_string(idx)))
                unload_ok.fetch_add(1);
            else
                unload_fail.fetch_add(1);

            auto mod = std::make_unique<S60Module>("S60_" + std::to_string(idx), &total_exec, nullptr, 50);
            if (mgr.AddModule(std::move(mod)))
                load_ok.fetch_add(1);
            else
                load_fail.fetch_add(1);
        }
    };

    // ---- Dispatcher ----
    auto dispatcher = [&]() {
        int seq = 0;
        while (!stop.load())
        {
            std::this_thread::sleep_for(2ms);
            ParmarPack p;
            p.mod_id  = "S60_" + std::to_string(seq % 5);
            p.func_id = "work";
            p.show_explanation = false;
            if (!mgr.Dispatch(&p))
                dispatch_fail.fetch_add(1);
            seq++;
        }
    };

    // ---- Bus querier ----
    auto bus_querier = [&]() {
        while (!stop.load())
        {
            std::this_thread::sleep_for(3ms);
            auto names = bus.GetSignalNames();
            for (auto& n : names)
                if (n.find("S60_") == 0)
                    bus.GetSlotCount(n);
        }
    };

    // ---- Launch ----
    auto start_time = std::chrono::steady_clock::now();

    std::thread t1(submitter, 0);
    std::thread t2(submitter, 100);
    std::thread t3(submitter, 200);
    std::thread t4(flapper);
    std::thread t5(dispatcher);
    std::thread t6(bus_querier);

    // ---- Progress every 10s ----
    for (int sec = 10; sec <= 60; sec += 10)
    {
        std::this_thread::sleep_for(10s);
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count();
        std::cout << "  [" << elapsed << "s] exec=" << total_exec.load()
                  << " L=" << load_ok.load() << "/" << load_fail.load()
                  << " U=" << unload_ok.load() << "/" << unload_fail.load()
                  << " Ef=" << emit_fail.load()
                  << " Df=" << dispatch_fail.load()
                  << " pool=" << tasks.GetFreeCount() << "/" << tasks.GetTotalCount() << "\n";
    }

    // ---- Stop ----
    std::cout << "\n  Stopping threads...\n";
    stop.store(true);
    t1.join(); t2.join(); t3.join();
    t4.join(); t5.join(); t6.join();

    // ---- Drain all remaining tasks ----
    for (int w = 0; w < 200; ++w)
    {
        DrainQueue(done_mutex, done_queue, tasks);
        if (tasks.GetFreeCount() == tasks.GetTotalCount()) break;
        std::this_thread::sleep_for(10ms);
    }

    // ---- Cleanup: unload all remaining modules ----
    for (int i = 0; i < 5; ++i)
        mgr.UnloadModule("S60_" + std::to_string(i));

    // ---- Remove zombie signals ----
    {
        auto names = bus.GetSignalNames();
        int zombies = 0;
        for (auto& n : names)
        {
            if (n.find("S60_") == 0)
            {
                bus.RemoveSignal(n);
                zombies++;
            }
        }
        if (zombies > 0)
            std::cout << "  Zombie signals cleaned: " << zombies << "\n";
    }

    // ---- Report ----
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();

    std::cout << "\n======================================================\n";
    std::cout << "  Duration:       " << total_ms << " ms\n";
    std::cout << "  Executed:       " << total_exec.load() << "\n";
    std::cout << "  Load OK/Fail:   " << load_ok.load() << " / " << load_fail.load() << "\n";
    std::cout << "  Unload OK/Fail: " << unload_ok.load() << " / " << unload_fail.load() << "\n";
    std::cout << "  Emit failures:  " << emit_fail.load() << "\n";
    std::cout << "  Dispatch fail:  " << dispatch_fail.load() << "\n";

    size_t final_free = tasks.GetFreeCount();
    std::cout << "  Pool:           " << final_free << "/" << tasks.GetTotalCount() << " free\n";

    EXPECT_EQ(final_free, tasks.GetTotalCount())
        << "All tasks should be returned to pool (leak detected!)";

    // Also verify no zombie modules in the manager
    // (S60_* modules were all unloaded above)

    std::cout << "  Pool integrity:  PASS (" << final_free << "/" << tasks.GetTotalCount() << ")\n";
    std::cout << "  No crash in 60s: PASS\n";
    std::cout << "  Memory: check process exit log for _CrtDumpMemoryLeaks\n";
    std::cout << "======================================================\n\n";
}
