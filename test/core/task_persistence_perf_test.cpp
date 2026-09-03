/// =================================================================
///  task_persistence_perf_test — 任务存储/加载对框架的影响（隔离测试）
/// =================================================================
///  测三件事：
///   1. File vs Mem 后端本身的 Save/Load 单次耗时（隔离测后端）
///   2. 批量 N 个任务 Save + LoadAll 总用时（看总耗时秒数）
///   3. 持久化对框架吞吐的影响（跑任务时每任务 Save 一次快照，
///      对比 Null / Mem / File 三种后端）
///
///  复用已有模式：TempDir（RAII 临时目录，来自 task_filepersistence_test.cpp）、
///  RegisterFastModule 风格的快模块、RunWithoutInput + result sink 计数。
/// =================================================================
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/ITaskPersistence.h"
#include "core/MemTaskStore.h"
#include "core/NullTaskStore.h"
#include "core/TaskFilePersistence.h"
#include "core/ModuleLifeManager.h"
#include "core/ParmarPack.h"
#include "core/ResultStore.h"
#include "core/ShellEngine.h"
#include "core/Task.h"
#include "core/TasksPool.h"
#include "event_bus/event_bus.h"
#include "sdk/IModule.h"
#include "core/ModuleBaseObject.h"

using namespace std::chrono_literals;
namespace fs = std::filesystem;

// ================================================================
//  复用：TempDir（RAII 临时目录，来自 task_filepersistence_test.cpp）
// ================================================================
class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() / ("perf_persist_" + std::to_string(rand()));
        fs::create_directories(path_);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path_, ec); }
    std::string Path() const { return path_.string(); }
private:
    fs::path path_;
};

// ================================================================
//  复用：快模块（来自 peak_stress_test.cpp，关掉 stdout 标题）
// ================================================================
static void RegisterFastModule() {
    auto& mgr = ModuleLifeManager::GetInstance();
    mgr.UnloadModule("PerfFast");
    class FastMod : public ModuleBaseObject {
    public:
        const char* GetName() const override { return "PerfFast"; }
        bool OnInit() override {
            REGISTER_FUNC("nop", "", {
                pack->return_value = "ok";
                pack->success = true;
            });
            return true;
        }
    };
    mgr.AddModule(std::make_unique<FastMod>());
}

static int StressWorkers() {
    unsigned hw = std::thread::hardware_concurrency();
    return (hw > 2) ? static_cast<int>(hw) - 2 : 1;
}

// 造一个真实的 TaskRecord（带 shards_json，模拟 ExportRecord 的输出）
static TaskRecord MakeRecord(uint32_t id, size_t shards) {
    TaskRecord r;
    r.task_id = id;
    r.state = "PAUSED";
    r.current_shard = shards / 2;
    r.declared_total = shards;
    r.created_at_ms = 1725000000000 + id;
    r.scheduled_at_ms = 0;
    r.retry_count = 0;
    r.max_retries = 3;
    // 模拟真实 shards_json：N 个分片对象
    r.shards_json = "[";
    for (size_t i = 0; i < shards; ++i) {
        if (i) r.shards_json += ",";
        r.shards_json += "{\"mod\":\"PerfFast\",\"func\":\"nop\",\"p\":{}}";
    }
    r.shards_json += "]";
    return r;
}

// ================================================================
//  Test 1: File vs Mem 单次 Save/Load 耗时
// ================================================================
TEST(TaskPersistencePerfTest, SingleSaveLoadLatency) {
    const int ROUNDS = 1000;
    const size_t SHARDS = 10;
    auto rec = MakeRecord(1, SHARDS);

    // --- Mem ---
    MemPersistence mem;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < ROUNDS; ++i) {
        rec.task_id = i;
        mem.Save(rec);
    }
    auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < ROUNDS; ++i) {
        mem.Load(i);
    }
    auto t2 = std::chrono::steady_clock::now();
    double mem_save_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / (double)ROUNDS;
    double mem_load_us = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count() / (double)ROUNDS;

    // --- File ---
    TempDir tmp;
    TaskFilePersistence file(tmp.Path());
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < ROUNDS; ++i) {
        rec.task_id = i;
        file.Save(rec);
    }
    t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < ROUNDS; ++i) {
        file.Load(i);
    }
    t2 = std::chrono::steady_clock::now();
    double file_save_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / (double)ROUNDS;
    double file_load_us = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count() / (double)ROUNDS;

    std::cout << "[SingleLatency] rounds=" << ROUNDS << " shards=" << SHARDS << "\n"
              << "  Mem  Save: " << mem_save_us  << " us/op   Load: " << mem_load_us  << " us/op\n"
              << "  File Save: " << file_save_us << " us/op   Load: " << file_load_us << " us/op\n"
              << "  File/Mem Save ratio: " << (mem_save_us > 0 ? file_save_us / mem_save_us : 0) << "x\n"
              << "  File/Mem Load ratio: " << (mem_load_us > 0 ? file_load_us / mem_load_us : 0) << "x" << std::endl;

    // File 必然比 Mem 慢（I/O），但都应在合理范围
    EXPECT_GT(file_save_us, 0);
    EXPECT_GT(file_load_us, 0);
}

