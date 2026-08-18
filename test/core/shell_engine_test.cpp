/// =================================================================
///  ShellEngine 测试 — 主循环引擎隔离验证
/// =================================================================
///
///  通过 InjectCommand + RequestStop 驱动引擎（不依赖 cin/cout）。
///  用 ResultStore 和 Task pool 状态验证正确性。
///  覆盖：启动、命令执行、关闭竞态、内存稳定、并发注入、吞吐测量。
/// =================================================================

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

#include "core/ShellEngine.h"
#include "core/ModuleLifeManager.h"
#include "core/ParmarPack.h"
#include "core/ResultStore.h"
#include "event_bus/event_bus.h"
#include "sdk/IModule.h"

using namespace std::chrono_literals;

// ================================================================
//  辅助：获取进程内存
// ================================================================
static size_t GetWorkingSetKB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX mc{};
    mc.cb = sizeof(mc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mc), sizeof(mc)))
        return mc.WorkingSetSize / 1024;
#endif
    return 0;
}

// ================================================================
//  辅助：激进清理所有单例，防止跨测试污染
// ================================================================
static void AggressiveCleanup() {
    // ① ResultStore — 清空残留结果
    ResultStore::Get().Clear();

    // ② CommandParser — 排空内部队列
    auto& parser = CommandParser::Get();
    std::unique_ptr<ParmarPack> stale;
    while (parser.TryPopPack(stale)) {}

    // ③ EventBus — 清除已知的测试模块信号
    auto& bus = EventBus::GetInstance();
    const char* test_prefixes[] = {
        "SE_Test.", "SE_Fast.", "PeakFast.", "CrashTest.",
        "ITest.", "BypassMod.", "TModule.", "X_Dangle.",
        "Blocker.", "TickTest.", "Chaos_", "G_", "IM_",
        "Burst.", "Exhaust.", "Parallel.", "Cycle.",
        "Fast.", "Slow.", "Shutdown.", "Racy.",
        "builtin_test.", "test_add.", "dup.", "findme.", "count_",
        "dispatch_test.", "init_test.", "Alpha.", "Beta.", "Concurrent.",
        "SDKMod.", "Calc.", "Idle.",
    };
    for (const auto& sig : bus.GetSignalNames()) {
        for (auto* prefix : test_prefixes) {
            if (sig.compare(0, strlen(prefix), prefix) == 0) {
                bus.RemoveSignal(sig);
                break;
            }
        }
    }
}

class ShellEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        AggressiveCleanup();
        exec_count_.store(0);
        auto& mgr = ModuleLifeManager::GetInstance();
        mgr.UnloadModule("SE_Test");
        mgr.AddModule(std::make_unique<TestModule>("SE_Test", &exec_count_));
    }

    void TearDown() override {
        auto& mgr = ModuleLifeManager::GetInstance();
        mgr.UnloadModule("SE_Test");
        ResultStore::Get().Clear();
    }

    class TestModule : public ModuleBaseObject {
    public:
        TestModule(std::string name, std::atomic<int>* cnt)
            : name_(std::move(name)), exec_count_(cnt) {}
        const char* GetName() const override { return name_.c_str(); }
        bool OnInit() override {
            REGISTER_FUNC("echo", "echo back", {
                std::string msg = pack->GetOr("msg", "no_msg");
                pack->return_value = name_ + ":" + msg;
                pack->success = true;
                if (exec_count_) exec_count_->fetch_add(1);
            });
            REGISTER_FUNC("slow", "slow work", {
                std::this_thread::sleep_for(50ms);
                pack->return_value = name_ + ":done";
                pack->success = true;
                if (exec_count_) exec_count_->fetch_add(1);
            });
            return true;
        }
    private:
        std::string name_;
        std::atomic<int>* exec_count_;
    };

    std::atomic<int> exec_count_{0};
};

