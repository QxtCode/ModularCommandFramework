/// =================================================================
///  DLL Lifecycle Stress Tests — 隔离→集成→跨线程 层层加码
/// =================================================================
///
///  测试层级:
///   L1: 隔离测试 — 单线程 Load→Execute→Unload 基本功能
///   L2: 并发执行 — Load 后多线程同时 Emit，然后 Unload
///   L3: 跨线程卸载 — 执行期间从另一个线程 Unload
///   L4: 反复加载卸载 — 同一个 DLL 反复 Load/Unload N 轮
///   L5: 混合场景 — Load/Unload + Execute + Dispatch 同时进行
///   L6: OnShutdown/boundary — OnShutdown 正确调用、DoubleUnload、UnloadNonexistent

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/ThreadPool.h"
#include "core/TasksPool.h"
#include "core/Task.h"
#include "core/ModuleLifeManager.h"
#include "core/ParmarPack.h"
#include "sdk/IModule.h"
#include "event_bus/event_bus.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace std::chrono_literals;

// ================================================================
//  Helper: read counter from TestDLLModule.dll via GetProcAddress
// ================================================================
#ifdef _WIN32
static int ReadDLLCounter(const char* func_name)
{
    // The DLL is already loaded — find its handle via GetModuleHandle
    HMODULE h = GetModuleHandleA("TestDLLModule.dll");
    if (!h) return -1;
    auto fn = reinterpret_cast<int(*)()>(GetProcAddress(h, func_name));
    if (!fn) return -1;
    return fn();
}

static bool IsDLLLoaded()
{
    return GetModuleHandleA("TestDLLModule.dll") != nullptr;
}
#endif

// ================================================================
//  Built-in module with controllable lifecycle hooks
// ================================================================
class LifecycleModule : public ModuleBaseObject
{
public:
    LifecycleModule(std::string name,
                    std::atomic<int>* init_count   = nullptr,
                    std::atomic<int>* shutdown_count = nullptr,
                    std::atomic<int>* exec_count   = nullptr,
                    std::atomic<int>* in_flight    = nullptr,
                    int delay_ms = 0)
        : name_(std::move(name)), init_count_(init_count),
          shutdown_count_(shutdown_count), exec_count_(exec_count),
          in_flight_(in_flight), delay_ms_(delay_ms) {}

    const char* GetName() const override { return name_.c_str(); }

    bool OnInit() override
    {
        if (init_count_) init_count_->fetch_add(1);
        REGISTER_FUNC("work", "do work", {
            if (in_flight_) in_flight_->fetch_add(1);
            if (delay_ms_ > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
            if (exec_count_) exec_count_->fetch_add(1);
            if (in_flight_) in_flight_->fetch_sub(1);
            pack->success = true;
            pack->return_value = name_ + "_done";
        });
        REGISTER_FUNC("ping", "quick ping", {
            if (exec_count_) exec_count_->fetch_add(1);
            pack->success = true;
        });
        return true;
    }

    void OnShutdown() override
    {
        if (shutdown_count_) shutdown_count_->fetch_add(1);
    }

private:
    std::string name_;
    std::atomic<int>* init_count_;
    std::atomic<int>* shutdown_count_;
    std::atomic<int>* exec_count_;
    std::atomic<int>* in_flight_;
    int delay_ms_;
};

// ================================================================
//  Fixture: sets up the full pipeline
// ================================================================
class DLLLifecycleTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        mgr_ = &ModuleLifeManager::GetInstance();
        bus_ = &EventBus::GetInstance();
    }

    void TearDown() override
    {
        // Clean up any leftover lifecycle modules
        for (auto& sig : bus_->GetSignalNames())
        {
            if (sig.find("Lifecycle_") == 0 || sig.find("StressDLL_") == 0)
                bus_->RemoveSignal(sig);
        }
    }

    ModuleLifeManager* mgr_;
    EventBus* bus_;
};

