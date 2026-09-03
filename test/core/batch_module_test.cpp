/// =================================================================
///  BatchModule 隔离测试 (v2.8)
/// =================================================================
///  验证 test_shell 第一个多分片可暂停可恢复模块。
///
///  关键认知（实测得出，修正初版假设）：
///    - ResultStore/sink 收的是「任务级」结果，不是「分片级」——
///      Worker 跑完 while(Step) 后只对最后一个 CurrentPack PushResult 一次
///      (ShellEngine.h:376-379)。所以一个 N 分片任务，sink 只收 1 条。
///    - 分片执行数要靠 EventBus 订阅 Batch.chunk 信号统计（每分片都 emit）。
///    - chunk 读文件很快，经过引擎的任务秒完出池，FindTask 赶不上。
///      所以暂停/恢复测试用「纯 Task 手工构造 + 手动 Step」（仿
///      SnapshotCycleTest），不经过引擎，确定性 100%。
///
///  测试范围:
///    1. 基本多分片执行（引擎集成：订阅 Batch.chunk 统计分片数 + sink 收任务结果）
///    2. 暂停在分片边界（纯 Task 手工 Step，直接 Pause）
///    3. 恢复后继续（纯 Task：Restore + Resume + Step 跑完剩余）
///    4. 落盘往返（TaskFilePersistence Save/Load，shards_json 含 chunk 字段）
///    5. 边界（不存在目录 → 0 chunks）
/// =================================================================

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/MemTaskStore.h"
#include "core/ModuleLifeManager.h"
#include "core/ParmarPack.h"
#include "core/ResultStore.h"
#include "core/ShellEngine.h"
#include "core/Task.h"
#include "core/TaskFilePersistence.h"
#include "core/TasksPool.h"
#include "event_bus/event_bus.h"
#include "modules/BatchModule.h"
#include "modules/TaskManagerModule.h"
#include "sdk/IModule.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ================================================================
//  辅助：RAII 临时目录 + 建文件
// ================================================================
class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("batch_test_" + std::to_string(counter_++));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    std::string Path() const { return path_.string(); }

    // 在临时目录里建一个文件，写入 lines 行内容（每行 "line N"）
    void MakeFile(const std::string& name, size_t lines) {
        std::ofstream f(path_ / name);
        ASSERT_TRUE(f.is_open());
        for (size_t i = 0; i < lines; ++i) f << "line " << i << "\n";
    }

private:
    static std::atomic<int> counter_;
    fs::path path_;
};
std::atomic<int> TempDir::counter_{0};

// 激进清理 — 排空单例，防跨测试污染
// UnloadModule("Batch") 会删 Batch.* 信号（含测试订阅的 Batch.chunk slot），
// 所以用例1 订阅的统计 slot 会跟着一起清掉，无需额外 RemoveSignal。
static void AggressiveCleanup() {
    ResultStore::Get().Clear();
    auto& mgr = ModuleLifeManager::GetInstance();
    mgr.UnloadModule("Batch");
    mgr.UnloadModule("TaskManager");
}

// ================================================================
//  纯 Task 测试用的接线辅助
//  注册 Batch 模块到 ModuleLifeManager（AddModule 自动 ForEachFunc 接线
//  Batch.count / Batch.chunk 信号），让 Task::Step 能通过 EventBus 调到。
// ================================================================
static void RegisterBatchForManualStep() {
    auto& mgr = ModuleLifeManager::GetInstance();
    mgr.UnloadModule("Batch");
    mgr.AddModule(std::make_unique<BatchModule>());
}

// 手工构造一个 Batch count 入口分片 pack
static std::unique_ptr<ParmarPack> MakeCountPack(const std::string& dir, int chunk) {
    auto p = std::make_unique<ParmarPack>();
    p->mod_id  = "Batch";
    p->func_id = "count";
    p->Set("dir", dir);
    p->Set("chunk", std::to_string(chunk));
    return p;
}