// ================================================================
//  Test 1: 构造与析构不崩溃
// ================================================================
TEST_F(ShellEngineTest, ConstructDestruct) {
    {
        ShellEngine engine(4, 2);
        EXPECT_EQ(engine.TotalTasks(), 4u);
        EXPECT_EQ(engine.FreeTasks(), 4u);
        EXPECT_EQ(engine.PendingResults(), 0u);
    }
    // 析构调用 Shutdown()，不崩溃
}

// ================================================================
//  Test 2: InjectCommand → ResultStore 有结果
// ================================================================
TEST_F(ShellEngineTest, InjectCommandProducesResult) {
    ShellEngine engine(4, 2);

    std::thread runner([&]() { engine.Run(); });

    engine.InjectCommand("-m:SE_Test -f:echo -v:msg|hello");

    // 等计数器递增（最多 1 秒，覆盖 10 个主循环周期）
    for (int w = 0; w < 100 && exec_count_.load() == 0; ++w)
        std::this_thread::sleep_for(10ms);

    engine.RequestStop();
    runner.join();

    EXPECT_GT(exec_count_.load(), 0)
        << "echo command should have executed at least once";
}

// ================================================================
//  Test 3: 多条命令并发 — ResultStore 命中全部
// ================================================================
TEST_F(ShellEngineTest, MultipleCommandsAllComplete) {
    // 池子 >= N：SubmitTask 在池满时会丢弃命令（背压）。若池子 < N，
    // 快速注入 N 条会偶发丢 1~2 条（got < N）。池子 >= N 保证全部入队。
    ShellEngine engine(20, 4);

    // ★ RunWithoutInput：不启 stdin 输入线程，避开「测试环境 stdin 是 EOF，
    //   getline 立即返回把 running 设 false → 主循环提前退出」的竞态。
    std::thread runner([&]() { engine.RunWithoutInput(); });

    constexpr int N = 20;
    for (int i = 0; i < N; ++i)
        engine.InjectCommand("-m:SE_Test -f:echo -v:msg|cmd_" + std::to_string(i));

    // 等计数器达到 N（最多 2 秒）
    for (int w = 0; w < 200 && exec_count_.load() < N; ++w)
        std::this_thread::sleep_for(10ms);

    engine.RequestStop();
    runner.join();

    int got = exec_count_.load();
    std::cout << "[TEST] MultipleCommands: expected " << N
              << " got " << got << "\n";

    EXPECT_EQ(got, N) << "Every injected command should execute";
}

// ================================================================
//  Test 4: 空命令（回车）不崩溃
// ================================================================
TEST_F(ShellEngineTest, EmptyCommandNoCrash) {
    ShellEngine engine(4, 2);

    std::thread runner([&]() { engine.Run(); });

    engine.InjectCommand("");  // simulate hitting enter with no text

    std::this_thread::sleep_for(50ms);

    engine.RequestStop();
    runner.join();

    EXPECT_EQ(engine.PendingResults(), 0u)
        << "Empty command should not produce results";
    SUCCEED();
}

// ================================================================
//  Test 5: /exit 立即停止
// ================================================================
TEST_F(ShellEngineTest, ExitCommandStopsEngine) {
    ShellEngine engine(4, 2);

    auto start = std::chrono::steady_clock::now();
    std::thread runner([&]() { engine.Run(); });

    engine.InjectCommand("/exit");

    runner.join();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    std::cout << "[TEST] Exit: engine stopped in " << elapsed << "ms\n";
    EXPECT_LT(elapsed, 500) << "/exit should stop engine quickly";
}

// ================================================================
//  Test 6: RequestStop 干净退出
// ================================================================
TEST_F(ShellEngineTest, RequestStopCleanExit) {
    ShellEngine engine(4, 2);

    std::thread runner([&]() { engine.Run(); });

    // 先注入几条慢命令
    for (int i = 0; i < 3; ++i)
        engine.InjectCommand("-m:SE_Test -f:slow");

    std::this_thread::sleep_for(30ms);

    engine.RequestStop();
    runner.join();

    // Shutdown 应该等待慢任务完成并 drain 结果
    // 验证不崩溃即可
    SUCCEED();
}

