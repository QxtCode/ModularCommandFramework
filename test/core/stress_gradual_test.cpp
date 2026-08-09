/// =================================================================
///  Graduated Stress — 2min → 3min, leaves 2 cores free
/// =================================================================

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "core/ThreadPool.h"
#include "core/TasksPool.h"
#include "core/Task.h"
#include "core/ModuleLifeManager.h"
#include "core/ParmarPack.h"
#include "sdk/IModule.h"
#include "event_bus/event_bus.h"

using namespace std::chrono_literals;

// ---- Module ----
class GModule : public ModuleBaseObject
{
public:
    GModule(std::string n, std::atomic<int>* exec, int us = 50)
        : name_(std::move(n)), exec_(exec), burn_us_(us) {}
    const char* GetName() const override { return name_.c_str(); }
    bool OnInit() override {
        REGISTER_FUNC("w", "", {
            if (burn_us_ > 0) {
                auto end = std::chrono::steady_clock::now()
                    + std::chrono::microseconds(burn_us_);
                while (std::chrono::steady_clock::now() < end)
                    { volatile int x=0; (void)x; }
            }
            if (exec_) exec_->fetch_add(1);
            pack->success = true;
        });
        return true;
    }
private:
    std::string name_; std::atomic<int>* exec_; int burn_us_;
};

static void Drain(std::mutex& m, std::queue<Task*>& q, TasksPool& p) {
    std::unique_lock lk(m);
    while (!q.empty()) { Task* t = q.front(); q.pop(); lk.unlock(); p.Release(t); lk.lock(); }
}

// ---- The test runner ----
static void RunStress(int duration_sec)
{
    auto& mgr = ModuleLifeManager::GetInstance();
    auto& bus = EventBus::GetInstance();

    unsigned hw = std::thread::hardware_concurrency();
    int workers_n = (hw > 4) ? static_cast<int>(hw) - 2 : 2;

    std::cout << "\n========== " << duration_sec << "s Stress ("
              << workers_n << " workers, " << hw - workers_n
              << " cores reserved) ==========\n\n";

    std::atomic<bool> stop{false};
    std::atomic<int> total_exec{0}, load_ok{0}, load_fail{0};
    std::atomic<int> unload_ok{0}, unload_fail{0};
    std::atomic<int> emit_fail{0}, dispatch_fail{0};

    ThreadPool workers(workers_n);
    TasksPool tasks(48);
    std::mutex done_mutex;
    std::queue<Task*> done_queue;

    for (int i = 0; i < 5; ++i)
        mgr.AddModule(std::make_unique<GModule>(
            "G_" + std::to_string(i), &total_exec, 40));

    // ---- Submitters (×2) ----
    auto submitter = [&](int s) {
        int seq = s;
        while (!stop.load()) {
            Drain(done_mutex, done_queue, tasks);
            auto pk = std::make_unique<ParmarPack>();
            pk->mod_id = "G_" + std::to_string(seq % 5);
            pk->func_id = "w"; pk->show_explanation = false;
            Task* t = tasks.Acquire(std::move(pk));
            if (!t) { std::this_thread::sleep_for(1ms); continue; }
            workers.Enqueue([t, &bus, &done_mutex, &done_queue, &emit_fail]() {
                bool ok = bus.Emit(t->CurrentPack()->mod_id + ".w",
                                   t->CurrentPack());
                if (!ok) emit_fail.fetch_add(1);
                { std::lock_guard lk(done_mutex); done_queue.push(t); }
            });
            seq++;
        }
    };

    // ---- Flapper ----
    auto flapper = [&]() {
        int c = 0;
        while (!stop.load()) {
            std::this_thread::sleep_for(10ms);
            int idx = c % 5; c++;
            if (mgr.UnloadModule("G_" + std::to_string(idx)))
                unload_ok.fetch_add(1); else unload_fail.fetch_add(1);
            auto m = std::make_unique<GModule>(
                "G_" + std::to_string(idx), &total_exec, 30);
            if (mgr.AddModule(std::move(m)))
                load_ok.fetch_add(1); else load_fail.fetch_add(1);
        }
    };

    // ---- Dispatcher ----
    auto dispatcher = [&]() {
        int s = 0;
        while (!stop.load()) {
            std::this_thread::sleep_for(5ms);
            ParmarPack p; p.mod_id = "G_" + std::to_string(s % 5);
            p.func_id = "w"; if (!mgr.Dispatch(&p)) dispatch_fail.fetch_add(1);
            s++;
        }
    };

    // ---- Bus querier ----
    auto querier = [&]() {
        while (!stop.load()) {
            std::this_thread::sleep_for(5ms);
            for (auto& n : bus.GetSignalNames())
                if (n.find("G_") == 0) bus.GetSlotCount(n);
        }
    };

    auto t0 = std::chrono::steady_clock::now();

    std::thread t1(submitter, 0);
    std::thread t2(submitter, 100);
    std::thread t3(flapper);
    std::thread t4(dispatcher);
    std::thread t5(querier);

    // Progress every 30s
    for (int sec = 30; sec <= duration_sec; sec += 30)
    {
        std::this_thread::sleep_for(30s);
        auto elap = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::cout << "  [" << elap << "s] exec=" << total_exec.load()
                  << " L=" << load_ok.load() << "/" << load_fail.load()
                  << " U=" << unload_ok.load() << "/" << unload_fail.load()
                  << " Ef=" << emit_fail.load()
                  << " Df=" << dispatch_fail.load()
                  << " pool=" << tasks.GetFreeCount() << "/" << tasks.GetTotalCount()
                  << std::endl;
    }

    stop.store(true);
    t1.join(); t2.join(); t3.join();
    t4.join(); t5.join();

    // Final drain
    for (int w = 0; w < 300; ++w) {
        Drain(done_mutex, done_queue, tasks);
        if (tasks.GetFreeCount() == tasks.GetTotalCount()) break;
        std::this_thread::sleep_for(10ms);
    }

    // Cleanup
    for (int i = 0; i < 5; ++i) mgr.UnloadModule("G_" + std::to_string(i));
    for (auto& n : bus.GetSignalNames())
        if (n.find("G_") == 0) bus.RemoveSignal(n);

    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    size_t free_cnt = tasks.GetFreeCount();

    std::cout << "\n  " << duration_sec << "s result: exec=" << total_exec.load()
              << " L=" << load_ok.load() << " U=" << unload_ok.load()
              << " Ef=" << emit_fail.load() << " Df=" << dispatch_fail.load()
              << " pool=" << free_cnt << "/" << tasks.GetTotalCount()
              << " time=" << total_ms << "ms"
              << std::endl;

    EXPECT_EQ(free_cnt, tasks.GetTotalCount())
        << "Pool leak: " << free_cnt << "/" << tasks.GetTotalCount();
}

TEST(StressGraduated, TwoMinutes)  { RunStress(120); }
TEST(StressGraduated, ThreeMinutes) { RunStress(180); }
