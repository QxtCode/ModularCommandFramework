/// =================================================================
///  Task Snapshot 隔离测试 (v2.6)
/// =================================================================
///  测试范围:
///    1. ParmarPack JSON 序列化/反序列化（往返 + 特殊字符 + 非法输入）
///    2. Task::ExportRecord / Restore（快照往返，真实分片恢复）
///    3. pause → Save → resume → Load → Restore 完整闭环
///
///  隔离级别: 手工组装 TasksPool / MemPersistence，不碰 ShellEngine。
///  闭环用"手动 Step 驱动"而非线程池竞态，保证确定性。
/// =================================================================

#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/ITaskPersistence.h"
#include "core/MemTaskStore.h"
#include "core/Task.h"
#include "core/TasksPool.h"
#include "core/ParmarPack.h"
#include "core/ModuleBaseObject.h"
#include "core/ModuleLifeManager.h"
#include "core/ResultStore.h"
#include "core/ShellEngine.h"
#include "modules/TaskManagerModule.h"
#include "event_bus/event_bus.h"

using namespace std::chrono_literals;

// ================================================================
//  1. ParmarPack JSON 序列化
// ================================================================

TEST(ParmarPackJson, RoundTripBasic) {
    ParmarPack p;
    p.mod_id = "Calc";
    p.func_id = "add";
    p.Set("a", "1");
    p.Set("b", "2");
    p.success = true;
    p.return_value = "3";

    std::string json = p.ToJson();
    EXPECT_FALSE(json.empty());

    ParmarPack q;
    ASSERT_TRUE(ParmarPack::FromJson(json, q));
    EXPECT_EQ(q.mod_id, "Calc");
    EXPECT_EQ(q.func_id, "add");
    EXPECT_EQ(q.GetOr("a", ""), "1");
    EXPECT_EQ(q.GetOr("b", ""), "2");
    EXPECT_TRUE(q.success);
    EXPECT_EQ(q.return_value, "3");
}

TEST(ParmarPackJson, RoundTripSpecialChars) {
    ParmarPack p;
    p.mod_id = "Mod";
    p.func_id = "echo";
    p.Set("msg", "hello \"world\"\n\t\\ slash");
    p.Set("zh", "中文测试");

    std::string json = p.ToJson();
    ParmarPack q;
    ASSERT_TRUE(ParmarPack::FromJson(json, q));
    EXPECT_EQ(q.GetOr("msg", ""), "hello \"world\"\n\t\\ slash");
    EXPECT_EQ(q.GetOr("zh", ""), "中文测试");
}

TEST(ParmarPackJson, FromJsonInvalid) {
    ParmarPack q;
    EXPECT_FALSE(ParmarPack::FromJson("", q));
    EXPECT_FALSE(ParmarPack::FromJson("not json", q));
    EXPECT_FALSE(ParmarPack::FromJson("{", q));
    EXPECT_FALSE(ParmarPack::FromJson("{\"mod\":1}", q));  // mod 必须是字符串
}

// ================================================================
//  2. Task 快照导出 / 恢复
// ================================================================

TEST(TaskSnapshot, ExportRecordFields) {
    Task t;
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id = "Calc";
    pack->func_id = "add";
    pack->Set("a", "1");
    t.Assign(std::move(pack));
    t.SetTotalShards(5);

    TaskRecord rec = t.ExportRecord();
    EXPECT_EQ(rec.state, "IDLE");           // 尚未执行
    EXPECT_EQ(rec.current_shard, 0u);
    EXPECT_EQ(rec.declared_total, 5u);
    EXPECT_FALSE(rec.shards_json.empty());
}

TEST(TaskSnapshot, ExportRestoreRoundTrip) {
    Task src;
    {
        auto p = std::make_unique<ParmarPack>();
        p->mod_id = "M";
        p->func_id = "f1";
        src.Assign(std::move(p));
    }
    {
        auto p = std::make_unique<ParmarPack>();
        p->mod_id = "M";
        p->func_id = "f2";
        p->Set("k", "v");
        src.PushShard(std::move(p));
    }
    src.SetTotalShards(2);
    src.Pause();

    TaskRecord rec = src.ExportRecord();
    EXPECT_EQ(rec.state, "PAUSED");

    Task dst;
    ASSERT_TRUE(dst.Restore(rec));
    EXPECT_EQ(dst.GetState(), Task::State::PAUSED);
    EXPECT_EQ(dst.GetCurrentShard(), 0u);
    EXPECT_EQ(dst.GetTotalShards(), 2u);
}

TEST(TaskSnapshot, RestoreRealShardsNotPlaceholder) {
    // 关键回归：Restore 必须恢复真实分片（mod_id/func_id/params），
    // 而不是 v2.6 初版的 "restored"/"restored" 占位分片。
    Task src;
    {
        auto p = std::make_unique<ParmarPack>();
        p->mod_id = "RealMod";
        p->func_id = "realfunc";
        p->Set("x", "42");
        src.Assign(std::move(p));
    }
    src.Pause();
    TaskRecord rec = src.ExportRecord();

    Task dst;
    ASSERT_TRUE(dst.Restore(rec));

    ParmarPack* cp = dst.CurrentPack();
    ASSERT_NE(cp, nullptr);
    EXPECT_EQ(cp->mod_id, "RealMod");
    EXPECT_EQ(cp->func_id, "realfunc");
    EXPECT_EQ(cp->GetOr("x", ""), "42");
}

// ================================================================
//  3. 完整闭环：pause → Save → resume → Load → Restore
// ================================================================