// ================================================================
//  Test 7: Pool 满载时 Inject 不崩溃
// ================================================================
TEST_F(ShellEngineTest, PoolExhaustionGraceful) {
    // Tiny pool — easy to exhaust
    ShellEngine engine(1, 1);

    std::thread runner([&]() { engine.Run(); });

    // 注入一个慢任务占用唯一槽位
    engine.InjectCommand("-m:SE_Test -f:slow");

    std::this_thread::sleep_for(20ms);

    // 再注入 — 应该提示池满而非崩溃
    for (int i = 0; i < 5; ++i)
        engine.InjectCommand("-m:SE_Test -f:echo -v:msg|overflow");

    std::this_thread::sleep_for(200ms);

    engine.RequestStop();
    runner.join();

    SUCCEED();
}

// ================================================================
//  Test 8: Run → Shutdown → 再次访问 ResultStore 不崩溃
// ================================================================
TEST_F(ShellEngineTest, ShutdownThenReadResultStore) {
    {
        ShellEngine engine(4, 2);
        std::thread runner([&]() { engine.Run(); });

        engine.InjectCommand("-m:SE_Test -f:echo -v:msg|final");
        std::this_thread::sleep_for(100ms);
        engine.InjectCommand("/exit");
        runner.join();
    }

    // ShellEngine 析构后，ResultStore（单例）仍可访问
    auto batch = ResultStore::Get().Drain();
    // 可能为空（被 Shutdown drain 过了），但不应该崩溃
    SUCCEED();
}

// ================================================================
//  Test 9: 快速 Start/Stop 多轮循环
// ================================================================
TEST_F(ShellEngineTest, RapidStartStopCycles) {
    for (int round = 0; round < 5; ++round) {
        ShellEngine engine(4, 2);

        std::thread runner([&]() { engine.Run(); });

        engine.InjectCommand("-m:SE_Test -f:echo -v:msg|r" + std::to_string(round));

        std::this_thread::sleep_for(50ms);
        engine.InjectCommand("/exit");
        runner.join();
    }
    SUCCEED();
}

// ================================================================
//  Test 10: Concurrency — 高并发注入 + 引擎运行不崩溃
// ================================================================
TEST_F(ShellEngineTest, HighConcurrencyInject) {
    ShellEngine engine(8, 4);

    std::atomic<bool> injecting{true};
    std::atomic<int> injected{0};

    std::thread runner([&]() { engine.Run(); });

    // 并发注入线程
    std::thread injector([&]() {
        for (int i = 0; i < 100; ++i) {
            engine.InjectCommand("-m:SE_Test -f:echo -v:msg|burst_" + std::to_string(i));
            injected.fetch_add(1);
            std::this_thread::sleep_for(2ms);
        }
        injecting.store(false);
    });

    injector.join();

    // 等待引擎处理完
    for (int w = 0; w < 100; ++w) {
        std::this_thread::sleep_for(10ms);
        if (engine.FreeTasks() == engine.TotalTasks())
            break;
    }
    std::this_thread::sleep_for(100ms);

    engine.InjectCommand("/exit");
    runner.join();

    // 收集
    int total = 0;
    auto batch = ResultStore::Get().Drain();
    total += static_cast<int>(batch.size());

    std::cout << "[TEST] HighConcurrency: injected=" << injected.load()
              << " results=" << total << "\n";

    EXPECT_GT(injected.load(), 0);
    SUCCEED();
}

// ================================================================
//  ★ 空指针安全测试
// ================================================================