// ================================================================
//  1. 基本多分片执行（引擎集成）
// ================================================================
TEST(BatchModuleTest, CountLinesAcrossFiles) {
    AggressiveCleanup();
    TempDir tmp;
    tmp.MakeFile("a.txt", 25);   // chunk=10 → 3 块 (10,10,5)
    tmp.MakeFile("b.txt", 8);    // chunk=10 → 1 块 (8)

    // 订阅 Batch.chunk 信号统计分片执行数（每分片 emit 一次）
    // BatchModule 注册时 ModuleLifeManager 已注册 Batch.chunk 信号。
    RegisterBatchForManualStep();  // 先注册模块 → 信号已存在
    auto& bus = EventBus::GetInstance();
    std::atomic<int> chunk_runs{0};
    // 注意：Batch.chunk 是 Signal<ParmarPack*>，订阅用无对象绑定的 LinkSlotFunc
    bus.LinkSlotFunc<ParmarPack*>("Batch.chunk",
        [&chunk_runs](ParmarPack*) { chunk_runs.fetch_add(1); });

    MemPersistence store;
    ShellEngine engine(8, 4);
    engine.SetTaskPersistence(&store);

    std::vector<ParmarPack> sink;
    std::mutex sink_mtx;
    engine.SetResultSink([&](const ParmarPack& p) {
        std::lock_guard<std::mutex> lk(sink_mtx);
        sink.push_back(p);
    });

    std::thread runner([&]() { engine.RunWithoutInput(); });
    engine.InjectCommand("-m:Batch -f:count -v:dir|" + tmp.Path() + ",chunk|10");

    // 等任务级结果（1 条）
    for (int w = 0; w < 300 && sink.size() < 1; ++w)
        std::this_thread::sleep_for(10ms);

    engine.RequestStop();
    runner.join();

    // 任务级结果：sink 只收 1 条（最后分片的 CurrentPack）
    ASSERT_EQ(sink.size(), 1u) << "任务级结果应只有 1 条";
    EXPECT_TRUE(sink[0].success) << "最后分片应成功: " << sink[0].return_value;

    // 分片级执行数：4 个 chunk（a.txt 3 块 + b.txt 1 块）
    // 入口分片是 Batch.count，不触发 Batch.chunk 订阅
    EXPECT_EQ(chunk_runs.load(), 4) << "应执行 4 个 chunk 分片";

    AggressiveCleanup();
}

// ================================================================
//  2. 暂停在分片边界（纯 Task 手工 Step）
// ================================================================
TEST(BatchModuleTest, PauseAtChunkBoundary) {
    AggressiveCleanup();
    RegisterBatchForManualStep();
    auto& bus = EventBus::GetInstance();

    TempDir tmp;
    tmp.MakeFile("big.txt", 1000);  // chunk=10 → 100 块

    // 手工构造入口分片，Acquire 一个 Task
    TasksPool pool(4);
    Task* t = pool.Acquire(MakeCountPack(tmp.Path(), 10));
    ASSERT_NE(t, nullptr);

    // Step 1：跑入口分片（count），它会枚举 + PushShard 100 个 chunk
    ASSERT_TRUE(t->Step(bus));
    ASSERT_EQ(t->GetTotalShards(), 101u) << "入口 + 100 chunk";

    // 再 Step 几个 chunk，然后 Pause（在分片边界）
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(t->Step(bus));
    size_t ran = t->GetCurrentShard();  // 入口 + 5 chunk = 6
    t->Pause();
    ASSERT_EQ(t->GetState(), Task::State::PAUSED);

    // 暂停后 Step 应返回 false
    ASSERT_FALSE(t->Step(bus));

    // 导出快照：current_shard 应 = 6（跑到第 6 个分片后暂停）
    TaskRecord rec = t->ExportRecord();
    EXPECT_EQ(rec.state, "PAUSED");
    EXPECT_EQ(rec.current_shard, ran);
    EXPECT_GT(rec.current_shard, 1u) << "应已跑过入口 + 至少一块";
    EXPECT_LT(rec.current_shard, 101u) << "不应跑完全部";

    pool.Release(t);
    AggressiveCleanup();
}

// ================================================================
//  3. 恢复后继续（Restore + Resume + Step 跑完剩余）
// ================================================================
TEST(BatchModuleTest, ResumeContinuesFromCheckpoint) {
    AggressiveCleanup();
    RegisterBatchForManualStep();
    auto& bus = EventBus::GetInstance();

    TempDir tmp;
    tmp.MakeFile("big.txt", 50);  // chunk=10 → 5 块，少一点跑得快

    TasksPool pool(4);
    Task* t = pool.Acquire(MakeCountPack(tmp.Path(), 10));
    ASSERT_NE(t, nullptr);

    // 跑入口 + 2 个 chunk，然后 Pause
    ASSERT_TRUE(t->Step(bus));   // 入口
    ASSERT_TRUE(t->Step(bus));   // chunk 1
    ASSERT_TRUE(t->Step(bus));   // chunk 2
    ASSERT_EQ(t->GetCurrentShard(), 3u);
    t->Pause();

    // 存快照
    MemPersistence store;
    store.Save(t->ExportRecord());
    auto rec = store.LoadAll();
    ASSERT_EQ(rec.size(), 1u);
    ASSERT_EQ(rec[0].current_shard, 3u);

    // 模拟"进程退出"：Release 原 task
    pool.Release(t);

    // 模拟"重启恢复"：新 Task + Restore + Resume
    Task restored;
    ASSERT_TRUE(restored.Restore(rec[0]));
    EXPECT_EQ(restored.GetState(), Task::State::PAUSED);
    EXPECT_EQ(restored.GetCurrentShard(), 3u) << "从第 3 个分片继续";
    EXPECT_EQ(restored.GetTotalShards(), 6u) << "入口 + 5 chunk";

    restored.Resume();
    // 手动 Step 跑完剩余（chunk 3,4,5）
    while (restored.Step(bus)) {}
    EXPECT_EQ(restored.GetState(), Task::State::COMPLETED)
        << "恢复后应跑完剩余分片";
    EXPECT_EQ(restored.GetCurrentShard(), 6u) << "应跑完全部 6 个分片";

    AggressiveCleanup();
}