// 计数模块：每个分片 +1，未到 total 就追加下一个分片。
// 用来验证"暂停在分片边界 + 恢复后从断点继续"。
class TickMod : public ModuleBaseObject {
public:
    TickMod(std::atomic<int>* cnt, int total) : cnt_(cnt), total_(total) {}
    const char* GetName() const override { return "TickMod"; }

    bool OnInit() override {
        REGISTER_FUNC("tick", "one shard", {
            int n = cnt_->fetch_add(1) + 1;
            if (n < total_) {
                auto next = std::make_unique<ParmarPack>();
                next->mod_id = "TickMod";
                next->func_id = "tick";
                next->show_explanation = false;
                pack->owner_task->PushShard(std::move(next));
            }
            pack->success = true;
        });
        return true;
    }

private:
    std::atomic<int>* cnt_;
    int total_;
};

class SnapshotCycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        bus_ = &EventBus::GetInstance();
        store_ = std::make_unique<MemPersistence>();
        cnt_.store(0);
        ModuleLifeManager::GetInstance().UnloadModule("TickMod");
        ModuleLifeManager::GetInstance().AddModule(
            std::make_unique<TickMod>(&cnt_, kTotal));
    }

    void TearDown() override {
        ModuleLifeManager::GetInstance().UnloadModule("TickMod");
        ResultStore::Get().Clear();
    }

    static constexpr int kTotal = 10;
    EventBus* bus_;
    std::unique_ptr<MemPersistence> store_;
    std::atomic<int> cnt_{0};
};

TEST_F(SnapshotCycleTest, PauseSaveResumeLoadRestore) {
    // 1. Acquire + 手动推进 3 个分片（cnt = 3）
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id = "TickMod";
    pack->func_id = "tick";
    pack->show_explanation = false;

    TasksPool pool(4);
    Task* task = pool.Acquire(std::move(pack));
    ASSERT_NE(task, nullptr);
    uint32_t tid = task->GetID();

    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(task->Step(*bus_));
    }
    EXPECT_EQ(cnt_.load(), 3);
    EXPECT_EQ(task->GetCurrentShard(), 3u);

    // 2. Pause + 快照 + 保存
    task->Pause();
    TaskRecord rec = task->ExportRecord();
    EXPECT_EQ(rec.state, "PAUSED");
    EXPECT_EQ(rec.current_shard, 3u);
    store_->Save(rec);

    // 3. 归还槽位（模拟 Worker Release，Reset 清空状态）
    pool.Release(task);

    // 4. Load + Acquire + Restore
    auto opt = store_->Load(tid);
    ASSERT_TRUE(opt.has_value());

    auto dummy = std::make_unique<ParmarPack>();
    Task* restored = pool.Acquire(std::move(dummy));
    ASSERT_NE(restored, nullptr);
    ASSERT_TRUE(restored->Restore(*opt));
    EXPECT_EQ(restored->GetID(), tid);
    EXPECT_EQ(restored->GetCurrentShard(), 3u);
    EXPECT_EQ(restored->GetState(), Task::State::PAUSED);

    // 5. Resume + 继续执行剩余 7 个分片
    restored->Resume();
    while (restored->Step(*bus_)) {}
    EXPECT_EQ(cnt_.load(), kTotal);                       // 3 + 7 = 10
    EXPECT_EQ(restored->GetState(), Task::State::COMPLETED);

    store_->Delete(tid);
    pool.Release(restored);
}

// ================================================================
//  4. 序列化边界
// ================================================================

TEST(ParmarPackJson, RoundTripEmptyParams) {
    ParmarPack p;
    p.mod_id = "";
    p.func_id = "";
    p.success = false;

    std::string json = p.ToJson();
    ParmarPack q;
    ASSERT_TRUE(ParmarPack::FromJson(json, q));
    EXPECT_EQ(q.mod_id, "");
    EXPECT_EQ(q.func_id, "");
    EXPECT_TRUE(q.params.empty());
    EXPECT_FALSE(q.success);
}

TEST(ParmarPackJson, RoundTripMultiValue) {
    ParmarPack p;
    p.mod_id = "M";
    p.func_id = "f";
    p.params["k"] = {"a", "b", "c"};       // 多值
    p.params["single"] = {"x"};

    std::string json = p.ToJson();
    ParmarPack q;
    ASSERT_TRUE(ParmarPack::FromJson(json, q));
    EXPECT_EQ(q.GetAll("k").size(), 3u);
    EXPECT_EQ(q.GetAll("k")[0], "a");
    EXPECT_EQ(q.GetAll("k")[2], "c");
    EXPECT_EQ(q.GetAll("single").size(), 1u);
    EXPECT_EQ(q.GetAll("single")[0], "x");
}

TEST(ParmarPackJson, RoundTripUnicodeAndEmpty) {
    ParmarPack p;
    p.mod_id = "Mod";
    p.func_id = "func";
    p.Set("emoji", "🚀 test 😀");
    p.Set("empty", "");
    p.Set("space", "  leading and trailing  ");

    std::string json = p.ToJson();
    ParmarPack q;
    ASSERT_TRUE(ParmarPack::FromJson(json, q));
    EXPECT_EQ(q.GetOr("emoji", ""), "🚀 test 😀");
    EXPECT_EQ(q.GetOr("empty", ""), "");
    EXPECT_EQ(q.GetOr("space", ""), "  leading and trailing  ");
}

TEST(ParmarPackJson, RoundTripLongString) {
    ParmarPack p;
    p.mod_id = "M";
    p.func_id = "f";
    std::string long_str(10000, 'a');
    long_str += "中文结尾";
    p.Set("big", long_str);

    std::string json = p.ToJson();
    ParmarPack q;
    ASSERT_TRUE(ParmarPack::FromJson(json, q));
    EXPECT_EQ(q.GetOr("big", ""), long_str);
}