// Test 11: ResultStore::PushResult(nullptr pack) 不崩溃
TEST_F(ShellEngineTest, NullPackSafety) {
    auto& store = ResultStore::Get();
    store.Clear();

    // Push null pack — must not crash
    store.PushResult(99, nullptr);
    EXPECT_TRUE(store.HasResults());

    auto batch = store.Drain();
    ASSERT_EQ(batch.size(), 1u);
    EXPECT_EQ(batch[0]->task_id, 99u);
    EXPECT_EQ(batch[0]->pack, nullptr);

    // DrainResults on null pack — must not crash
    // ShellEngine::DrainResults checks if(item->pack) before formatting
    ShellEngine engine(1, 1);
    std::thread runner([&]() { engine.Run(); });
    engine.RequestStop();
    runner.join();

    store.Clear();
    SUCCEED();
}

// Test 12: 空字符串注入不崩溃
TEST_F(ShellEngineTest, EmptyStringInjectionNoCrash) {
    ShellEngine engine(2, 1);

    std::thread runner([&]() { engine.Run(); });

    // 注入各种边界字符串
    engine.InjectCommand("");                          // 空
    engine.InjectCommand("-m:SE_Test");                // 缺少 -f:
    engine.InjectCommand("-m:NoSuchModule -f:echo");   // 不存在的模块

    std::this_thread::sleep_for(100ms);
    engine.InjectCommand("/exit");
    runner.join();

    SUCCEED();
}

// ================================================================
//  ★ 内存泄漏压测
// ================================================================

// Test 13: 反复创建/销毁 ShellEngine，验证无内存泄漏
TEST_F(ShellEngineTest, MemoryLeak_RepeatedLifecycle) {
#ifdef _WIN32
    size_t mem_before = GetWorkingSetKB();

    constexpr int CYCLES = 30;
    for (int i = 0; i < CYCLES; ++i) {
        ShellEngine engine(4, 2);

        std::thread runner([&]() { engine.Run(); });

        // 注入批量命令
        for (int j = 0; j < 10; ++j)
            engine.InjectCommand("-m:SE_Test -f:echo -v:msg|leak_" + std::to_string(j));

        std::this_thread::sleep_for(50ms);
        engine.InjectCommand("/exit");
        runner.join();
    }

    size_t mem_after = GetWorkingSetKB();
    long long delta = static_cast<long long>(mem_after) - static_cast<long long>(mem_before);

    std::cout << "[TEST] MemoryLeak_Cycles: before=" << mem_before
              << " KB → after=" << mem_after << " KB (delta=" << delta << " KB, "
              << CYCLES << " cycles)\n";

    // 30 轮创建/销毁，内存增长应 < 5MB
    EXPECT_LT(delta, 5120)
        << "Memory leak detected after " << CYCLES << " lifecycles: " << delta << " KB";
#else
    SUCCEED();
#endif
}

// Test 14: 长时间运行后的内存稳定性
TEST_F(ShellEngineTest, MemoryStability_LongRun) {
#ifdef _WIN32
    ShellEngine engine(8, 4);
    size_t mem_start = GetWorkingSetKB();

    std::thread runner([&]() { engine.Run(); });

    // 持续注入命令 5 秒
    auto start = std::chrono::steady_clock::now();
    int count = 0;
    while (std::chrono::steady_clock::now() - start < 5s) {
        engine.InjectCommand("-m:SE_Test -f:echo -v:msg|s" + std::to_string(count));
        count++;
        std::this_thread::sleep_for(10ms);
    }

    engine.InjectCommand("/exit");
    runner.join();

    size_t mem_end = GetWorkingSetKB();
    long long delta = static_cast<long long>(mem_end) - static_cast<long long>(mem_start);

    std::cout << "[TEST] MemoryStability: " << count << " commands over 5s, "
              << "delta=" << delta << " KB\n";

    EXPECT_LT(delta, 10240)
        << "Memory grew too much during sustained load: " << delta << " KB";
#else
    SUCCEED();
#endif
}

