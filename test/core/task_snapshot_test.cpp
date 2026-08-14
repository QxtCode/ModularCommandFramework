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