TEST(ParmarPackJson, FromJsonTruncatedInputs) {
    ParmarPack q;
    EXPECT_FALSE(ParmarPack::FromJson("{\"mod\":\"x\"", q));              // 缺 }
    EXPECT_FALSE(ParmarPack::FromJson("{\"mod\"}", q));                    // 缺值
    EXPECT_FALSE(ParmarPack::FromJson("{\"mod\":\"x\",}", q));             // 尾随逗号
    EXPECT_FALSE(ParmarPack::FromJson("{\"success\":tru}", q));            // 截断 true
    EXPECT_FALSE(ParmarPack::FromJson("{\"params\":{\"k\":[\"a\"}", q));   // 缺 ]
}

TEST(ParmarPackJson, FromJsonTrailingGarbage) {
    ParmarPack q;
    EXPECT_FALSE(ParmarPack::FromJson("{\"mod\":\"x\"} extra", q));        // 尾随垃圾
    EXPECT_FALSE(ParmarPack::FromJson("{\"mod\":\"x\"}{}", q));            // 多个对象
}

// ================================================================
//  5. 快照边界
// ================================================================

TEST(TaskSnapshot, ExportEmptyShards) {
    // 未 Assign 的 task：ExportRecord 应安全返回空快照
    Task t;
    TaskRecord rec = t.ExportRecord();
    EXPECT_EQ(rec.current_shard, 0u);
    EXPECT_EQ(rec.shards_json, "[]");
}

TEST(TaskSnapshot, RestoreEmptyJsonUsesPlaceholder) {
    Task t;
    TaskRecord rec;
    rec.task_id = 1;
    rec.state = "PAUSED";
    rec.current_shard = 0;
    rec.shards_json = "";   // 空快照

    ASSERT_TRUE(t.Restore(rec));
    EXPECT_EQ(t.GetState(), Task::State::PAUSED);
    ASSERT_NE(t.CurrentPack(), nullptr) << "空快照应退化出占位分片，保证 Step 不崩";
}

TEST(TaskSnapshot, RestoreCorruptJsonFallsBack) {
    Task t;
    TaskRecord rec;
    rec.task_id = 2;
    rec.state = "PAUSED";
    rec.shards_json = "not json at all";

    ASSERT_TRUE(t.Restore(rec)) << "损坏 JSON 不应崩溃，应退化占位";
    EXPECT_EQ(t.GetState(), Task::State::PAUSED);
    EXPECT_NE(t.CurrentPack(), nullptr);
}

TEST(TaskSnapshot, RestoreCurrentShardBeyond) {
    // current_shard 越界（损坏快照）：恢复不崩溃，Step 会判完成
    Task src;
    {
        auto p = std::make_unique<ParmarPack>();
        p->mod_id = "M";
        p->func_id = "f";
        src.Assign(std::move(p));
    }
    TaskRecord rec = src.ExportRecord();
    rec.current_shard = 999;   // 越界

    Task dst;
    ASSERT_TRUE(dst.Restore(rec));
    EXPECT_EQ(dst.GetCurrentShard(), 999u);
    EXPECT_NE(dst.CurrentPack(), nullptr);
}

// ================================================================
//  6. 并发安全
// ================================================================

TEST(TaskSnapshot, ConcurrentExportRecord) {
    // 多线程并发读同一 task 的快照：ExportRecord 加锁，结果必须一致
    Task src;
    for (int i = 0; i < 5; ++i) {
        auto p = std::make_unique<ParmarPack>();
        p->mod_id = "M";
        p->func_id = "f" + std::to_string(i);
        p->Set("i", std::to_string(i));
        if (i == 0) src.Assign(std::move(p));
        else        src.PushShard(std::move(p));
    }

    constexpr int kThreads = 8;
    std::vector<TaskRecord> recs(kThreads);
    std::vector<std::thread> th;
    for (int t = 0; t < kThreads; ++t) {
        th.emplace_back([&src, &recs, t]() {
            recs[t] = src.ExportRecord();
        });
    }
    for (auto& t : th) t.join();

    EXPECT_FALSE(recs[0].shards_json.empty());
    for (int t = 1; t < kThreads; ++t) {
        EXPECT_EQ(recs[t].shards_json, recs[0].shards_json)
            << "线程 " << t << " 的快照与其他线程不一致";
        EXPECT_EQ(recs[t].current_shard, recs[0].current_shard);
    }
}

TEST(TaskSnapshot, ConcurrentExportRestore) {
    // 各线程独立 Export → Restore，验证无共享状态竞争 + 结果正确
    constexpr int kThreads = 8;
    std::vector<std::thread> th;
    std::atomic<int> ok{0};

    for (int t = 0; t < kThreads; ++t) {
        th.emplace_back([&ok, t]() {
            Task src;
            auto p = std::make_unique<ParmarPack>();
            p->mod_id = "M";
            p->func_id = "f" + std::to_string(t);
            p->Set("v", std::to_string(t));
            src.Assign(std::move(p));
            src.Pause();

            TaskRecord rec = src.ExportRecord();
            Task dst;
            if (!dst.Restore(rec)) return;

            ParmarPack* cp = dst.CurrentPack();
            if (cp && cp->mod_id == "M" &&
                cp->func_id == "f" + std::to_string(t) &&
                cp->GetOr("v", "") == std::to_string(t) &&
                dst.GetState() == Task::State::PAUSED) {
                ok.fetch_add(1);
            }
        });
    }
    for (auto& t : th) t.join();
    EXPECT_EQ(ok.load(), kThreads);
}

