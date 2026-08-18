/// =================================================================
///  PeakStress 测试 — 峰值吞吐和极限压力
/// =================================================================
///
///  测试维度：
///   [RampUp]         全速注入，测量峰值吞吐
///   [Burst]          瞬间超容量爆发
///   [Sustained]      10 秒持续负载，验证无内存泄漏
///   [PoolUtil]       Pool 槽位占用和归还
///   [Recovery]       过载后恢复时间
///   [SlotException]  catch(...) 兜底防止进程崩溃
///
///  所有测试使用极快模块（执行 < 1μs），确保瓶颈是框架本身而非业务逻辑。
/// =================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

#include "core/ModuleLifeManager.h"
#include "core/ParmarPack.h"
#include "core/ResultStore.h"
#include "core/ShellEngine.h"
#include "core/Task.h"
#include "core/TasksPool.h"
#include "core/ThreadPool.h"
#include "event_bus/event_bus.h"
#include "sdk/IModule.h"

using namespace std::chrono_literals;

// v2.7: 压力测试线程数 = CPU 总核心 - 2，避免跑满所有核心影响 CI 稳定性
static int StressWorkers() {
    unsigned hw = std::thread::hardware_concurrency();
    return (hw > 2) ? static_cast<int>(hw) - 2 : 1;
}

// ================================================================
//  辅助工具
// ================================================================

static size_t GetWorkingSetKB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX mc{}; mc.cb = sizeof(mc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mc), sizeof(mc)))
        return mc.WorkingSetSize / 1024;
#endif
    return 0;
}

/// 注册极快模块（< 1μs）
static void RegisterFastModule(const std::string& name = "PeakFast") {
    auto& mgr = ModuleLifeManager::GetInstance();
    mgr.UnloadModule(name);
    class FastMod : public ModuleBaseObject {
        std::string n_;
    public:
        explicit FastMod(std::string n) : n_(std::move(n)) {}
        const char* GetName() const override { return n_.c_str(); }
        bool OnInit() override {
            REGISTER_FUNC("nop", "", {
                pack->return_value = n_ + "_ok";
                pack->success = true;
            });
            return true;
        }
    };
    mgr.AddModule(std::make_unique<FastMod>(name));
}

/// 注册慢模块（每个命令 sleep 200ms）—— 用于观察池子占用。
/// 快命令 <1μs 就处理完，50ms 观察窗口内池子早就归还了（used=0），
/// 测不到"占用"状态。慢命令保证观察时 Worker 仍在处理。
static void RegisterSlowModule(const std::string& name = "PeakSlow") {
    auto& mgr = ModuleLifeManager::GetInstance();
    mgr.UnloadModule(name);
    class SlowMod : public ModuleBaseObject {
        std::string n_;
    public:
        explicit SlowMod(std::string n) : n_(std::move(n)) {}
        const char* GetName() const override { return n_.c_str(); }
        bool OnInit() override {
            REGISTER_FUNC("slow", "", {
                std::this_thread::sleep_for(200ms);
                pack->return_value = n_ + "_slow_ok";
                pack->success = true;
            });
            return true;
        }
    };
    mgr.AddModule(std::make_unique<SlowMod>(name));
}

/// 帮助: 启动 engine, 持续注入直到 done, 收集统计
struct RunStats {
    int total_submitted = 0;
    int total_completed = 0;
    int pool_busy_errors = 0;
    double elapsed_ms  = 0;
    double throughput   = 0;   // cmds/sec
    size_t mem_delta_kb = 0;

    void Print(const char* label) const {
        std::cout << "[" << label << "] submitted=" << total_submitted
                  << " completed=" << total_completed
                  << " rejected=" << pool_busy_errors
                  << " time=" << elapsed_ms << "ms"
                  << " throughput=" << throughput << " cmd/s"
                  << " mem_delta=" << mem_delta_kb << "KB" << std::endl;
    }
};

static void CleanupModule(const std::string& name = "PeakFast") {
    ModuleLifeManager::GetInstance().UnloadModule(name);
    ResultStore::Get().Clear();
}

/// 激进清理 — 排空所有单例，防跨测试污染
static void AggressiveCleanup() {
    ResultStore::Get().Clear();
    auto& parser = CommandParser::Get();
    std::unique_ptr<ParmarPack> stale;
    while (parser.TryPopPack(stale)) {}
}

class PeakStressTest : public ::testing::Test {
protected:
    void SetUp() override    { AggressiveCleanup(); }
    void TearDown() override { AggressiveCleanup(); }
};