// ================================================================
//  L1: 隔离 — 单线程 Load→Execute→Unload (DLL)
// ================================================================
#ifdef _WIN32
TEST_F(DLLLifecycleTest, DLL_LoadExecuteUnload)
{
    // Load (DLL may already be loaded by other tests — LoadLibrary is ref-counted)
    ASSERT_TRUE(mgr_->LoadDLLModule("TestDLLModule.dll"));
    ASSERT_NE(mgr_->GetModule("TestDLL"), nullptr);

    // Execute — via EventBus Emit
    ParmarPack pack;
    pack.mod_id  = "TestDLL";
    pack.func_id = "ping";
    pack.Set("msg", "hello_dll");
    pack.show_explanation = false;

    bool emitted = bus_->Emit("TestDLL.ping", &pack);
    EXPECT_TRUE(emitted);
    EXPECT_TRUE(pack.success);

    // Unload
    ASSERT_TRUE(mgr_->UnloadModule("TestDLL"));
    EXPECT_EQ(mgr_->GetModule("TestDLL"), nullptr);

    // After unload: Dispatch returns 404
    ParmarPack pack2;
    pack2.mod_id = "TestDLL";
    EXPECT_FALSE(mgr_->Dispatch(&pack2));
    EXPECT_EQ(pack2.error.code, 404);

    // After unload: Emit returns false
    EXPECT_FALSE(bus_->Emit("TestDLL.ping", &pack2));

    // No zombie signals
    for (auto& sig : bus_->GetSignalNames())
        EXPECT_TRUE(sig.find("TestDLL.") == std::string::npos)
            << "Zombie signal: " << sig;
}
#endif

// ================================================================
//  L1: 隔离 — 单线程 Load→Execute→Unload (built-in module)
// ================================================================
TEST_F(DLLLifecycleTest, Builtin_LoadExecuteUnload)
{
    std::atomic<int> init_count{0};
    std::atomic<int> shutdown_count{0};
    std::atomic<int> exec_count{0};

    // Load
    auto mod = std::make_unique<LifecycleModule>(
        "Lifecycle_Basic", &init_count, &shutdown_count, &exec_count, nullptr, 0);
    ASSERT_TRUE(mgr_->AddModule(std::move(mod)));
    EXPECT_EQ(init_count.load(), 1);
    EXPECT_EQ(shutdown_count.load(), 0);
    ASSERT_NE(mgr_->GetModule("Lifecycle_Basic"), nullptr);

    // Execute
    ParmarPack pack;
    pack.mod_id  = "Lifecycle_Basic";
    pack.func_id = "work";
    pack.show_explanation = false;
    EXPECT_TRUE(bus_->Emit("Lifecycle_Basic.work", &pack));
    EXPECT_TRUE(pack.success);
    EXPECT_EQ(exec_count.load(), 1);

    // Unload
    ASSERT_TRUE(mgr_->UnloadModule("Lifecycle_Basic"));
    EXPECT_EQ(shutdown_count.load(), 1);
    EXPECT_EQ(mgr_->GetModule("Lifecycle_Basic"), nullptr);

    // Emit after unload → not found
    EXPECT_FALSE(bus_->Emit("Lifecycle_Basic.work", &pack));
}

// ================================================================
//  L1: 隔离 — UnloadNonexistent / DoubleUnload
// ================================================================
TEST_F(DLLLifecycleTest, Builtin_UnloadEdgeCases)
{
    EXPECT_FALSE(mgr_->UnloadModule("never_loaded_xyz"));

    auto mod = std::make_unique<LifecycleModule>("EdgeCase", nullptr, nullptr, nullptr, nullptr, 0);
    ASSERT_TRUE(mgr_->AddModule(std::move(mod)));

    EXPECT_TRUE(mgr_->UnloadModule("EdgeCase"));
    EXPECT_FALSE(mgr_->UnloadModule("EdgeCase"));  // double unload → false
    EXPECT_EQ(mgr_->GetModule("EdgeCase"), nullptr);
}

// ================================================================
//  L2: 并发执行 — 多线程同时 Emit，确认全部完成后再 Unload
// ================================================================
TEST_F(DLLLifecycleTest, Builtin_MultiThreadEmitBeforeUnload)
{
    std::atomic<int> exec_count{0};
    std::atomic<int> shutdown_count{0};

    auto mod = std::make_unique<LifecycleModule>(
        "Lifecycle_MT", nullptr, &shutdown_count, &exec_count, nullptr, 50);
    ASSERT_TRUE(mgr_->AddModule(std::move(mod)));

    ThreadPool workers(8);
    constexpr int N = 100;

    // 8 threads, each sending tasks (distribute evenly)
    std::vector<std::future<int>> futures;
    for (int t = 0; t < 8; ++t)
    {
        int batch = N / 8 + (t < N % 8 ? 1 : 0);  // handle remainder
        futures.push_back(workers.Submit([&, batch]() -> int {
            int local = 0;
            for (int i = 0; i < batch; ++i)
            {
                ParmarPack pack;
                pack.mod_id  = "Lifecycle_MT";
                pack.func_id = "work";
                pack.show_explanation = false;
                bus_->Emit("Lifecycle_MT.work", &pack);
                if (pack.success) local++;
            }
            return local;
        }));
    }

    int total = 0;
    for (auto& f : futures)
        total += f.get();

    EXPECT_EQ(total, N);
    EXPECT_EQ(exec_count.load(), N);

    // Now unload — all tasks should have finished
    ASSERT_TRUE(mgr_->UnloadModule("Lifecycle_MT"));
    EXPECT_EQ(shutdown_count.load(), 1);
}