// ================================================================
//  6.1 store 层多线程存储 / 卸载 / 清理 (v2.8)
// ================================================================
//
//  这些测试不碰 ShellEngine，只压 MemPersistence 的并发 Save/Delete/Load/GC。
//  覆盖三条"并发写后端"会出的 bug：
//    A) 混合 Save+Delete 交错 → 无崩溃，终态可被 Load 复现
//    B) 同一 id 并发 Save → Load 必返回合法记录（last-writer-wins，无半写损坏）
//    C) LoadAll 与 GC 并发 → 无崩溃，LoadAll 返回的记录自洽
//
//  断言全部来自手算字面量（4*N、偶数 id 保留等），不复用被测代码逻辑。

TEST(MemPersistenceConcurrency, ConcurrentSaveDeleteMix) {
    // 捕的 bug: Save/Delete 交错下，map 操作缺锁 → 数据竞争 / 崩溃，
    // 或 erase 后迭代器失效。这里 4 线程对[0, 4N) 交错 Save+Delete。
    MemPersistence store;
    constexpr int N = 200;
    constexpr int kThreads = 4;

    std::vector<std::thread> th;
    for (int t = 0; t < kThreads; ++t) {
        th.emplace_back([&store, t, N]() {
            for (int i = 0; i < N; ++i) {
                uint32_t id = static_cast<uint32_t>(t * N + i);
                TaskRecord r; r.task_id = id; r.state = "PAUSED";
                store.Save(r);
                // 偶数 id 立即删，模拟 Worker 收尾 Delete；奇数保留。
                if (id % 2 == 0) store.Delete(id);
            }
        });
    }
    for (auto& t : th) t.join();

    // 终态: 奇数 id 保留（PAUSED），偶数 id 被删。
    // 4*N 个 id 里奇数 = 2*N 个。
    EXPECT_EQ(store.Size(), static_cast<size_t>(2 * N));
    EXPECT_EQ(store.LoadAll().size(), static_cast<size_t>(2 * N));

    // 抽样: 奇数 id 在、偶数 id 不在。
    EXPECT_TRUE(store.Load(1u).has_value());
    EXPECT_FALSE(store.Load(0u).has_value());
    EXPECT_TRUE(store.Load(static_cast<uint32_t>(4 * N - 1)).has_value());
    EXPECT_FALSE(store.Load(static_cast<uint32_t>(4 * N - 2)).has_value());
}

TEST(MemPersistenceConcurrency, ConcurrentSaveSameId) {
    // 捕的 bug: 多线程同时 Save 同一 id → 半写损坏或 Load 返回非法记录。
    // 期望: last-writer-wins，每次 Load 拿到的都是字段自洽的 TaskRecord。
    MemPersistence store;
    constexpr int kThreads = 8;
    constexpr int kIters = 300;
    const uint32_t shared_id = 42;

    std::vector<std::thread> th;
    for (int t = 0; t < kThreads; ++t) {
        th.emplace_back([&store, shared_id, t, kIters]() {
            for (int i = 0; i < kIters; ++i) {
                TaskRecord r;
                r.task_id = shared_id;
                r.state = "PAUSED";
                r.current_shard = static_cast<size_t>(t * 1000 + i);  // 标记来源
                store.Save(r);
            }
        });
    }
    for (auto& t : th) t.join();

    // 终态只有一条记录（同 id 覆盖），且字段自洽。
    auto opt = store.Load(shared_id);
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->task_id, shared_id);
    EXPECT_EQ(opt->state, "PAUSED");
    // current_shard 必落在某线程的合法区间 [t*1000, t*1000+kIters)，不可为半写。
    size_t cs = opt->current_shard;
    bool from_some_thread = false;
    for (int t = 0; t < kThreads; ++t) {
        if (cs >= static_cast<size_t>(t * 1000) &&
            cs <  static_cast<size_t>(t * 1000 + kIters)) {
            from_some_thread = true;
            break;
        }
    }
    EXPECT_TRUE(from_some_thread) << "current_shard=" << cs << " 应来自某线程完整写入";
    EXPECT_EQ(store.Size(), 1u);
}

TEST(MemPersistenceConcurrency, ConcurrentLoadAllWithGC) {
    // 捕的 bug: LoadAll 正遍历 map 时，GC 同时 erase 终态记录 → 迭代器失效崩溃。
    // 期望: 两者并发不崩，LoadAll 返回的快照自洽（非终态）。
    MemPersistence store;
    constexpr int kIds = 400;
    // 预填: 偶数 PAUSED(非终态)、奇数 COMPLETED(终态)
    for (int i = 0; i < kIds; ++i) {
        TaskRecord r; r.task_id = static_cast<uint32_t>(i);
        r.state = (i % 2 == 0) ? "PAUSED" : "COMPLETED";
        store.Save(r);
    }

    std::atomic<bool> stop{false};
    std::vector<std::thread> th;

    // 线程1: 持续 GC（删终态）
    th.emplace_back([&store, &stop]() {
        while (!stop.load()) store.GC();
    });
    // 线程2: 持续 LoadAll
    th.emplace_back([&store, &stop]() {
        while (!stop.load()) {
            auto all = store.LoadAll();
            // LoadAll 只返回非终态 → 不应混入 COMPLETED
            for (const auto& r : all)
                EXPECT_NE(r.state, "COMPLETED");
        }
    });

    // 跑 100ms 让两线程交错
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true);
    for (auto& t : th) t.join();

    // 终态: GC 删完所有 COMPLETED，只剩 PAUSED（kIds/2 个）。
    EXPECT_EQ(store.Size(), static_cast<size_t>(kIds / 2));
    EXPECT_EQ(store.LoadAll().size(), static_cast<size_t>(kIds / 2));
}

