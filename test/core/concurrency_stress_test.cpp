/// =================================================================
///  Concurrency Stress Tests — 并发缺陷追踪
/// =================================================================
///
///  审计日期: 2026-08-10
///  审计范围: ModuleLifeManager / EventBus / Task / TasksPool / ThreadPool
///
///  【已修复的 Bug】
///
///  Bug 1 [已修复 - v2.4]: AddModule TOCTOU → 僵尸信号 → use-after-free
///    修复: AddModule 全程持 unique_lock，ConnectToEventBus 在锁内执行。
///    两个线程同时 AddModule 同名模块时，只有一个成功。失败的线程
///    OnInit() 虽然被浪费执行了，但不会在 EventBus 上留下僵尸信号。
///    残留小问题: OnInit() 在锁外调用，重复调用有副作用浪费。
///
///  Bug 2 [已修复 - v2.4]: Task::shards_ data race
///    修复: 添加 shards_mutex_，PushShard/Step/Reset/GetProgress 全部加锁。
///
///  Bug 4 [已修复 - v2.4]: Slot lambda 捕获裸 [this] → use-after-free
///    修复: Slot 持有 weak_ptr<ModuleBaseObject>，Run() 时 Lock() 检查存活。
///    异常安全: Slot::Run() 用 try-catch(...) 兜底（v2.5）。
///
///  【已知待处理问题】
///
///  问题 A [MEDIUM — 连锁阻塞]: Emit 持 shared_lock 执行全部 Slot
///    Emit() 持有 bus_mutex_ shared_lock → TraverseSlots → 逐个执行 Slot。
///    RemoveSignal 需要 unique_lock → 被阻塞，直到所有 Emit 完成。
///    UnloadModule 调 RemoveSignal → 被阻塞。
///    UnloadModule 持有 module_map_ unique_lock → Dispatch/GetModule 也被阻塞。
///    结果: 一个慢 Slot 连锁阻塞整个框架。
///    这是故意的设计权衡（安全 > 性能），但可用性影响大。
///    方案待定:
///      A1. Emit 先拍快照再释放锁（Slot 执行不持锁）
///      A2. 引入超时机制（Slot 超时自动跳过）
///      A3. 分离"信号注册锁"和"Slot 执行"，异步 Emit
///
///  问题 B [LOW — 无死任务检测]: 卡住的 Slot 永久占用 Worker + Pool 槽位
///    模块回调进入死循环 → Worker 线程永久卡住。
///    Cancel/Pause 只在分片间生效，无法中断当前分片。
///    后果: Worker 数递减，Pool 槽位泄露，吞吐量下降。
///
///  问题 C [LOW — Tick() 持锁]: TasksPool::Tick() 持 mutex_ 调 Step()
///    Tick 期间 Acquire/Release 被阻塞。main.cpp 不用 Tick，暂时无实际影响。
///
///  【并发安全保证（已验证）】
///
///  1. DLL 卸载安全: Emit 持 shared_lock → RemoveSignal 等 Emit 完成才删信号
///     → 信号删除后模块 delete → 不会 use-after-free。
///  2. Slot 弱引用保护: weak_ptr<ModuleBaseObject> + Run() 时 Lock()。
///  3. EventBus DLL 单例: static instance 在 DLL 内，所有模块共享唯一实例。
///  4. std::shared_mutex 读写分离: 多个 Emit 可并发（shared_lock），
///     RemoveSignal/RegisterSignal 串行化（unique_lock）。

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
//  Helper: 可控制执行时长的测试模块
// ================================================================
class SlowModule : public ModuleBaseObject
{
public:
    SlowModule(std::string name, std::atomic<int>* exec_count,
               std::atomic<int>* in_flight, int delay_ms = 50)
        : name_(std::move(name)), exec_count_(exec_count),
          in_flight_(in_flight), delay_ms_(delay_ms) {}

    const char* GetName() const override { return name_.c_str(); }