// ================================================================
//  Test 1: RampUp — 快速注入，测量实际 ResultStore 产出的峰值吞吐
// ================================================================
TEST_F(PeakStressTest, RampUpThroughput) {
    RegisterFastModule("PeakFast");

    const int W = StressWorkers();
    constexpr int N = 5000;  // 注入固定量，全速注入测峰值处理吞吐
    ShellEngine engine(W, W);
    // ★ RunWithoutInput + result sink 统计真实完成数（避开 stdin EOF + 残留尾巴）
    std::atomic<int> completed{0};
    engine.SetResultSink([&completed](const ParmarPack&) { completed.fetch_add(1); });
    std::thread runner([&]() { engine.RunWithoutInput(); });

    auto t0 = std::chrono::steady_clock::now();

    // 全速注入固定量。有限量 → 注入完 input_queue 会清空，主循环回到
    // DrainResults（无限注入会饿死 DrainResults，sink 永远收不到结果）。
    std::atomic<int> injected{0};
    std::thread injector([&]() {
        for (int i = 0; i < N; ++i) {
            engine.InjectCommand("-m:PeakFast -f:nop");
            injected.fetch_add(1);
        }
    });
    injector.join();

    // 等所有命令完成并被 Drain（sink 计数接近 N）。
    // 用 completed 而非 FreeTasks 作完成信号：FreeTasks 在 MainLoop 启动延迟时
    // 会误判"池子空=完成"，导致提前 /exit、结果没被 Drain（flaky）。
    // completed 是"真实完成且已 Drain"的可靠信号，单调递增不会误判。
    for (int w = 0; w < 1000 && completed.load() < N * 9 / 10; ++w) {
        std::this_thread::sleep_for(10ms);
    }
    auto t1 = std::chrono::steady_clock::now();

    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    double throughput = total_ms > 0 ? (completed.load() * 1000.0 / total_ms) : 0;

    engine.InjectCommand("/exit");
    runner.join();

    std::cout << "[RampUp] injected=" << injected.load()
              << " completed=" << completed.load()
              << " time=" << total_ms << "ms"
              << " throughput=" << throughput << " cmd/s" << std::endl;

    EXPECT_GT(completed.load(), N / 2) << "Should complete most injected commands";
    EXPECT_GT(throughput, 10.0) << "Minimum throughput should exceed 10 cmd/s";

    CleanupModule("PeakFast");
}

// ================================================================
//  Test 2: Burst — 瞬间超容量爆发
// ================================================================
TEST_F(PeakStressTest, BurstOverload) {
    RegisterFastModule("PeakFast");

    const int W = StressWorkers();
    constexpr int BURST = 500;  // 500 条命令瞬间注入

    ShellEngine engine(W, W);
    // ★ RunWithoutInput + result sink 统计真实完成数
    std::atomic<int> completed{0};
    engine.SetResultSink([&completed](const ParmarPack&) { completed.fetch_add(1); });
    std::thread runner([&]() { engine.RunWithoutInput(); });

    int accepted = 0, rejected = 0;
    auto t0 = std::chrono::steady_clock::now();

    // 瞬间注入（不 sleep）
    for (int i = 0; i < BURST; ++i) {
        // 通过检查 FreeTasks 估算是否会被拒绝
        if (engine.FreeTasks() > 0)
            accepted++;
        else
            rejected++;
        engine.InjectCommand("-m:PeakFast -f:nop");
    }

    // 等待全部处理完
    for (int w = 0; w < 200; ++w) {
        if (engine.FreeTasks() == engine.TotalTasks()) break;
        std::this_thread::sleep_for(10ms);
    }
    std::this_thread::sleep_for(200ms);

    auto t1 = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    engine.InjectCommand("/exit");
    runner.join();

    double throughput = elapsed_ms > 0 ? (completed.load() * 1000.0 / elapsed_ms) : 0;

    std::cout << "[Burst] " << BURST << " cmds injected, "
              << completed.load() << " completed, "
              << "elapsed=" << elapsed_ms << "ms, "
              << "throughput=" << throughput << " cmd/s" << std::endl;

    // 核心断言: 不能崩溃
    EXPECT_GT(completed.load(), 0) << "Some commands should complete";
    EXPECT_EQ(engine.FreeTasks(), engine.TotalTasks())
        << "Pool should fully recover after burst";

    CleanupModule("PeakFast");
}