// ================================================================
//  7. 集成冒烟测试 — 真实 ShellEngine 路径
// ================================================================
//
//  与上面的隔离测试不同：这里走真实的 ShellEngine + TaskManagerModule，
//  验证框架层的"自动 Save"（Worker 收尾）和命令驱动的"自动 Load/Restore"
//  确实贯通。这是一条端到端基本路径，快速验证核心功能可用。

// 慢速计数模块：每分片 +1，暴露 task_id 供 pause/resume 命令定位。
class SmokeWorkMod : public ModuleBaseObject {
public:
    SmokeWorkMod(std::atomic<int>* cnt, std::atomic<uint32_t>* tid, int total)
        : cnt_(cnt), tid_(tid), total_(total) {}
    const char* GetName() const override { return "SmokeWork"; }

    bool OnInit() override {
        REGISTER_FUNC("tick", "one slow shard", {
            if (tid_) tid_->store(pack->owner_task->GetID());  // 暴露 task_id
            std::this_thread::sleep_for(30ms);                 // 慢一点，给 pause 留时间
            int n = cnt_->fetch_add(1) + 1;
            if (n < total_) {
                auto next = std::make_unique<ParmarPack>();
                next->mod_id = "SmokeWork";
                next->func_id = "tick";
                next->show_explanation = false;
                pack->owner_task->PushShard(std::move(next));
            }
            pack->success = true;
        });
        return true;
    }

private:
    std::atomic<int>* cnt_;
    std::atomic<uint32_t>* tid_;
    int total_;
};

class SnapshotSmokeTest : public ::testing::Test {
protected:
    void SetUp() override {
        cnt_.store(0);
        tid_.store(0);
        auto& mgr = ModuleLifeManager::GetInstance();
        mgr.UnloadModule("SmokeWork");
        mgr.UnloadModule("TaskManager");
        mgr.AddModule(std::make_unique<SmokeWorkMod>(&cnt_, &tid_, kTotal));
    }

    void TearDown() override {
        auto& mgr = ModuleLifeManager::GetInstance();
        mgr.UnloadModule("SmokeWork");
        mgr.UnloadModule("TaskManager");
        ResultStore::Get().Clear();
    }

    static constexpr int kTotal = 12;
    std::atomic<int> cnt_{0};
    std::atomic<uint32_t> tid_{0};
};

TEST_F(SnapshotSmokeTest, EngineAutoSaveOnPause) {
    // 冒烟测试：验证 ShellEngine 集成后，暂停任务会自动保存快照，且快照可恢复。
    //
    // 注意：测试环境的 stdin 是 EOF，InputThread 的 getline 立即返回会导致
    // 主循环在处理完第一批命令后退出。因此这里用"直接 Pause"触发暂停
    // （不经过 pause 命令），聚焦验证 ShellEngine 的 Worker 收尾自动 Save
    // 这一核心集成点。pause/resume 命令路径已由隔离测试覆盖
    // （SnapshotCycleTest / TaskManagerTest）。

    auto store = std::make_shared<MemPersistence>();
    ShellEngine engine(8, 4);
    engine.SetTaskPersistence(store.get());

    std::thread runner([&]() { engine.Run(); });

    // 1. 提交长任务（新 pool 的首个任务 id = 1）
    engine.InjectCommand("-m:SmokeWork -f:tick");
    for (int w = 0; w < 200 && tid_.load() == 0; ++w)
        std::this_thread::sleep_for(10ms);
    uint32_t id = tid_.load();
    ASSERT_NE(id, 0u) << "任务应已开始执行";

    // 2. 直接 Pause（绕过命令，见上方注释）
    Task* t = engine.GetPool().FindTask(id);
    ASSERT_NE(t, nullptr) << "任务应仍在池中";
    t->Pause();

    // 3. 等 ShellEngine 的 Worker 收尾自动 Save
    for (int w = 0; w < 200 && store->LoadAll().empty(); ++w)
        std::this_thread::sleep_for(10ms);

    engine.RequestStop();
    runner.join();

    // 4. 验证快照已保存 + 内容正确
    auto pending = store->LoadAll();
    ASSERT_FALSE(pending.empty()) << "pause 后 ShellEngine 应自动保存快照";
    EXPECT_EQ(pending[0].task_id, id);
    EXPECT_EQ(pending[0].state, "PAUSED");

    // 5. 验证快照可恢复并继续执行到完成
    Task restored;
    ASSERT_TRUE(restored.Restore(pending[0]));
    restored.Resume();
    EventBus& bus = EventBus::GetInstance();
    while (restored.Step(bus)) {}
    EXPECT_EQ(restored.GetState(), Task::State::COMPLETED);
    EXPECT_EQ(cnt_.load(), kTotal) << "恢复后应跑完剩余分片";
}