// ================================================================
//  L3: 跨线程卸载 — 执行期间从另一个线程 Unload
// ================================================================
TEST_F(DLLLifecycleTest, Builtin_UnloadDuringExecution)
{
    std::atomic<int> exec_count{0};
    std::atomic<int> in_flight{0};
    std::atomic<int> shutdown_count{0};

    auto mod = std::make_unique<LifecycleModule>(
        "Lifecycle_UnloadDuring", nullptr, &shutdown_count,
        &exec_count, &in_flight, 300);  // 300ms — enough window
    ASSERT_TRUE(mgr_->AddModule(std::move(mod)));

    ThreadPool workers(4);
    TasksPool tasks(16);
    std::mutex done_mutex;
    std::queue<Task*> done_queue;

    // Submit 12 tasks (each takes 300ms, 4 workers → 3 batches)
    for (int i = 0; i < 12; ++i)
    {
        auto pack = std::make_unique<ParmarPack>();
        pack->mod_id  = "Lifecycle_UnloadDuring";
        pack->func_id = "work";
        pack->show_explanation = false;

        Task* task = tasks.Acquire(std::move(pack));
        ASSERT_NE(task, nullptr) << "Task pool exhausted at i=" << i;

        workers.Enqueue([task, this, &done_mutex, &done_queue]() {
            try { while (task->Step(*bus_)) {} }
            catch (...) {}
            { std::lock_guard lock(done_mutex); done_queue.push(task); }
        });
    }

    // Wait for execution to start
    for (int w = 0; w < 50 && in_flight.load() == 0; ++w)
        std::this_thread::sleep_for(10ms);
    ASSERT_GT(in_flight.load(), 0) << "No tasks started executing";

    std::cout << "[L3] " << in_flight.load()
              << " tasks in-flight. Unloading now..." << std::endl;

    // ★ Unload from THIS thread while workers are executing ★
    auto unload_start = std::chrono::steady_clock::now();
    ASSERT_TRUE(mgr_->UnloadModule("Lifecycle_UnloadDuring"));
    auto unload_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - unload_start).count();

    EXPECT_EQ(shutdown_count.load(), 1);
    std::cout << "[L3] Unload took " << unload_ms
              << "ms (expected ~300ms — waited for in-flight slots)"
              << std::endl;

    // Drain remaining tasks
    for (int w = 0; w < 50; ++w)
    {
        {
            std::unique_lock lock(done_mutex);
            while (!done_queue.empty())
            {
                Task* t = done_queue.front(); done_queue.pop();
                lock.unlock();
                tasks.Release(t);
                lock.lock();
            }
        }
        if (tasks.GetFreeCount() == tasks.GetTotalCount()) break;
        std::this_thread::sleep_for(20ms);
    }

    std::cout << "[L3] Completed: " << exec_count.load()
              << " executions (some tasks may have failed after unload)"
              << std::endl;

    // After unload, Emit should fail
    ParmarPack p;
    p.mod_id = "Lifecycle_UnloadDuring";
    p.func_id = "work";
    EXPECT_FALSE(bus_->Emit("Lifecycle_UnloadDuring.work", &p));
}