// ================================================================
//  Test 3: Sustained — 30 秒持续负载（80% 容量）
// ================================================================
TEST_F(PeakStressTest, SustainedLoad) {
    RegisterFastModule("PeakFast");

    const int W = StressWorkers();
    constexpr int DURATION_SEC = 10;  // 10 秒足够发现趋势
    constexpr int RATE_PER_SEC = 200; // 目标注入速率

    ShellEngine engine(W, W);
    size_t mem_start = GetWorkingSetKB();

    // ★ 用 RunWithoutInput()：不启动 stdin 输入线程。测试环境 stdin 是 EOF，
    //   getline 会立即返回把 running 设 false，导致主循环提前退出，延迟注入
    //   的命令永远不被处理。RunWithoutInput 只跑主循环，靠 /exit 停。
    // ★ 用 result sink 统计真实完成数：ResultStore 是"中转站"，结果一进去
    //   就被主循环 DrainResults 消费掉，停引擎时 Drain 拿到的只是残留尾巴，
    //   不是真实完成数（违反"别查 ResultStore 用原子计数器"的原则）。
    std::atomic<int> completed{0};
    engine.SetResultSink([&completed](const ParmarPack&) {
        completed.fetch_add(1);
    });

    std::thread runner([&]() { engine.RunWithoutInput(); });

    std::atomic<int> injected{0};
    std::atomic<int> rejected{0};
    std::atomic<bool> done{false};

    auto t0 = std::chrono::steady_clock::now();

    // 注入线程: 持续注入 10 秒
    std::thread injector([&]() {
        while (!done.load()) {
            int free = static_cast<int>(engine.FreeTasks());
            if (free > 0) {
                engine.InjectCommand("-m:PeakFast -f:nop");
                injected.fetch_add(1);
            } else {
                rejected.fetch_add(1);
                engine.InjectCommand("-m:PeakFast -f:nop");  // 仍注入，让引擎自己拒绝
            }
            // 控制速率
            std::this_thread::sleep_for(std::chrono::microseconds(1000000 / RATE_PER_SEC));
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(DURATION_SEC));
    done.store(true);
    injector.join();

    // 等待追赶
    for (int w = 0; w < 100; ++w) {
        if (engine.FreeTasks() == engine.TotalTasks()) break;
        std::this_thread::sleep_for(20ms);
    }

    auto t1 = std::chrono::steady_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    size_t mem_end = GetWorkingSetKB();

    // 停引擎（Shutdown 内有 idle-drain）
    engine.InjectCommand("/exit");
    runner.join();

    double throughput = total_ms > 0 ? (completed.load() * 1000.0 / total_ms) : 0;
    long long mem_delta = static_cast<long long>(mem_end) - static_cast<long long>(mem_start);

    std::cout << "[Sustained] " << DURATION_SEC << "s: "
              << "injected=" << injected.load()
              << " completed=" << completed.load()
              << " rejected=" << rejected.load()
              << " throughput=" << throughput << " cmd/s"
              << " mem_delta=" << mem_delta << "KB" << std::endl;

    // 持续负载（80% 容量场景）下，注入的命令绝大部分应被处理。
    // 若主循环跑不起来（如误用 Run() 撞上 stdin EOF），completed 会骤降到
    // 0~1，这里能立刻抓住。
    EXPECT_GT(completed.load(), injected.load() / 2);
    EXPECT_EQ(engine.FreeTasks(), engine.TotalTasks());
    // 10 秒持续负载，内存增长 < 50MB（含 OS 缓存波动）
    EXPECT_LT(mem_delta, 51200) << "Sustained load memory leak: " << mem_delta << "KB";

    CleanupModule("PeakFast");
}

// ================================================================
//  Test 4: 并发利用率 — 多条命令并发，验证 Pool 全利用
// ================================================================
TEST_F(PeakStressTest, PoolUtilization) {
    RegisterSlowModule("PeakSlow");

    const int W = StressWorkers();
    ShellEngine engine(W, W);
    std::thread runner([&]() { engine.RunWithoutInput(); });

    // 注入正好填满池子的慢命令（每个 200ms，保证观察时仍在处理）
    for (int i = 0; i < W; ++i)
        engine.InjectCommand("-m:PeakSlow -f:slow");

    // 等主循环把命令提交到池子（Worker 开始处理）
    std::this_thread::sleep_for(50ms);
    size_t used = engine.TotalTasks() - engine.FreeTasks();
    std::cout << "[PoolUtil] Pool utilization: " << used << "/" << engine.TotalTasks() << std::endl;

    // 慢命令 50ms 时还没处理完，池子应被占用
    EXPECT_GT(used, 0) << "Pool should be utilized while slow tasks run";

    // 等全部完成
    for (int w = 0; w < 100; ++w) {
        if (engine.FreeTasks() == engine.TotalTasks()) break;
        std::this_thread::sleep_for(20ms);
    }

    engine.InjectCommand("/exit");
    runner.join();

    EXPECT_EQ(engine.FreeTasks(), engine.TotalTasks())
        << "All tasks should return to pool";

    CleanupModule("PeakSlow");
}

// ================================================================
//  Test 5: Recovery — 超载后恢复
// ================================================================
TEST_F(PeakStressTest, RecoveryAfterOverload) {
    RegisterFastModule("PeakFast");

    const int W = StressWorkers();

    ShellEngine engine(W, W);
    std::thread runner([&]() { engine.RunWithoutInput(); });

    // Phase 1: 正常负载（基线）
    for (int i = 0; i < 50; ++i) {
        engine.InjectCommand("-m:PeakFast -f:nop");
        std::this_thread::sleep_for(5ms);
    }
    for (int w = 0; w < 50 && engine.FreeTasks() < engine.TotalTasks(); ++w)
        std::this_thread::sleep_for(10ms);
    std::this_thread::sleep_for(50ms);
    ResultStore::Get().Drain();  // 清空

    EXPECT_EQ(engine.FreeTasks(), engine.TotalTasks())
        << "Pool should be idle after baseline phase";

    // Phase 2: 过载（瞬间 200 条注入到 4 槽位池）
    for (int i = 0; i < 200; ++i)
        engine.InjectCommand("-m:PeakFast -f:nop");

    // Phase 3: 恢复 — 等池子空闲
    auto t0 = std::chrono::steady_clock::now();
    for (int w = 0; w < 200; ++w) {
        if (engine.FreeTasks() == engine.TotalTasks()) break;
        std::this_thread::sleep_for(10ms);
    }
    std::this_thread::sleep_for(100ms);
    auto recovery_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // Phase 4: 验证恢复后仍可正常工作
    for (int i = 0; i < 20; ++i) {
        engine.InjectCommand("-m:PeakFast -f:nop");
        std::this_thread::sleep_for(5ms);
    }
    for (int w = 0; w < 50 && engine.FreeTasks() < engine.TotalTasks(); ++w)
        std::this_thread::sleep_for(10ms);

    engine.InjectCommand("/exit");
    runner.join();

    std::cout << "[Recovery] Overload recovery time: " << recovery_ms
              << "ms, pool=" << engine.FreeTasks() << "/" << engine.TotalTasks()
              << std::endl;

    EXPECT_EQ(engine.FreeTasks(), engine.TotalTasks())
        << "Pool should fully recover after overload";
    EXPECT_LT(recovery_ms, 3000)
        << "Recovery should complete within 3s after overload";

    CleanupModule("PeakFast");
}

// ================================================================
//  Test 6: Slot exception caught (catch-all safety net)
// ================================================================
TEST_F(PeakStressTest, SlotExceptionDoesNotCrashProcess) {
    auto& mgr = ModuleLifeManager::GetInstance();
    auto& bus = EventBus::GetInstance();
    ResultStore::Get().Clear();

    // 注册会抛异常的模块
    mgr.UnloadModule("CrashTest");
    class CrashMod : public ModuleBaseObject {
    public:
        const char* GetName() const override { return "CrashTest"; }
        bool OnInit() override {
            REGISTER_FUNC("boom", "throws exception", {
                throw std::runtime_error("Deliberate module crash!");
            });
            return true;
        }
    };
    mgr.AddModule(std::make_unique<CrashMod>());

    const int W = StressWorkers();
    ShellEngine engine(W, W);
    std::thread runner([&]() { engine.RunWithoutInput(); });

    // 注入会抛出异常的命令
    engine.InjectCommand("-m:CrashTest -f:boom");

    std::this_thread::sleep_for(200ms);

    // 再注入正常命令——应该仍然工作
    RegisterFastModule("PeakFast");
    engine.InjectCommand("-m:PeakFast -f:nop");
    std::this_thread::sleep_for(200ms);

    engine.InjectCommand("/exit");
    runner.join();

    // 如果进程还没崩（到达这里），说明 catch(...) 兜底生效
    std::cout << "[SlotException] Process survived deliberate module crash!" << std::endl;
    SUCCEED();

    mgr.UnloadModule("CrashTest");
    CleanupModule("PeakFast");
}