// ================================================================
//  Test 2: 批量 Save + LoadAll 总用时（用户要看总耗时秒数）
// ================================================================
TEST(TaskPersistencePerfTest, BatchSaveLoadAllTotalTime) {
    const int N[] = {100, 1000, 5000};
    const size_t SHARDS = 10;

    std::cout << "[BatchTotal] shards_per_task=" << SHARDS << "\n";
    for (int n : N) {
        std::vector<TaskRecord> recs;
        recs.reserve(n);
        for (int i = 0; i < n; ++i) recs.push_back(MakeRecord(i, SHARDS));

        // --- Mem ---
        MemPersistence mem;
        auto t0 = std::chrono::steady_clock::now();
        for (auto& r : recs) mem.Save(r);
        auto t1 = std::chrono::steady_clock::now();
        auto mem_loaded = mem.LoadAll();
        auto t2 = std::chrono::steady_clock::now();
        double mem_save_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        double mem_load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();

        // --- File ---
        TempDir tmp;
        TaskFilePersistence file(tmp.Path());
        t0 = std::chrono::steady_clock::now();
        for (auto& r : recs) file.Save(r);
        t1 = std::chrono::steady_clock::now();
        auto file_loaded = file.LoadAll();
        t2 = std::chrono::steady_clock::now();
        double file_save_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        double file_load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
        double file_total_s = (file_save_ms + file_load_ms) / 1000.0;

        std::cout << "  N=" << n
                  << " | Mem  Save=" << mem_save_ms  << "ms Load=" << mem_load_ms  << "ms"
                     " (LoadAll=" << mem_loaded.size() << ")"
                  << " | File Save=" << file_save_ms << "ms Load=" << file_load_ms << "ms"
                     " (LoadAll=" << file_loaded.size() << ")"
                  << " | File总用时=" << file_total_s << "s" << std::endl;

        EXPECT_EQ(file_loaded.size(), static_cast<size_t>(n));  // 全部非终态，应全加载
    }
}

// ================================================================
//  Test 3: 持久化对框架吞吐的影响
//  跑一批快任务，每个任务完成时在 sink 里 Save 一次快照（模拟最坏落盘），
//  对比 Null / Mem / File 三种后端的吞吐。
// ================================================================
static double RunWithPersistence(ITaskPersistence* store, int W, int N) {
    ShellEngine engine(W, W);
    engine.SetTaskPersistence(store);
    std::atomic<int> completed{0};
    engine.SetResultSink([&completed, store](const ParmarPack& pack) {
        completed.fetch_add(1);
        // 模拟最坏情况：每个结果都落盘一次快照
        if (store) {
            TaskRecord r;
            r.task_id = static_cast<uint32_t>(completed.load());
            r.state = "COMPLETED";
            r.shards_json = "[{\"mod\":\"PerfFast\",\"func\":\"nop\"}]";
            store->Save(r);
        }
    });
    std::thread runner([&]() { engine.RunWithoutInput(); });

    auto t0 = std::chrono::steady_clock::now();
    std::thread injector([&]() {
        for (int i = 0; i < N; ++i)
            engine.InjectCommand("-m:PerfFast -f:nop");
    });
    injector.join();
    for (int w = 0; w < 30000; ++w) {
        if (engine.FreeTasks() == engine.TotalTasks() && completed.load() > 0) break;
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(3ms);
    auto t1 = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    engine.InjectCommand("/exit");
    runner.join();
    return us > 0 ? (completed.load() * 1'000'000.0 / us) : 0;
}

TEST(TaskPersistencePerfTest, PersistenceImpactOnThroughput) {
    RegisterFastModule();
    const int W = StressWorkers();
    const int N = 5000;
    const int ROUNDS = 3;

    struct Backend { const char* name; ITaskPersistence* store; };
    NullPersistence null_store;
    MemPersistence  mem_store;
    TempDir tmp;
    TaskFilePersistence file_store(tmp.Path());

    Backend backends[] = {
        {"Null", &null_store},
        {"Mem",  &mem_store},
        {"File", &file_store},
    };

    std::cout << "[PersistenceImpact] W=" << W << " N=" << N << " rounds=" << ROUNDS << "\n";
    for (auto& b : backends) {
        double best = 0, sum = 0;
        for (int r = 0; r < ROUNDS; ++r) {
            ResultStore::Get().Clear();
            double th = RunWithPersistence(b.store, W, N);
            sum += th;
            if (th > best) best = th;
        }
        // 结论走 stderr：stdout 重定向到 nul 丢噪声时，stderr 结论仍可见
        fprintf(stderr, "  %s: avg=%.0f best=%.0f cmd/s\n", b.name, sum / ROUNDS, best);
        std::cout << "  " << b.name << ": avg=" << (sum / ROUNDS) << " best=" << best << " cmd/s" << std::endl;
    }

    // 清理：删掉 File 后端写的文件 + 卸载模块
    ModuleLifeManager::GetInstance().UnloadModule("PerfFast");
    ResultStore::Get().Clear();
    // File 后端的 GC 清掉它自己写的 COMPLETED 记录
    file_store.GC();
}