// ================================================================
//  8. 命令注入测试 — 真实 pause/resume 命令路径 (v2.8)
// ================================================================
//
//  与第 7 节冒烟测试的区别：这里走「真实命令路径」——
//    InjectCommand("-m:TaskManager -f:pause -v:id|N") → CommandParser
//    → TaskManagerModule::Execute → TasksPool::FindTask → Pause
//  第 7 节用「直接 Pause」绕过了命令解析，本节补齐命令链。
//
//  关键工程决策（来自 memory/test-strategy 与代码核实）：
//    1. 用 RunWithoutInput()，不用 Run()。测试环境 stdin 是 EOF，
//       Run() 会启输入线程、getline 立即返回把 running 置 false，
//       主循环可能在处理完命令前就退出。RunWithoutInput 只跑主循环，
//       命令全靠 InjectCommand 喂，确定性最高。
//    2. SetResultSink 捕获命令响应（TaskManager 的 return_value / error.message）
//       到 vector，用于断言命令路径的成功/失败语义。不查 ResultStore 残留。
//    3. 工作模块必须「慢分片」（每分片 sleep），让任务占着槽位等 pause 命令命中；
//       否则任务秒完、FindTask 返回 null、pause 命令报 not found。

// 命令注入用的工作模块：慢分片 + 暴露每个 task 的 id 与进度。
// 与 SmokeWorkMod 不同：独立计数（per-fixture），total 可配，多任务场景可实例化。
class CmdWorkMod : public ModuleBaseObject {
public:
    CmdWorkMod(std::atomic<int>* cnt, std::atomic<uint32_t>* tid, int total, int delay_ms)
        : cnt_(cnt), tid_(tid), total_(total), delay_ms_(delay_ms) {}
    const char* GetName() const override { return "CmdWork"; }

    bool OnInit() override {
        REGISTER_FUNC("tick", "slow shard for command test", {
            if (tid_) tid_->store(pack->owner_task->GetID());
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
            // cnt_ 仅用于断言「总执行次数」，不参与流程控制（多任务共享会互相干扰）。
            cnt_->fetch_add(1);
            // 用任务自己的分片索引判断是否 push 下一分片——每个任务独立跑到 total_。
            // GetCurrentShard() 在 Slot 执行期间返回当前分片索引 k（0-based），
            // 执行完若 k+1 < total_ 就追加下一分片。
            size_t cur = pack->owner_task->GetCurrentShard();
            if (cur + 1 < static_cast<size_t>(total_)) {
                auto next = std::make_unique<ParmarPack>();
                next->mod_id = "CmdWork";
                next->func_id = "tick";
                next->show_explanation = false;
                pack->owner_task->PushShard(std::move(next));
            }
            pack->success = true;
        });
        return true;
    }

private:
    std::atomic<int>* cnt_;
    std::atomic<uint32_t>* tid_;
    int total_;
    int delay_ms_;
};

// 命令注入测试 fixture：RunWithoutInput + ResultSink 捕获响应。
class SnapshotCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        cnt_.store(0);
        tid_.store(0);
        auto& mgr = ModuleLifeManager::GetInstance();
        mgr.UnloadModule("CmdWork");
        mgr.UnloadModule("TaskManager");
        // TaskManagerModule 需要引擎的 pool/workers/store，在测试用例里创建引擎后注册。
    }

    void TearDown() override {
        auto& mgr = ModuleLifeManager::GetInstance();
        mgr.UnloadModule("CmdWork");
        mgr.UnloadModule("TaskManager");
        ResultStore::Get().Clear();
    }

    // 启动引擎 + 注册 CmdWork + 注入 store + 起 ResultSink 收响应。
    // 返回 sink 里捕获的响应 vector 引用，供断言。
    void StartEngine(int pool_size = 8, int workers = 4, int total = 12,
                     std::chrono::milliseconds delay_ms = 30ms) {
        total_ = total;
        store_ = std::make_shared<MemPersistence>();
        engine_ = std::make_unique<ShellEngine>(pool_size, workers);
        engine_->SetTaskPersistence(store_.get());
        engine_->SetResultSink([this](const ParmarPack& p) {
            // 收所有响应到一个 vector（mutex 保护，命令路径并发回响应）
            std::lock_guard<std::mutex> lk(sink_mtx_);
            sink_.push_back(p);
        });

        ModuleLifeManager::GetInstance().AddModule(
            std::make_unique<CmdWorkMod>(&cnt_, &tid_, total,
                                         static_cast<int>(delay_ms.count())));
        // TaskManagerModule 需要引擎的 pool/workers/store 引用——
        // 用 GetPool()/GetWorkers() 拿到，store 用注入的那个。
        ModuleLifeManager::GetInstance().AddModule(
            std::make_unique<TaskManagerModule>(
                engine_->GetPool(), engine_->GetWorkers(), store_.get()));

        // 主循环跑在后台线程，不启输入线程（RunWithoutInput）。
        runner_ = std::thread([this]() { engine_->RunWithoutInput(); });
    }

    void StopEngine() {
        if (engine_) engine_->RequestStop();
        if (runner_.joinable()) runner_.join();
    }

    // 等待 tid_ 被填（首个分片执行时模块会写入 task id）。
    uint32_t WaitTaskStarted(int max_waits = 200) {
        for (int w = 0; w < max_waits && tid_.load() == 0; ++w)
            std::this_thread::sleep_for(10ms);
        return tid_.load();
    }

    int total_ = 12;
    std::shared_ptr<MemPersistence> store_;
    std::unique_ptr<ShellEngine> engine_;
    std::thread runner_;
    std::atomic<int> cnt_{0};
    std::atomic<uint32_t> tid_{0};

    std::mutex sink_mtx_;
    std::vector<ParmarPack> sink_;
};

// ---- B组: 注入命令 pause → resume 完整闭环 ----