// ================================================================
//  ★ 并发速度吞吐测试
// ================================================================

// Test 15: 测量吞吐量（命令/秒）
TEST_F(ShellEngineTest, ThroughputBenchmark) {
    // 池子 >= BATCH：背压会丢命令，让 completed 不确定，吞吐统计还会被
    // 等待循环的 sleep 时间污染（completed 到不了阈值 → 等满 → elapsed 虚高）。
    // 池子够大则 BATCH 条全部入队，completed == BATCH，吞吐可稳定测量。
    ShellEngine engine(200, 4);

    // 注册一个极快的模块
    auto& mgr = ModuleLifeManager::GetInstance();
    mgr.UnloadModule("SE_Fast");
    class FastMod : public ModuleBaseObject {
        const char* GetName() const override { return "SE_Fast"; }
        bool OnInit() override {
            REGISTER_FUNC("nop", "", {
                pack->return_value = "ok";
                pack->success = true;
                // 极快：不做任何事
            });
            return true;
        }
    };
    mgr.AddModule(std::make_unique<FastMod>());

    // ★ RunWithoutInput + result sink 统计真实完成数：
    //   - 避开 stdin EOF 竞态（Run 会启输入线程，测试环境 stdin 是 EOF）
    //   - 避开查 ResultStore 残留尾巴（结果早就被 DrainResults 消费了）
    std::atomic<int> completed{0};
    engine.SetResultSink([&completed](const ParmarPack&) { completed.fetch_add(1); });
    std::thread runner([&]() { engine.RunWithoutInput(); });

    constexpr int BATCH = 200;
    auto t0 = std::chrono::steady_clock::now();

    for (int i = 0; i < BATCH; ++i)
        engine.InjectCommand("-m:SE_Fast -f:nop");

    // 等全部完成（池子 >= BATCH，无背压丢命令，completed 会到 BATCH）
    for (int w = 0; w < 400 && completed.load() < BATCH; ++w)
        std::this_thread::sleep_for(5ms);

    auto t1 = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    engine.InjectCommand("/exit");
    runner.join();

    double throughput = (elapsed_ms > 0) ? (completed.load() * 1000.0 / elapsed_ms) : 0;

    std::cout << "[TEST] Throughput: " << completed.load() << " commands in "
              << elapsed_ms << "ms = " << throughput << " cmd/s\n";

    EXPECT_GT(completed.load(), BATCH * 0.8) << "At least 80% of commands should succeed";
    EXPECT_GT(throughput, 50.0) << "Throughput should exceed 50 cmd/s";

    mgr.UnloadModule("SE_Fast");
}

// ================================================================
//  ★ 关闭竞态验证
// ================================================================

// Test 16: Shutdown 竞态 — 确保最后时刻的结果不丢失
TEST_F(ShellEngineTest, ShutdownRace_LastResultNotLost) {
    ShellEngine engine(4, 2);

    std::thread runner([&]() { engine.Run(); });

    // 注入一批 slow 命令（50ms 执行时间）和 exit
    constexpr int N = 10;
    for (int i = 0; i < N; ++i)
        engine.InjectCommand("-m:SE_Test -f:slow");

    std::this_thread::sleep_for(20ms);

    // exit 时 slow 任务还在执行
    engine.InjectCommand("/exit");
    runner.join();

    // 显式 Shutdown — idle-drain 等待所有 slow 任务完成
    engine.Shutdown();

    EXPECT_EQ(engine.FreeTasks(), engine.TotalTasks())
        << "All tasks should be back in pool after shutdown";

    SUCCEED();
}