// ================================================================
//  L4: 反复加载卸载 — 同一个模块反复 Load/Unload 100 轮
// ================================================================
TEST_F(DLLLifecycleTest, Builtin_RepeatedLoadUnload)
{
    std::atomic<int> total_init{0};
    std::atomic<int> total_shutdown{0};
    std::atomic<int> total_exec{0};

    constexpr int ROUNDS = 100;

    for (int round = 0; round < ROUNDS; ++round)
    {
        // Load
        auto mod = std::make_unique<LifecycleModule>(
            "Lifecycle_Cycle", &total_init, &total_shutdown, &total_exec, nullptr, 1);
        ASSERT_TRUE(mgr_->AddModule(std::move(mod)));

        // Execute once
        ParmarPack pack;
        pack.mod_id  = "Lifecycle_Cycle";
        pack.func_id = "work";
        pack.show_explanation = false;
        EXPECT_TRUE(bus_->Emit("Lifecycle_Cycle.work", &pack));
        EXPECT_TRUE(pack.success);

        // Unload
        ASSERT_TRUE(mgr_->UnloadModule("Lifecycle_Cycle"));
        EXPECT_EQ(mgr_->GetModule("Lifecycle_Cycle"), nullptr);

        // Verify clean — no zombie signals
        for (auto& sig : bus_->GetSignalNames())
            ASSERT_TRUE(sig.find("Lifecycle_Cycle.") == std::string::npos)
                << "Round " << round << ": zombie signal " << sig;
    }

    EXPECT_EQ(total_init.load(), ROUNDS);
    EXPECT_EQ(total_shutdown.load(), ROUNDS);
    EXPECT_EQ(total_exec.load(), ROUNDS);
}

// ================================================================
//  L5: 混合场景 — 两个线程同时 Load/Unload/Execute 不同模块
// ================================================================
TEST_F(DLLLifecycleTest, Builtin_ChaosLoadUnloadExecute)
{
    std::atomic<bool> stop{false};
    std::atomic<int> total_exec{0};
    std::atomic<int> load_count{0};
    std::atomic<int> unload_count{0};
    std::atomic<int> emit_failures{0};

    ThreadPool workers(6);
    TasksPool tasks(24);
    std::mutex done_mutex;
    std::queue<Task*> done_queue;

    // Pre-load 3 modules
    for (int i = 0; i < 3; ++i)
    {
        auto mod = std::make_unique<LifecycleModule>(
            "Chaos_" + std::to_string(i), nullptr, nullptr, &total_exec, nullptr, 5);
        mgr_->AddModule(std::move(mod));
    }

    // Thread A: submit tasks to random modules
    std::thread submitter([&]() {
        int seq = 0;
        while (!stop.load())
        {
            std::string name = "Chaos_" + std::to_string(seq % 5);

            // Drain
            {
                std::unique_lock lock(done_mutex);
                while (!done_queue.empty())
                {
                    Task* t = done_queue.front(); done_queue.pop();
                    lock.unlock(); tasks.Release(t); lock.lock();
                }
            }

            auto pack = std::make_unique<ParmarPack>();
            pack->mod_id  = name;
            pack->func_id = "work";
            pack->show_explanation = false;

            Task* task = tasks.Acquire(std::move(pack));
            if (!task) { std::this_thread::sleep_for(1ms); continue; }

            workers.Enqueue([task, this, &done_mutex, &done_queue, &emit_failures]() {
                auto* cp = task->CurrentPack();
                bool ok = bus_->Emit(cp->mod_id + ".work", cp);
                if (!ok) emit_failures.fetch_add(1);
                { std::lock_guard lock(done_mutex); done_queue.push(task); }
            });
            seq++;
        }
    });

    // Thread B: load/unload non-existent modules
    std::thread flapper([&]() {
        int cycle = 0;
        while (!stop.load())
        {
            std::this_thread::sleep_for(30ms);
            std::string name = "Chaos_" + std::to_string(cycle % 5);

            // Try to unload (might fail if already unloaded)
            if (mgr_->UnloadModule(name))
                unload_count.fetch_add(1);

            // Always try to load (might fail if already loaded)
            auto mod = std::make_unique<LifecycleModule>(
                name, nullptr, nullptr, &total_exec, nullptr, 3);
            if (mgr_->AddModule(std::move(mod)))
                load_count.fetch_add(1);

            cycle++;
        }
    });

    std::this_thread::sleep_for(5s);
    stop.store(true);

    submitter.join();
    flapper.join();

    // Final drain
    for (int w = 0; w < 100; ++w)
    {
        {
            std::unique_lock lock(done_mutex);
            while (!done_queue.empty())
            {
                Task* t = done_queue.front(); done_queue.pop();
                lock.unlock(); tasks.Release(t); lock.lock();
            }
        }
        if (tasks.GetFreeCount() == tasks.GetTotalCount()) break;
        std::this_thread::sleep_for(10ms);
    }

    // Cleanup
    for (int i = 0; i < 5; ++i)
        mgr_->UnloadModule("Chaos_" + std::to_string(i));
    for (auto& sig : bus_->GetSignalNames())
        if (sig.find("Chaos_") == 0) bus_->RemoveSignal(sig);

    std::cout << "[L5] Chaos: exec=" << total_exec.load()
              << " loads=" << load_count.load()
              << " unloads=" << unload_count.load()
              << " emit_fails=" << emit_failures.load() << std::endl;
}