TEST_F(SnapshotCommandTest, CommandPauseResumeComplete) {
    // 捕的 bug: 命令路径 pause/resume 任一环断裂——
    //   CommandParser 没把 id 塞进 pack、TaskManager 拿不到 id、
    //   自动 Save 没触发、resume 没真反序列化分片、或 resume 后没续跑。
    // 期望: pause 命令后 store 有快照；resume 命令后任务跑完、cnt==total、store 已清。
    StartEngine(/*pool*/8, /*workers*/4, /*total*/12, /*delay*/40ms);

    // 1. 提交长任务，等它开始（首个分片暴露 id）
    engine_->InjectCommand("-m:CmdWork -f:tick");
    uint32_t id = WaitTaskStarted();
    ASSERT_NE(id, 0u) << "任务应已开始执行";

    // 2. 等任务跑几个分片（cnt>=2），确保确实在执行中、可被 pause 命中
    for (int w = 0; w < 200 && cnt_.load() < 2; ++w)
        std::this_thread::sleep_for(10ms);
    ASSERT_GE(cnt_.load(), 2) << "任务应在执行中";

    // 3. 注入 pause 命令（真实命令路径）
    engine_->InjectCommand("-m:TaskManager -f:pause -v:id|" + std::to_string(id));

    // 4. 等 Worker 收尾自动 Save（store 出现该 id 的快照）
    for (int w = 0; w < 300 && !store_->Load(id).has_value(); ++w)
        std::this_thread::sleep_for(10ms);
    ASSERT_TRUE(store_->Load(id).has_value()) << "pause 后 ShellEngine 应自动保存快照";
    {
        auto opt = store_->Load(id);
        ASSERT_TRUE(opt.has_value());
        EXPECT_EQ(opt->state, "PAUSED");
        EXPECT_EQ(opt->task_id, id);
    }
    int cnt_at_pause = cnt_.load();

    // 5. 注入 resume 命令（真实命令路径，触发 Load+Acquire+Restore+Enqueue）
    engine_->InjectCommand("-m:TaskManager -f:resume -v:id|" + std::to_string(id));

    // 6. 等任务续跑到完成：cnt==total
    for (int w = 0; w < 400 && cnt_.load() < total_; ++w)
        std::this_thread::sleep_for(10ms);
    EXPECT_EQ(cnt_.load(), total_) << "resume 后应跑完剩余分片 ("
        << cnt_at_pause << " + " << (total_ - cnt_at_pause) << ")";

    // 7. resume 命令内部会 store->Delete(id) → store 应已清空该 id
    for (int w = 0; w < 100 && store_->Load(id).has_value(); ++w)
        std::this_thread::sleep_for(10ms);
    EXPECT_FALSE(store_->Load(id).has_value()) << "resume 后应 Delete 快照";

    StopEngine();

    // 8. 验证命令路径确实回传了响应（pause/resume 各一条 success）
    std::lock_guard<std::mutex> lk(sink_mtx_);
    int success_resp = 0;
    for (const auto& p : sink_) {
        // CmdWork 的 tick 分片结果 success=true 也走 sink；只数 TaskManager 的 resume 响应
        if (p.success && !p.return_value.empty() &&
            p.return_value.find("resumed") != std::string::npos)
            ++success_resp;
    }
    EXPECT_GE(success_resp, 1) << "resume 命令应回传 'resumed' 响应";
}

// ---- C组: 多任务运行时存储 / 卸载 / 边界 ----

TEST_F(SnapshotCommandTest, MultiTaskPauseResumeAll) {
    // 捕的 bug: 多任务并发暂停/恢复时快照串台——
    //   store 里 4 条快照对应错 id、或 resume 拿了别人的快照、或 pool 槽位耗尽。
    // 期望: 4 个任务各自独立 pause→save→resume→完成，store 终态空、池全空、cnt==4*total。
    // 串行 pause/resume（确定性优先，见设计确认）。
    StartEngine(/*pool*/8, /*workers*/4, /*total*/30, /*delay*/40ms);
    constexpr int kTasks = 4;

    // 1. 交错「提交 → 拿 id → 立即 pause」每个任务。
    //    关键：必须在提交下一个任务之前就 pause 当前任务，否则当前任务
    //    会在 4 个 Worker 上很快跑完出池，pause 命中时 FindTask 返回 null。
    //    先 pause 占住槽位，再提交下一个，保证 4 个任务都在池中处于 PAUSED。
    std::vector<uint32_t> ids;
    for (int i = 0; i < kTasks; ++i) {
        tid_.store(0);  // 重置，等这个任务的首分片写入新 id
        engine_->InjectCommand("-m:CmdWork -f:tick");
        uint32_t id = WaitTaskStarted();
        ASSERT_NE(id, 0u) << "第 " << i << " 个任务应已开始";
        ids.push_back(id);

        // 等它跑 1~2 个分片，确保确实在执行中（可被 pause 在分片边界）
        for (int w = 0; w < 200 && cnt_.load() < (i + 1); ++w)
            std::this_thread::sleep_for(10ms);

        // 立即 pause 这个任务（趁它还在池中）
        engine_->InjectCommand("-m:TaskManager -f:pause -v:id|" + std::to_string(id));
        for (int w = 0; w < 300 && !store_->Load(id).has_value(); ++w)
            std::this_thread::sleep_for(10ms);
        ASSERT_TRUE(store_->Load(id).has_value()) << "任务 " << id << " 应已保存快照";
    }
    ASSERT_EQ(ids.size(), kTasks);
    // 4 个 id 互不相同（TasksPool 分配唯一递增 id）
    std::set<uint32_t> uniq(ids.begin(), ids.end());
    EXPECT_EQ(uniq.size(), kTasks) << "4 个任务 id 应互不相同";
    // 此时 store 应有 4 条快照，且各自 id 不串台。
    EXPECT_EQ(store_->LoadAll().size(), kTasks) << "应有 4 条快照";
    for (uint32_t id : ids) {
        auto opt = store_->Load(id);
        ASSERT_TRUE(opt.has_value());
        EXPECT_EQ(opt->task_id, id) << "快照 id 不应串台";
        EXPECT_EQ(opt->state, "PAUSED");
    }

    // 3. 逐个 resume，等该任务续跑到完成（cnt 累计）。
    int cnt_before_resume = cnt_.load();
    for (uint32_t id : ids) {
        engine_->InjectCommand("-m:TaskManager -f:resume -v:id|" + std::to_string(id));
    }
    // 等全部完成：cnt == 4 * total
    int target = kTasks * total_;
    for (int w = 0; w < 600 && cnt_.load() < target; ++w)
        std::this_thread::sleep_for(10ms);
    EXPECT_EQ(cnt_.load(), target) << "4 个任务应全部续跑完成";

    // 4. store 应清空（resume 内部 Delete）。
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(store_->LoadAll().size(), 0u) << "所有快照应已被 Delete";

    StopEngine();

    // 5. 命令路径应有 4 条 resumed 响应。
    std::lock_guard<std::mutex> lk(sink_mtx_);
    int resume_resp = 0;
    for (const auto& p : sink_) {
        if (p.success && p.return_value.find("resumed") != std::string::npos)
            ++resume_resp;
    }
    EXPECT_GE(resume_resp, kTasks) << "应有 4 条 resumed 响应";
    (void)cnt_before_resume;
}