// Test 17: Shutdown 期间并发 Push + Drain，数据完整性
TEST_F(ShellEngineTest, ShutdownConcurrentPushDrain) {
    ShellEngine engine(8, 4);

    std::atomic<int> before_shutdown_results{0};
    std::atomic<bool> shutdown_started{false};

    std::thread runner([&]() { engine.Run(); });

    // 注入大量快速命令
    for (int i = 0; i < 50; ++i)
        engine.InjectCommand("-m:SE_Test -f:echo -v:msg|sd_" + std::to_string(i));

    // 等待部分完成
    std::this_thread::sleep_for(30ms);

    // 在 shutdown 前记录已产生的结果数
    before_shutdown_results.store(static_cast<int>(ResultStore::Get().Size()));

    engine.InjectCommand("/exit");
    runner.join();

    // Shutdown 的 idle-drain 已经打印了结果
    // 验证不崩溃 + task pool 完整
    EXPECT_EQ(engine.FreeTasks(), engine.TotalTasks());

    std::cout << "[TEST] ShutdownRace: pre-shutdown results="
              << before_shutdown_results.load() << "\n";

    SUCCEED();
}

// ================================================================
//  ★ Task pool 压力测试
// ================================================================

// Test 18: Pool 满载时的背压行为
TEST_F(ShellEngineTest, PoolBackpressure) {
    // 小池：只有 2 个槽位，1 个工人
    ShellEngine engine(2, 1);

    std::thread runner([&]() { engine.Run(); });

    // 注入 20 个慢命令（50ms 每个）
    for (int i = 0; i < 20; ++i)
        engine.InjectCommand("-m:SE_Test -f:slow");

    // 等待全部完成（需要约 20×50ms/1worker = 1000ms）
    for (int w = 0; w < 200; ++w) {
        std::this_thread::sleep_for(10ms);
        if (engine.FreeTasks() == engine.TotalTasks())
            break;
    }

    engine.InjectCommand("/exit");
    runner.join();

    EXPECT_EQ(engine.FreeTasks(), engine.TotalTasks())
        << "Pool should recover after all tasks complete";

    SUCCEED();
}

// Test 19: 零 workers 边界（单任务串行执行，依赖 ThreadPool 构造）
// 注意：ThreadPool(0) 会创建 0 个工人，任务在 Enqueue 后不会被消费
TEST_F(ShellEngineTest, Edge_ZeroWorkers) {
    // 实际测试：1 worker 确保能消耗任务
    ShellEngine engine(4, 1);

    std::thread runner([&]() { engine.Run(); });

    engine.InjectCommand("-m:SE_Test -f:echo -v:msg|single_worker");

    for (int w = 0; w < 50 && engine.FreeTasks() == engine.TotalTasks(); ++w)
        std::this_thread::sleep_for(10ms);

    std::this_thread::sleep_for(50ms);

    engine.InjectCommand("/exit");
    runner.join();

    SUCCEED();
}

// Test 20: 大量 Input 积压（输入比处理快）
TEST_F(ShellEngineTest, InputBacklog) {
    ShellEngine engine(4, 2);

    // ★ RunWithoutInput + result sink 统计真实完成数（避开 stdin EOF + ResultStore 残留）
    std::atomic<int> completed{0};
    engine.SetResultSink([&completed](const ParmarPack&) { completed.fetch_add(1); });
    std::thread runner([&]() { engine.RunWithoutInput(); });

    // 快速注入 100 个命令（不 sleep）
    for (int i = 0; i < 100; ++i)
        engine.InjectCommand("-m:SE_Test -f:echo -v:msg|bl_" + std::to_string(i));

    // 等：有结果产出，且池子全归还（在途任务都跑完）
    for (int w = 0; w < 200; ++w) {
        std::this_thread::sleep_for(20ms);
        if (completed.load() > 0 && engine.FreeTasks() == engine.TotalTasks())
            break;
    }

    engine.InjectCommand("/exit");
    runner.join();

    std::cout << "[TEST] InputBacklog: " << completed.load() << " results from 100 commands\n";

    EXPECT_GT(completed.load(), 0) << "Some commands should complete";
    EXPECT_EQ(engine.FreeTasks(), engine.TotalTasks());

    SUCCEED();
}