// ================================================================
//  L6: OnShutdown + 边界情况
// ================================================================
TEST_F(DLLLifecycleTest, Builtin_OnShutdownCalledExactlyOnce)
{
    std::atomic<int> shutdown_count{0};

    auto mod = std::make_unique<LifecycleModule>(
        "ShutdownTest", nullptr, &shutdown_count, nullptr, nullptr, 0);
    ASSERT_TRUE(mgr_->AddModule(std::move(mod)));

    // Execute some tasks
    for (int i = 0; i < 5; ++i)
    {
        ParmarPack pack;
        pack.mod_id  = "ShutdownTest";
        pack.func_id = "work";
        pack.show_explanation = false;
        bus_->Emit("ShutdownTest.work", &pack);
    }

    // Unload
    ASSERT_TRUE(mgr_->UnloadModule("ShutdownTest"));
    EXPECT_EQ(shutdown_count.load(), 1);

    // Double unload should NOT call OnShutdown again
    EXPECT_FALSE(mgr_->UnloadModule("ShutdownTest"));
    EXPECT_EQ(shutdown_count.load(), 1) << "OnShutdown called more than once!";
}

// ================================================================
//  L6: 加载失败不残留
// ================================================================
TEST_F(DLLLifecycleTest, Builtin_LoadFailureCleanup)
{
    std::atomic<int> init_count{0};
    std::atomic<int> shutdown_count{0};

    // First load: success
    auto mod1 = std::make_unique<LifecycleModule>(
        "FailTest", &init_count, &shutdown_count, nullptr, nullptr, 0);
    ASSERT_TRUE(mgr_->AddModule(std::move(mod1)));
    EXPECT_EQ(init_count.load(), 1);

    // Second load: duplicate → should fail
    auto mod2 = std::make_unique<LifecycleModule>(
        "FailTest", &init_count, &shutdown_count, nullptr, nullptr, 0);
    EXPECT_FALSE(mgr_->AddModule(std::move(mod2)));
    // v2.4: OnInit is called BEFORE the duplicate check (fixes TOCTOU).
    // The duplicate module's OnInit increments the counter, then the lock
    // check discovers the duplicate and the module is discarded.
    // OnShutdown is called when the duplicate module is destroyed.
    EXPECT_EQ(init_count.load(), 2) << "Duplicate module's OnInit IS called";
    EXPECT_EQ(shutdown_count.load(), 0)
        << "OnShutdown called for discarded duplicate (v2.4 shared_ptr behavior)";

    // Clean up
    mgr_->UnloadModule("FailTest");
}

// ================================================================
//  L6: v2.6 快照式 Emit — Unload 不再被慢 Slot 阻塞
// ================================================================
TEST_F(DLLLifecycleTest, Builtin_DispatchBlockedDuringUnload)
{
    std::atomic<int> in_flight{0};

    auto mod = std::make_unique<LifecycleModule>(
        "BlockTest", nullptr, nullptr, nullptr, &in_flight, 300);
    ASSERT_TRUE(mgr_->AddModule(std::move(mod)));

    ThreadPool workers(2);

    // Start a slow task
    auto fut = workers.Submit([&]() {
        ParmarPack pack;
        pack.mod_id  = "BlockTest";
        pack.func_id = "work";
        pack.show_explanation = false;
        bus_->Emit("BlockTest.work", &pack);
    });

    // Wait for execution to start
    for (int w = 0; w < 50 && in_flight.load() == 0; ++w)
        std::this_thread::sleep_for(10ms);
    ASSERT_GT(in_flight.load(), 0);

    // Start Unload shortly after
    std::this_thread::sleep_for(50ms);
    auto unload_start = std::chrono::steady_clock::now();
    mgr_->UnloadModule("BlockTest");
    auto unload_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - unload_start).count();
    std::cout << "[L6] Unload took " << unload_ms << "ms" << std::endl;

    fut.wait();

    // v2.6: 快照式 Emit 后，Unload(RemoveSignal) 不再被慢 Slot 阻塞。
    // Unload 应在 < 100ms 内完成，即使 Slot 要 300ms。
    EXPECT_LT(unload_ms, 100)
        << "v2.6 snapshot-Emit: Unload should NOT wait for slow Emit (" << unload_ms << "ms)";
}