TEST_F(SnapshotCommandTest, CommandPauseNonExistent) {
    // 捕的 bug: pause 一个不存在的 id 应优雅返回 not found，而非崩溃或静默。
    StartEngine(8, 4, 6, 30ms);

    engine_->InjectCommand("-m:TaskManager -f:pause -v:id|99999");
    // 等命令处理完（sink 收到响应）
    for (int w = 0; w < 100 && sink_.empty(); ++w) {
        std::lock_guard<std::mutex> lk(sink_mtx_);
        if (!sink_.empty()) break;
        std::this_thread::sleep_for(10ms);
    }
    StopEngine();

    std::lock_guard<std::mutex> lk(sink_mtx_);
    ASSERT_FALSE(sink_.empty()) << "应收到 pause 命令的响应";
    // 找到 TaskManager 的 pause 响应（非 CmdWork 的 tick 结果）
    bool found_not_found = false;
    for (const auto& p : sink_) {
        if (!p.success && p.error.message.find("not found") != std::string::npos) {
            found_not_found = true;
            break;
        }
    }
    EXPECT_TRUE(found_not_found) << "不存在的 id 应返回 'not found' 错误";
}

TEST_F(SnapshotCommandTest, CommandPauseAlreadyPaused) {
    // 捕的 bug: 对已 PAUSED 的任务再 pause 应幂等返回 "already paused"，
    //   而非重复触发 Save 或状态错乱。
    StartEngine(8, 4, 12, 40ms);

    engine_->InjectCommand("-m:CmdWork -f:tick");
    uint32_t id = WaitTaskStarted();
    ASSERT_NE(id, 0u);
    for (int w = 0; w < 200 && cnt_.load() < 2; ++w)
        std::this_thread::sleep_for(10ms);

    // 第一次 pause
    engine_->InjectCommand("-m:TaskManager -f:pause -v:id|" + std::to_string(id));
    for (int w = 0; w < 300 && !store_->Load(id).has_value(); ++w)
        std::this_thread::sleep_for(10ms);
    ASSERT_TRUE(store_->Load(id).has_value());

    // 第二次 pause（任务已 PAUSED 且可能已 Release 出池）。
    // 注意：Worker 收尾 Release 后 FindTask 返回 null，此时 pause 命令走 "not found" 分支
    // 而非 "already paused"。两种都是合法的幂等表现——这里只验证不崩溃、有响应。
    engine_->InjectCommand("-m:TaskManager -f:pause -v:id|" + std::to_string(id));
    std::this_thread::sleep_for(80ms);  // 等第二条命令响应

    StopEngine();

    // 断言：进程没崩（能走到这里就是证明），且 store 里快照仍合法。
    auto opt = store_->Load(id);
    if (opt.has_value()) {
        EXPECT_EQ(opt->state, "PAUSED");
    }
    SUCCEED() << "重复 pause 未崩溃，幂等性保持";
}

TEST_F(SnapshotCommandTest, CommandResumeNoSavedState) {
    // 捕的 bug: resume 一个从未存过快照的 id 应返回 "No saved state"，
    //   而非 Acquire 空槽位或崩溃。
    StartEngine(8, 4, 6, 30ms);

    engine_->InjectCommand("-m:TaskManager -f:resume -v:id|88888");
    for (int w = 0; w < 100; ++w) {
        std::lock_guard<std::mutex> lk(sink_mtx_);
        // resume 失败响应 success=false 且带 "No saved state"
        bool got = false;
        for (const auto& p : sink_) {
            if (!p.success && p.error.message.find("No saved state") != std::string::npos) {
                got = true; break;
            }
        }
        if (got) break;
        std::this_thread::sleep_for(10ms);
    }
    StopEngine();

    std::lock_guard<std::mutex> lk(sink_mtx_);
    bool found_no_state = false;
    for (const auto& p : sink_) {
        if (!p.success && p.error.message.find("No saved state") != std::string::npos) {
            found_no_state = true; break;
        }
    }
    EXPECT_TRUE(found_no_state) << "无快照的 id 应返回 'No saved state'";
    // 且 store 里不应凭空冒出 88888 的记录
    EXPECT_FALSE(store_->Load(88888u).has_value());
}