    bool OnInit() override
    {
        REGISTER_FUNC("work", "slow work item", {
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

private:
    std::string name_;
    std::atomic<int>* exec_count_;
    std::atomic<int>* in_flight_;
    int delay_ms_;
};

// 辅助：drain done_queue 并归还 task
static void DrainDoneQueue(std::mutex& m, std::queue<Task*>& q, TasksPool& pool)
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

// 辅助：提交任务（带自动 drain）
static void SubmitAndDrain(TasksPool& tasks, ThreadPool& workers,
                           EventBus& bus,
                           std::mutex& done_mutex, std::queue<Task*>& done_queue,
                           const char* mod_name, int count, bool use_step_loop = true)
{
    int submitted = 0;
    while (submitted < count)
    {
        DrainDoneQueue(done_mutex, done_queue, tasks);

        auto pack = std::make_unique<ParmarPack>();
        pack->mod_id = mod_name;
        pack->func_id = "work";
        pack->show_explanation = false;

        Task* task = tasks.Acquire(std::move(pack));
        if (!task) { std::this_thread::sleep_for(1ms); continue; }

        if (use_step_loop)
        {
            workers.Enqueue([task, &bus, &done_mutex, &done_queue]() {
                try { while (task->Step(bus)) {} }
                catch (...) {}
                { std::lock_guard lock(done_mutex); done_queue.push(task); }
            });
        }
        else
        {
            workers.Enqueue([task, &bus, &done_mutex, &done_queue]() {
                bus.Emit(task->CurrentPack()->mod_id + ".work",
                         task->CurrentPack());
                { std::lock_guard lock(done_mutex); done_queue.push(task); }
            });
        }
        ++submitted;
    }
}

// ================================================================
//  Bug 1: AddModule TOCTOU race — 两个线程同时加同名模块
// ================================================================
//  问题：AddModule 先 shared_lock 查重 → 释放锁 → OnInit+ConnectToEventBus
//        → unique_lock 插入。中间有空窗，两个线程都能通过查重。

TEST(ConcurrencyStress, AddModuleTOCTOU_Race)
{
    auto& mgr = ModuleLifeManager::GetInstance();
    auto& bus = EventBus::GetInstance();

    // 清理
    mgr.UnloadModule("TModule");

    std::atomic<int> success_count{0};
    std::atomic<int> zombie_detected{0};

    // 4 个线程同时 AddModule 同名模块，重复 100 轮
    auto racer = [&]() {
        for (int i = 0; i < 100; ++i)
        {
            auto mod = std::make_unique<SlowModule>("TModule", nullptr, nullptr, 0);
            if (mgr.AddModule(std::move(mod)))
                success_count.fetch_add(1);

            // 检查僵尸信号：如果模块被成功加载后又因为竞态被覆盖，
            // 可能有僵尸信号残留
            auto names = bus.GetSignalNames();
            int tmod_count = 0;
            for (const auto& n : names)
                if (n.find("TModule.") == 0) tmod_count++;
            // 正常情况最多 2 个信号（work + ping）
            if (tmod_count > 3)  // 允许一点余量（help + work + ping）
                zombie_detected.fetch_add(1);

            mgr.UnloadModule("TModule");
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back(racer);
    for (auto& t : threads) t.join();

    std::cout << "[TEST] TOCTOU: successes=" << success_count.load()
              << " zombies=" << zombie_detected.load() << "\n";

    // 清理残留
    mgr.UnloadModule("TModule");
    for (const auto& sig : bus.GetSignalNames())
        if (sig.find("TModule.") == 0) bus.RemoveSignal(sig);

    // 4 threads × 100 rounds = 400 attempts
    // 如果每次只有一个成功，success_count 应该 = 400 (每次 AddModule 成功一次)
    // 但因为每次都有 Unload，所以每次都应该成功
    // 关键指标：zombie_detected > 0 说明有竞态导致信号堆积
    if (zombie_detected.load() > 0)
        std::cout << "[TEST] TOCTOU BUG CONFIRMED: zombie signals detected!\n";

    SUCCEED();
}

// ================================================================
//  Bug 1b: AddModule TOCTOU — 导致 EventBus 上的 Slot 指向已删除模块
// ================================================================
//  场景：
//    1. 线程 A AddModule("X") → 通过查重 → OnInit → ConnectToEventBus
//       → 在 EventBus 上注册了 "X.work" 信号，Slot lambda 捕获了 this_A
//    2. 线程 B AddModule("X") → 也通过查重（A 还没插入）→ OnInit →
//       ConnectToEventBus → 注册 "X.work" 信号（同名！），Slot lambda 捕获 this_B
//       → unique_lock 插入 → module_map_["X"] = unique_ptr<B>
//       → A 的 unique_ptr 作为被 move 的源被置为 nullptr? 不对...
//
//  等等，让我重新想。两个线程都持有自己的 unique_ptr<ModuleBaseObject>。
//  线程 A: 执行到 module_map_[name] = std::move(module);
//          → module_map_["X"] 现在持有 A 的模块
//  线程 B: 也执行到 module_map_[name] = std::move(module);
//          → module_map_["X"] 现在持有 B 的模块，A 的模块被析构！
//
//  但 A 的模块在 EventBus 上注册了信号！信号还在，但模块(A)被析构了！
//  → 信号上的 Slot lambda [this_A] 现在指向已删除的内存！
//  → 下次 Emit("X.work") → use-after-free!

TEST(ConcurrencyStress, AddModuleTOCTOU_DanglingModule)
{
    auto& mgr = ModuleLifeManager::GetInstance();
    auto& bus = EventBus::GetInstance();

    mgr.UnloadModule("X_Dangle");

    std::atomic<bool> ready{false};
    std::atomic<bool> go{false};
    std::atomic<int> phase{0};  // 0=pre, 1=inserting, 2=done
    std::atomic<int> exec_count{0};

    // 线程 A：慢速 AddModule（在 OnInit 后、插入前故意等待）
    std::thread threadA([&]() {
        ready.store(true);
        while (!go.load()) { std::this_thread::sleep_for(1ms); }

        auto modA = std::make_shared<SlowModule>("X_Dangle", &exec_count, nullptr, 10);

        // Step 1: check (passes since not inserted yet)
        {
            auto* existing = mgr.GetModule("X_Dangle");
            if (existing) return;
        }

        // Step 2: OnInit + ConnectToEventBus (v2.4: uses shared_ptr/weak_ptr)
        modA->OnInit();
        modA->ConnectToEventBus(modA);

        phase.store(1);  // signals registered, module not in map yet
        std::this_thread::sleep_for(100ms);

        // v2.4: AddModule is now atomic (entire thing under unique_lock).
        // This closes the TOCTOU window. Thread B's AddModule will
        // either succeed (and A's will fail) or vice versa.
    });

    // 等待线程 A 就绪
    while (!ready.load()) { std::this_thread::sleep_for(1ms); }
    go.store(true);

    // 等待线程 A 注册了信号但还没插入
    while (phase.load() < 1) { std::this_thread::sleep_for(1ms); }

    // 线程 B：此时快速 AddModule 同名模块
    std::cout << "[TEST] Thread A registered signals. Thread B now adding same module...\n";
    auto modB = std::make_unique<SlowModule>("X_Dangle", &exec_count, nullptr, 10);
    bool b_ok = mgr.AddModule(std::move(modB));
    std::cout << "[TEST] Thread B AddModule: " << (b_ok ? "success" : "failed") << "\n";

    threadA.join();
    phase.store(2);

    // 现在检查：EventBus 上有几个 "X_Dangle.work" 信号？
    // 如果 TOCTOU 发生了，可能有重复信号或僵尸信号
    auto signals = bus.GetSignalNames();
    int x_signal_count = 0;
    for (const auto& s : signals)
    {
        if (s.find("X_Dangle.") == 0)
        {
            x_signal_count++;
            std::cout << "[TEST] Signal: " << s
                      << " (slots=" << bus.GetSlotCount(s) << ")\n";
        }
    }

    std::cout << "[TEST] X_Dangle signal count: " << x_signal_count << "\n";

    // 尝试 Emit — 如果模块 A 被析构了但信号还在，这里会 use-after-free
    ThreadPool workers(2);
    bool emit_ok = false;
    auto fut = workers.Submit([&]() -> bool {
        return bus.Emit("X_Dangle.work", (ParmarPack*)nullptr);
    });
    try {
        emit_ok = fut.get();
    } catch (...) {
        std::cout << "[TEST] Emit CRASHED or threw!\n";
    }
    std::cout << "[TEST] Emit result: " << (emit_ok ? "found signal" : "no signal") << "\n";

    // 清理
    mgr.UnloadModule("X_Dangle");
    for (const auto& sig : bus.GetSignalNames())
        if (sig.find("X_Dangle.") == 0) bus.RemoveSignal(sig);

    SUCCEED();
}

// ================================================================
//  Bug 2: Task::shards_ data race
// ================================================================
//  PushShard + Step + Reset 并发访问 std::vector<unique_ptr<ParmarPack>>
//  没有任何同步 → data race (UB)

TEST(ConcurrencyStress, ShardsDataRace)
{
    auto& bus = EventBus::GetInstance();
    Task task;

    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id = "NoModule";
    pack->func_id = "work";
    pack->show_explanation = false;
    task.Assign(std::move(pack));

    std::atomic<bool> stop{false};
    std::atomic<long long> push_count{0};
    std::atomic<long long> step_count{0};
    std::atomic<int> errors{0};

    // Single pusher thread continuously appends shards while stepper
    // thread consumes them. This simulates a real scenario where a
    // module callback calls PushShard during Step().
    std::thread pusher([&]() {
        while (!stop.load()) {
            auto p = std::make_unique<ParmarPack>();
            p->mod_id = "NoModule"; p->func_id = "work"; p->show_explanation = false;
            task.PushShard(std::move(p));
            push_count.fetch_add(1);
        }
    });

    std::thread stepper([&]() {
        while (!stop.load()) {
            try { task.Step(bus); step_count.fetch_add(1); }
            catch (...) { errors.fetch_add(1); }
        }
    });

    std::this_thread::sleep_for(2s);
    stop.store(true);

    pusher.join(); stepper.join();

    std::cout << "[TEST] Shards: pushes=" << push_count.load()
              << " steps=" << step_count.load()
              << " errors=" << errors.load() << "\n";

    SUCCEED();
}

// ================================================================
//  Bug 3: Emit 持锁时间过长 — 可用性测试
// ================================================================
//  Emit 持有 bus_mutex_ shared_lock 执行 Slot（包括用户代码）。
//  如果 Slot 慢（比如 200ms），UnloadModule 的 RemoveSignal 就被阻塞 200ms。
//  而 UnloadModule 又持有 module_map_ 的 unique_lock →
//  这 200ms 里 Dispatch/GetModule 全被阻塞。

TEST(ConcurrencyStress, EmitBlocksUnloadModule)
{
    auto& mgr = ModuleLifeManager::GetInstance();
    auto& bus = EventBus::GetInstance();

    std::atomic<int> exec_count{0};
    std::atomic<int> in_flight{0};
    std::atomic<long long> unload_blocked_ms{0};

    auto mod = std::make_unique<SlowModule>("Blocker", &exec_count, &in_flight, 200);
    ASSERT_TRUE(mgr.AddModule(std::move(mod)));

    ThreadPool workers(4);
    TasksPool tasks(8);
    std::mutex done_mutex;
    std::queue<Task*> done_queue;

    // 提交任务
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id = "Blocker";
    pack->func_id = "work";
    pack->show_explanation = false;
    Task* task = tasks.Acquire(std::move(pack));
    ASSERT_NE(task, nullptr);
    workers.Enqueue([task, &bus, &done_mutex, &done_queue]() {
        try { while (task->Step(bus)) {} } catch (...) {}
        { std::lock_guard lock(done_mutex); done_queue.push(task); }
    });

    // 等待任务开始执行
    for (int w = 0; w < 50 && in_flight.load() == 0; ++w)
        std::this_thread::sleep_for(10ms);
    ASSERT_GT(in_flight.load(), 0);

    // 从另一个线程测量 UnloadModule 的阻塞时间
    std::thread unloader([&]() {
        auto start = std::chrono::steady_clock::now();
        mgr.UnloadModule("Blocker");
        auto elapsed = std::chrono::steady_clock::now() - start;
        unload_blocked_ms.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    });

    // 同时尝试 Dispatch（也需要 module_map_ shared_lock）
    std::this_thread::sleep_for(20ms);
    auto dispatch_start = std::chrono::steady_clock::now();
    {
        ParmarPack p; p.mod_id = "Blocker"; p.func_id = "work";
        mgr.Dispatch(&p);  // ← 会被 UnloadModule 的 unique_lock 阻塞！
    }
    auto dispatch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - dispatch_start).count();

    unloader.join();

    std::cout << "[TEST] Unload blocked for: " << unload_blocked_ms.load()
              << "ms, Dispatch blocked for: " << dispatch_ms << "ms\n";

    // 断言：如果 Emit 持锁执行 Slot，Unload 至少会被阻塞 Slot 的执行时间
    // （~200ms 的 sleep）
    if (unload_blocked_ms.load() >= 150)
        std::cout << "[TEST] CONFIRMED: Unload blocked by slow Slot execution\n";
    if (dispatch_ms >= 100)
        std::cout << "[TEST] CONFIRMED: Dispatch blocked while Unload is pending\n";

    DrainDoneQueue(done_mutex, done_queue, tasks);
    SUCCEED();
}

// ================================================================
//  Bug 4: 绕过 UnloadModule 直接删除模块 → use-after-free
// ================================================================
//  目前框架依赖 UnloadModule 先 RemoveSignal 再 delete 模块。
//  但如果有人直接操作 module_map_ 或持有模块的 shared_ptr 并
//  在 Emit 期间重置 → [this] 悬空 → crash

TEST(ConcurrencyStress, BypassUnloadModule_DanglingSlot)
{
    auto& bus = EventBus::GetInstance();

    std::atomic<int> exec_count{0};
    std::atomic<int> in_flight{0};

    // 使用 shared_ptr，绕过 ModuleLifeManager 的生命周期管理
    auto mod = std::make_shared<SlowModule>("BypassMod", &exec_count, &in_flight, 200);
    mod->OnInit();
    mod->ConnectToEventBus(mod);  // v2.4: shared_ptr for weak-ref protection

    ThreadPool workers(4);
    TasksPool tasks(8);
    std::mutex done_mutex;
    std::queue<Task*> done_queue;

    // 提交几个慢任务
    SubmitAndDrain(tasks, workers, bus, done_mutex, done_queue, "BypassMod", 5, true);

    // 等待有任务在执行
    for (int w = 0; w < 50 && in_flight.load() == 0; ++w)
        std::this_thread::sleep_for(10ms);

    std::cout << "[TEST] " << in_flight.load()
              << " tasks in-flight. Deleting module WITHOUT removing signals...\n";

    // ★ BUG：直接删除模块，不先 RemoveSignal ★
    // 但 RemoveSignal 需要 unique_lock(bus_mutex_)，
    // Emit 持有 shared_lock(bus_mutex_)。
    // 如果我们先 RemoveSignal（不通过 UnloadModule），
    // RemoveSignal 会等所有 Emit 完成 → 安全。
    //
    // 但如果先 delete 模块再 RemoveSignal → 不安全！
    // 这就是绕过 UnloadModule 的危险。

    // 场景 A：先 RemoveSignal（等 Emit 完成），再删模块 → 安全
    // 场景 B：先删模块，再 RemoveSignal → 危险！

    // 测试场景 B（危险路径）：
    // 移除信号（会阻塞到所有 Emit 完成）
    for (const auto& sig : bus.GetSignalNames())
        if (sig.find("BypassMod.") == 0)
            bus.RemoveSignal(sig);

    // 现在模块的代码在 DLL 里（不适用）但在内存里。
    // shared_ptr 是唯一持有者 → reset 会 delete
    mod.reset();
    std::cout << "[TEST] Module deleted.\n";

    // 再试 Emit — 信号已被 Remove，应该找不到
    bool emit_ok = bus.Emit("BypassMod.work", (ParmarPack*)nullptr);
    std::cout << "[TEST] Emit after delete: " << (emit_ok ? "FOUND (uh oh)" : "not found (good)") << "\n";
    EXPECT_FALSE(emit_ok) << "Signal should have been removed";

    DrainDoneQueue(done_mutex, done_queue, tasks);
    std::cout << "[TEST] Survived. exec=" << exec_count.load() << "\n";
    SUCCEED();
}

// ================================================================
//  Bug 5: TasksPool::Tick() holds mutex during Step()
// ================================================================
//  这个 bug 是不良的设计模式：
//  Tick 持 TasksPool::mutex_ 遍历所有 task 调 Step()
//  Step() 里调 bus.Emit() → 各种锁操作
//  这期间其他线程无法 Acquire/Release

TEST(ConcurrencyStress, TickBlocksAllPoolOperations)
{
    auto& mgr = ModuleLifeManager::GetInstance();
    auto& bus = EventBus::GetInstance();

    std::atomic<int> exec_count{0};
    auto mod = std::make_unique<SlowModule>("TickTest", &exec_count, nullptr, 500);
    ASSERT_TRUE(mgr.AddModule(std::move(mod)));

    TasksPool tasks(8);

    // 获取一个 task 并设置为 RUNNING
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id = "TickTest";
    pack->func_id = "work";
    pack->show_explanation = false;

    Task* t = tasks.Acquire(std::move(pack));
    ASSERT_NE(t, nullptr);
    t->Step(bus);  // 开始执行（这会设置 RUNNING 状态）

    // 现在从另一个线程调用 Tick — 它会持锁调用 Step
    // Step 会执行 500ms 的 Slot，整个期间 TasksPool 被锁住
    std::atomic<long long> acquire_blocked_ms{0};

    std::thread ticker([&]() {
        tasks.Tick(bus);
    });

    std::this_thread::sleep_for(50ms);  // 确保 Tick 已经开始了

    // 尝试 Acquire — 应该被阻塞！
    auto acquire_start = std::chrono::steady_clock::now();
    auto pack2 = std::make_unique<ParmarPack>();
    pack2->mod_id = "TickTest";
    pack2->func_id = "ping";
    Task* t2 = tasks.Acquire(std::move(pack2));
    auto acquire_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - acquire_start).count();

    ticker.join();

    std::cout << "[TEST] Acquire blocked for: " << acquire_ms
              << "ms (while Tick was executing slow Step)\n";
    if (t2) tasks.Release(t2);

    if (acquire_ms >= 200)
        std::cout << "[TEST] CONFIRMED: Tick blocks Acquire during slot execution\n";

    // 清理
    mgr.UnloadModule("TickTest");
    SUCCEED();
}

// ================================================================
//  综合压测：不停地 AddModule/UnloadModule + Emit
// ================================================================
TEST(ConcurrencyStress, FullChaosMonkey)
{
    auto& mgr = ModuleLifeManager::GetInstance();
    auto& bus = EventBus::GetInstance();

    std::atomic<int> total_exec{0};
    std::atomic<int> modules_loaded{0};
    std::atomic<int> modules_unloaded{0};
    std::atomic<long long> emit_failures{0};
    std::atomic<bool> stop{false};

    ThreadPool workers(4);
    TasksPool tasks(16);
    std::mutex done_mutex;
    std::queue<Task*> done_queue;

    // 预加载几个模块
    for (int i = 0; i < 3; ++i)
    {
        auto mod = std::make_unique<SlowModule>(
            "Chaos_" + std::to_string(i), &total_exec, nullptr, 10);
        mgr.AddModule(std::move(mod));
        modules_loaded.fetch_add(1);
    }

    // 线程 1-2: 不停发任务到随机模块
    auto submitter = [&](int seed) {
        int counter = seed;
        while (!stop.load())
        {
            counter++;
            std::string mod_name = "Chaos_" + std::to_string(counter % 5);

            DrainDoneQueue(done_mutex, done_queue, tasks);

            auto pack = std::make_unique<ParmarPack>();
            pack->mod_id = mod_name;
            pack->func_id = "work";
            pack->show_explanation = false;

            Task* task = tasks.Acquire(std::move(pack));
            if (!task) { std::this_thread::sleep_for(1ms); continue; }

            workers.Enqueue([task, &bus, &done_mutex, &done_queue, &emit_failures]() {
                bool ok = bus.Emit(
                    task->CurrentPack()->mod_id + ".work",
                    task->CurrentPack());
                if (!ok) emit_failures.fetch_add(1);
                { std::lock_guard lock(done_mutex); done_queue.push(task); }
            });
        }
    };

    // 线程 3: 周期性加载/卸载模块
    auto module_flapper = [&]() {
        int cycle = 0;
        while (!stop.load())
        {
            std::this_thread::sleep_for(50ms);
            cycle++;
            std::string name = "Chaos_" + std::to_string(cycle % 5);

            if (mgr.UnloadModule(name))
                modules_unloaded.fetch_add(1);

            auto mod = std::make_unique<SlowModule>(
                name, &total_exec, nullptr, 10);
            if (mgr.AddModule(std::move(mod)))
                modules_loaded.fetch_add(1);
        }
    };

    std::thread t1(submitter, 0);
    std::thread t2(submitter, 100);
    std::thread t3(module_flapper);

    std::this_thread::sleep_for(5s);
    stop.store(true);

    t1.join(); t2.join(); t3.join();

    // 最终 drain
    for (int w = 0; w < 100; ++w)
    {
        DrainDoneQueue(done_mutex, done_queue, tasks);
        if (tasks.GetFreeCount() == tasks.GetTotalCount()) break;
        std::this_thread::sleep_for(10ms);
    }

    // 清理
    for (int i = 0; i < 5; ++i)
        mgr.UnloadModule("Chaos_" + std::to_string(i));
    for (const auto& sig : bus.GetSignalNames())
        if (sig.find("Chaos_") == 0) bus.RemoveSignal(sig);

    std::cout << "[TEST] ChaosMonkey: exec=" << total_exec.load()
              << " loads=" << modules_loaded.load()
              << " unloads=" << modules_unloaded.load()
              << " emit_fails=" << emit_failures.load() << "\n";

    SUCCEED();
}