// ================================================================
//  4. 落盘往返（TaskFilePersistence，shards_json 含 chunk 字段）
// ================================================================
TEST(BatchModuleTest, FilePersistenceRoundTrip) {
    AggressiveCleanup();
    RegisterBatchForManualStep();
    auto& bus = EventBus::GetInstance();

    TempDir tmp;
    tmp.MakeFile("a.txt", 25);  // chunk=10 → 3 块

    TasksPool pool(4);
    Task* t = pool.Acquire(MakeCountPack(tmp.Path(), 10));
    ASSERT_NE(t, nullptr);

    // 跑入口（push 3 个 chunk）+ 1 个 chunk，然后暂停
    ASSERT_TRUE(t->Step(bus));   // 入口
    ASSERT_TRUE(t->Step(bus));   // chunk 1
    t->Pause();

    // 用 TaskFilePersistence 落盘
    static std::atomic<int> store_counter{0};
    fs::path store_dir = fs::temp_directory_path() /
                         ("batch_store_" + std::to_string(store_counter++));
    fs::create_directories(store_dir);
    TaskFilePersistence store(store_dir.string());

    TaskRecord rec = t->ExportRecord();
    store.Save(rec);
    pool.Release(t);

    // 验证文件存在
    std::string dat_path = store_dir.string() + "/task_1.dat";
    EXPECT_TRUE(fs::exists(dat_path)) << "task_1.dat 应已落盘";

    // Load 回来验证内容
    auto loaded = store.Load(1);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->state, "PAUSED");
    // shards_json 应含 chunk 分片的 file/start/chunk 字段
    EXPECT_NE(loaded->shards_json.find("chunk"), std::string::npos)
        << "shards_json 应含 chunk func";
    EXPECT_NE(loaded->shards_json.find("a.txt"), std::string::npos)
        << "shards_json 应含文件名";
    EXPECT_NE(loaded->shards_json.find("start"), std::string::npos)
        << "shards_json 应含 start 字段";

    // 清理 store 目录
    std::error_code ec;
    fs::remove_all(store_dir, ec);
    AggressiveCleanup();
}

// ================================================================
//  5. 边界：不存在目录 → 0 chunks
// ================================================================
TEST(BatchModuleTest, NonexistentDirGraceful) {
    AggressiveCleanup();
    RegisterBatchForManualStep();
    auto& bus = EventBus::GetInstance();

    // 不存在的目录：FindFiles 返回空 → 0 chunk → 入口分片执行后
    // current_=1 >= shards_.size()=1 → COMPLETED（第一个 Step 即完成）
    TasksPool pool(4);
    Task* t = pool.Acquire(MakeCountPack("/no/such/dir/xyz", 10));
    ASSERT_NE(t, nullptr);

    // 入口分片执行：0 文件 → 0 chunk → SetTotalShards(1) → 任务完成
    ASSERT_TRUE(t->Step(bus));   // 入口分片执行 + current_++ + COMPLETED
    EXPECT_EQ(t->GetState(), Task::State::COMPLETED);
    EXPECT_EQ(t->GetTotalShards(), 1u) << "不存在目录应只有入口分片（0 chunk）";

    // 再 Step 应返回 false（已 COMPLETED）
    EXPECT_FALSE(t->Step(bus));

    // 入口分片 return_value 应含 "0 files"
    ParmarPack* cp = t->CurrentPack();
    ASSERT_NE(cp, nullptr);
    EXPECT_NE(cp->return_value.find("0 files"), std::string::npos)
        << "不存在目录应报 0 files: " << cp->return_value;

    pool.Release(t);
    AggressiveCleanup();
}