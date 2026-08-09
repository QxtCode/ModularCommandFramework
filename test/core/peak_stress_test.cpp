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

    constexpr int POOL = 16, WORKERS = 8, DURATION_MS = 1000;
    ShellEngine engine(POOL, WORKERS);
    std::thread runner([&]() { engine.Run(); });

    // Clear ResultStore
    ResultStore::Get().Drain();

    // 全速注入 1 秒
    std::atomic<bool> stop{false};
    std::atomic<int> injected{0};
    std::thread injector([&]() {
        while (!stop.load()) {
            engine.InjectCommand("-m:PeakFast -f:nop");
            injected.fetch_add(1);
            // 微小 yield，让引擎能呼吸
            std::this_thread::yield();
        }
    });

    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(DURATION_MS));
    stop.store(true);
    injector.join();

    // 等待所有任务完成
    for (int w = 0; w < 100; ++w) {
        if (engine.FreeTasks() == engine.TotalTasks()) break;
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(200ms);

    auto t1 = std::chrono::steady_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // 数 ResultStore 里实际完成的结果
    int completed = 0;
    auto batch = ResultStore::Get().Drain();
    completed += static_cast<int>(batch.size());
    // 再等一轮 drain
    std::this_thread::sleep_for(100ms);
    batch = ResultStore::Get().Drain();
    completed += static_cast<int>(batch.size());

    double throughput = total_ms > 0 ? (completed * 1000.0 / total_ms) : 0;

    engine.InjectCommand("/exit");
    runner.join();

    std::cout << "[RampUp] injected=" << injected.load()
              << " completed=" << completed
              << " time=" << total_ms << "ms"
              << " throughput=" << throughput << " cmd/s" << std::endl;

    // Throughput bounded by ShellEngine main loop (100ms cycle × pool_size)
    // Theoretical max: ~POOL_SIZE × 10 cycles/sec, practical ~30-80 cmd/s
    EXPECT_GT(completed, 10) << "Should complete at least some commands";
    EXPECT_GT(throughput, 10.0) << "Minimum throughput should exceed 10 cmd/s";
    std::cout << "[RampUp] Note: throughput limited by 100ms main-loop cycle." << std::endl;

    CleanupModule("PeakFast");
}

// ================================================================
//  Test 2: Burst — 瞬间超容量爆发
// ================================================================
TEST_F(PeakStressTest, BurstOverload) {
    RegisterFastModule("PeakFast");

    constexpr int POOL = 8, WORKERS = 4;
    constexpr int BURST = 500;  // 500 条命令瞬间注入

    ShellEngine engine(POOL, WORKERS);
    std::thread runner([&]() { engine.Run(); });

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

    // 收集结果
    int completed = 0;
    auto batch = ResultStore::Get().Drain();
    completed += static_cast<int>(batch.size());

    engine.InjectCommand("/exit");
    runner.join();

    double throughput = elapsed_ms > 0 ? (completed * 1000.0 / elapsed_ms) : 0;

    std::cout << "[Burst] " << BURST << " cmds injected, "
              << completed << " completed, "
              << "elapsed=" << elapsed_ms << "ms, "
              << "throughput=" << throughput << " cmd/s" << std::endl;

    // 核心断言: 不能崩溃
    EXPECT_GT(completed, 0) << "Some commands should complete";
    EXPECT_EQ(engine.FreeTasks(), engine.TotalTasks())
        << "Pool should fully recover after burst";

    CleanupModule("PeakFast");
}

// ================================================================
//  Test 3: Sustained — 30 秒持续负载（80% 容量）
// ================================================================
TEST_F(PeakStressTest, SustainedLoad) {
    RegisterFastModule("PeakFast");

    constexpr int POOL = 16, WORKERS = 8;
    constexpr int DURATION_SEC = 10;  // 10 秒足够发现趋势
    constexpr int RATE_PER_SEC = 200; // 目标注入速率

    ShellEngine engine(POOL, WORKERS);
    size_t mem_start = GetWorkingSetKB();

    std::thread runner([&]() { engine.Run(); });

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
    std::this_thread::sleep_for(200ms);

    auto t1 = std::chrono::steady_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // 收集
    int completed = 0;
    auto batch = ResultStore::Get().Drain();
    completed += static_cast<int>(batch.size());

    size_t mem_end = GetWorkingSetKB();

    engine.InjectCommand("/exit");
    runner.join();

    double throughput = total_ms > 0 ? (completed * 1000.0 / total_ms) : 0;
    long long mem_delta = static_cast<long long>(mem_end) - static_cast<long long>(mem_start);

    std::cout << "[Sustained] " << DURATION_SEC << "s: "
              << "injected=" << injected.load()
              << " completed=" << completed
              << " rejected=" << rejected.load()
              << " throughput=" << throughput << " cmd/s"
              << " mem_delta=" << mem_delta << "KB" << std::endl;

    EXPECT_GT(completed, 0);
    EXPECT_EQ(engine.FreeTasks(), engine.TotalTasks());
    // 10 秒持续负载，内存增长 < 50MB（含 OS 缓存波动）
    EXPECT_LT(mem_delta, 51200) << "Sustained load memory leak: " << mem_delta << "KB";

    CleanupModule("PeakFast");
}

// ================================================================
//  Test 4: 并发利用率 — 多条命令并发，验证 Pool 全利用
// ================================================================
TEST_F(PeakStressTest, PoolUtilization) {
    RegisterFastModule("PeakFast");

    constexpr int POOL = 8, WORKERS = 4;
    ShellEngine engine(POOL, WORKERS);
    std::thread runner([&]() { engine.Run(); });

    // 注入正好填满池子的命令数
    for (int i = 0; i < POOL; ++i)
        engine.InjectCommand("-m:PeakFast -f:nop");

    // 等池子被占满（或至少部分占用）
    std::this_thread::sleep_for(50ms);
    size_t used = engine.TotalTasks() - engine.FreeTasks();
    std::cout << "[PoolUtil] Pool utilization: " << used << "/" << engine.TotalTasks() << std::endl;

    // 等全部完成
    for (int w = 0; w < 100; ++w) {
        if (engine.FreeTasks() == engine.TotalTasks()) break;
        std::this_thread::sleep_for(20ms);
    }

    engine.InjectCommand("/exit");
    runner.join();

    EXPECT_EQ(engine.FreeTasks(), engine.TotalTasks())
        << "All tasks should return to pool";

    CleanupModule("PeakFast");
}

// ================================================================
//  Test 5: Recovery — 超载后恢复
// ================================================================
TEST_F(PeakStressTest, RecoveryAfterOverload) {
    RegisterFastModule("PeakFast");

    constexpr int POOL = 4, WORKERS = 2;

    ShellEngine engine(POOL, WORKERS);
    std::thread runner([&]() { engine.Run(); });

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

    ShellEngine engine(4, 2);
    std::thread runner([&]() { engine.Run(); });

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
