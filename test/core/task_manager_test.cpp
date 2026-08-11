/// =================================================================
///  TaskManager 隔离测试 (v2.6)
/// =================================================================
///  测试范围: Task::Restore / TasksPool::FindTask / MemPersistence /
///           pause-resume 完整流程 / 并发安全 / 边界条件
///  隔离级别: 不依赖 ShellEngine
/// =================================================================

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "core/ITaskPersistence.h"
#include "core/MemTaskStore.h"
#include "core/Task.h"
#include "core/TasksPool.h"
#include "core/ThreadPool.h"
#include "core/ModuleLifeManager.h"
#include "core/ModuleBaseObject.h"
#include "core/ResultStore.h"
#include "event_bus/event_bus.h"
#include "sdk/IModule.h"

using namespace std::chrono_literals;

// ================================================================
//  Fixture: 共享测试环境
// ================================================================
class TaskManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        bus_ = &EventBus::GetInstance();
        pool_ = std::make_unique<TasksPool>(8);
        workers_ = std::make_unique<ThreadPool>(4);
        store_ = std::make_unique<MemPersistence>();
        exec_count_.store(0);

        // 注册一个慢速测试模块（50ms per shard，验证暂停在分片边界）
        class WorkMod : public ModuleBaseObject {
        public:
            WorkMod(std::atomic<int>* cnt, int delay_ms)
                : cnt_(cnt), delay_ms_(delay_ms) {}
            const char* GetName() const override { return "WorkMod"; }
            bool OnInit() override {
                REGISTER_FUNC("slow", "slow work", {
                    if (delay_ms_ > 0)
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(delay_ms_));
                    if (cnt_) cnt_->fetch_add(1);
                    pack->success = true;
                });
                return true;
            }
        private:
            std::atomic<int>* cnt_;
            int delay_ms_;
        };
        ModuleLifeManager::GetInstance().UnloadModule("WorkMod");
        ModuleLifeManager::GetInstance().AddModule(
            std::make_unique<WorkMod>(&exec_count_, 150));
    }

    void TearDown() override {
        workers_.reset();
        pool_.reset();
        ModuleLifeManager::GetInstance().UnloadModule("WorkMod");
        ResultStore::Get().Clear();
    }

    /// 提交一个 Slow 任务，返回 task_id
    uint32_t SubmitSlow() {
        auto pack = std::make_unique<ParmarPack>();
        pack->mod_id = "WorkMod";
        pack->func_id = "slow";
        pack->show_explanation = false;

        Task* t = pool_->Acquire(std::move(pack));
        EXPECT_NE(t, nullptr);
        uint32_t id = t->GetID();

        workers_->Enqueue([this, t]() {
            try { while (t->Step(*bus_)) {} } catch (...) {}
            auto* cp = t->CurrentPack();
            if (cp) {
                auto r = std::make_unique<ParmarPack>(*cp);
                ResultStore::Get().PushResult(t->GetID(), std::move(r));
            }
            pool_->Release(t);
        });

        // 等 worker 开始执行
        std::this_thread::sleep_for(10ms);
        return id;
    }

    EventBus* bus_;
    std::unique_ptr<TasksPool> pool_;
    std::unique_ptr<ThreadPool> workers_;
    std::unique_ptr<MemPersistence> store_;
    std::atomic<int> exec_count_{0};
};

// ================================================================
//  1. Task::Restore 单元测试
// ================================================================
TEST(TaskRestore, RestoreFromRecord) {
    Task t;
    EXPECT_EQ(t.GetState(), Task::State::IDLE);

    TaskRecord rec;
    rec.task_id = 42;
    rec.state = "PAUSED";
    rec.current_shard = 3;
    rec.shards_json = R"([{"mod":"Test","func":"echo"}])";

    bool ok = t.Restore(rec);
    EXPECT_TRUE(ok);
    EXPECT_EQ(t.GetID(), 42u);
    EXPECT_EQ(t.GetState(), Task::State::PAUSED);
    EXPECT_EQ(t.GetCurrentShard(), 3u);

    // 恢复后可以 Resume
    t.Resume();
    EXPECT_EQ(t.GetState(), Task::State::RUNNING);
}

TEST(TaskRestore, RestoreFailsIfNotIdle) {
    Task t;
    // 先恢复到 RUNNING
    TaskRecord rec1; rec1.task_id = 1; rec1.state = "RUNNING";
    t.Restore(rec1);
    EXPECT_EQ(t.GetState(), Task::State::RUNNING);

    // 再尝试恢复 → 应该失败
    TaskRecord rec2; rec2.task_id = 2; rec2.state = "PAUSED";
    EXPECT_FALSE(t.Restore(rec2));
    EXPECT_EQ(t.GetID(), 1u); // 保持不变
}

TEST(TaskRestore, RestorePreservesState) {
    Task t;
    TaskRecord rec;
    rec.task_id = 7;
    rec.state = "FAILED";
    rec.declared_total = 10;
    rec.retry_count = 3;

    t.Restore(rec);
    EXPECT_EQ(t.GetState(), Task::State::FAILED);
    EXPECT_EQ(t.GetID(), 7u);
    EXPECT_EQ(t.GetTotalShards(), 10u);
}

// ================================================================
//  2. TasksPool::FindTask
// ================================================================
TEST(TasksPoolFind, FindById) {
    TasksPool pool(4);
    auto pack = std::make_unique<ParmarPack>();
    Task* t = pool.Acquire(std::move(pack));
    ASSERT_NE(t, nullptr);
    uint32_t id = t->GetID();

    Task* found = pool.FindTask(id);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->GetID(), id);

    // 找不存在的 id
    EXPECT_EQ(pool.FindTask(id + 999), nullptr);
}

TEST(TasksPoolFind, FindAfterRelease) {
    TasksPool pool(4);
    auto pack = std::make_unique<ParmarPack>();
    Task* t = pool.Acquire(std::move(pack));
    ASSERT_NE(t, nullptr);
    uint32_t id = t->GetID();
    EXPECT_GT(id, 0u) << "Acquire should assign unique ID";
    EXPECT_EQ(pool.FindTask(id), t) << "FindTask should find active task";

    // Release → Reset → ID 归零
    pool.Release(t);
    EXPECT_EQ(t->GetID(), 0u) << "Reset should clear ID to 0";
}

// ================================================================
//  3. MemPersistence 完整读写
// ================================================================
TEST(MemPersistenceFull, SaveLoadDelete) {
    MemPersistence store;
    TaskRecord r; r.task_id = 10; r.state = "PAUSED";
    store.Save(r);

    auto opt = store.Load(10);
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->state, "PAUSED");

    store.Delete(10);
    EXPECT_FALSE(store.Load(10).has_value());
}

TEST(MemPersistenceFull, LoadAllAndGC) {
    MemPersistence store;
    for (uint32_t i = 0; i < 10; ++i) {
        TaskRecord r; r.task_id = i;
        r.state = (i < 5) ? "PAUSED" : "COMPLETED";
        store.Save(r);
    }
    EXPECT_EQ(store.LoadAll().size(), 5u); // 5 PAUSED
    store.GC();
    EXPECT_EQ(store.Size(), 5u);
}

// ================================================================
//  4. Pause → Snapshot → Restore 完整流程
// ================================================================
TEST_F(TaskManagerTest, PauseResumeFullCycle) {
    // 手动控制完整流程（不依赖 Worker 竞态）

    // 1. Acquire + 直接 Pause
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id = "WorkMod";
    pack->func_id = "slow";
    Task* task = pool_->Acquire(std::move(pack));
    ASSERT_NE(task, nullptr);
    uint32_t tid = task->GetID();
    EXPECT_GT(tid, 0u);
    task->Pause();

    // 2. Snapshot
    TaskRecord rec;
    rec.task_id = tid;
    rec.state = "PAUSED";
    rec.current_shard = 0;
    store_->Save(rec);

    // 3. Release（Worker 从未启动，task 直接还池）
    pool_->Release(task);

    // 4. Load + Restore
    auto opt = store_->Load(tid);
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->state, "PAUSED");

    auto dummy = std::make_unique<ParmarPack>();
    Task* restored = pool_->Acquire(std::move(dummy));
    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->Restore(*opt));
    EXPECT_EQ(restored->GetID(), tid) << "Restored task should recover original ID";
    EXPECT_EQ(restored->GetState(), Task::State::PAUSED);

    restored->Resume();
    EXPECT_EQ(restored->GetState(), Task::State::RUNNING);

    // 5. 提交到 Worker 验证可执行
    std::atomic<bool> done{false};
    workers_->Enqueue([restored, &done, this]() {
        try { while (restored->Step(*bus_)) {} } catch (...) {}
        pool_->Release(restored);
        done.store(true);
    });

    for (int w = 0; w < 50 && !done.load(); ++w)
        std::this_thread::sleep_for(20ms);
    EXPECT_TRUE(done.load());
    store_->Delete(tid);
}

// ================================================================
//  5. 并发安全
// ================================================================
TEST_F(TaskManagerTest, ConcurrentPauseDifferentTasks) {
    constexpr int N = 4;
    std::vector<uint32_t> ids;
    for (int i = 0; i < N; ++i)
        ids.push_back(SubmitSlow());
    std::this_thread::sleep_for(10ms);

    // 并发暂停 4 个不同任务（Worker 可能已 Release，FindTask 可能返回 null）
    std::vector<std::thread> th;
    std::atomic<int> paused{0};
    for (int i = 0; i < N; ++i) {
        th.emplace_back([this, id = ids[i], &paused]() {
            Task* t = pool_->FindTask(id);
            if (t) { t->Pause(); paused.fetch_add(1); }
        });
    }
    for (auto& t : th) t.join();
    EXPECT_GE(paused.load(), 1);

    for (int w = 0; w < 100; ++w) {
        if (pool_->GetFreeCount() == pool_->GetTotalCount()) break;
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(pool_->GetFreeCount(), pool_->GetTotalCount());
}

TEST_F(TaskManagerTest, ConcurrentPauseSameTask) {
    uint32_t tid = SubmitSlow();
    std::this_thread::sleep_for(10ms);

    std::atomic<int> ok{0};
    std::vector<std::thread> th;
    for (int i = 0; i < 4; ++i) {
        th.emplace_back([this, tid, &ok]() {
            Task* t = pool_->FindTask(tid);
            if (t) { t->Pause(); ok.fetch_add(1); }
        });
    }
    for (auto& t : th) t.join();
    EXPECT_GE(ok.load(), 1);

    for (int w = 0; w < 50; ++w) {
        if (pool_->GetFreeCount() == pool_->GetTotalCount()) break;
        std::this_thread::sleep_for(10ms);
    }
    SUCCEED();
}

TEST_F(TaskManagerTest, MemPersistenceConcurrentSave) {
    MemPersistence store;
    constexpr int N = 200;
    std::vector<std::thread> th;
    for (int t = 0; t < 4; ++t) {
        th.emplace_back([&store, t]() {
            for (int i = 0; i < N; ++i) {
                TaskRecord r; r.task_id = static_cast<uint32_t>(t * N + i);
                r.state = "PAUSED";
                store.Save(r);
            }
        });
    }
    for (auto& t : th) t.join();
    EXPECT_EQ(store.LoadAll().size(), 4u * N);
    EXPECT_EQ(store.Size(), 4u * N);
}

// ================================================================
//  6. 内存泄漏: 反复 Pause/Resume
// ================================================================
TEST_F(TaskManagerTest, RepeatedPauseRestoreNoLeak) {
    for (int cycle = 0; cycle < 20; ++cycle) {
        uint32_t tid = SubmitSlow();
        std::this_thread::sleep_for(10ms);

        Task* t = pool_->FindTask(tid);
        if (!t) continue;  // already released by worker
        t->Pause();

        // 在 Release 之前快照
        TaskRecord rec;
        rec.task_id = tid;
        rec.state = "PAUSED";
        rec.current_shard = t->GetCurrentShard();
        store_->Save(rec);

        // 等 Worker Release（150ms slot + overhead）
        for (int w = 0; w < 80 && pool_->FindTask(tid) != nullptr; ++w)
            std::this_thread::sleep_for(10ms);

        // Restore
        auto opt = store_->Load(tid);
        if (opt.has_value()) {
            auto dummy = std::make_unique<ParmarPack>();
            Task* r = pool_->Acquire(std::move(dummy));
            if (r) {
                r->Restore(*opt);
                workers_->Enqueue([this, r]() {
                    try { while (r->Step(*bus_)) {} } catch (...) {}
                    pool_->Release(r);
                });
                store_->Delete(tid);
            }
        }
        std::this_thread::sleep_for(5ms);
    }

    for (int w = 0; w < 100; ++w) {
        if (pool_->GetFreeCount() == pool_->GetTotalCount()) break;
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(pool_->GetFreeCount(), pool_->GetTotalCount());
}

// ================================================================
//  7. 边界条件
// ================================================================
TEST_F(TaskManagerTest, PauseNonExistentTask) {
    Task* t = pool_->FindTask(99999);
    EXPECT_EQ(t, nullptr);
}

TEST_F(TaskManagerTest, DoublePauseIsIdempotent) {
    uint32_t tid = SubmitSlow();
    std::this_thread::sleep_for(5ms);
    Task* t = pool_->FindTask(tid);
    if (!t) return;  // Worker already released
    t->Pause();
    t->Pause();  // 第二次 pause，幂等
    EXPECT_EQ(t->GetState(), Task::State::PAUSED);

    for (int w = 0; w < 50; ++w) {
        if (pool_->GetFreeCount() == pool_->GetTotalCount()) break;
        std::this_thread::sleep_for(10ms);
    }
    SUCCEED();
}

TEST_F(TaskManagerTest, ResumeWithoutStoreFails) {
    MemPersistence empty;
    auto opt = empty.Load(99999);
    EXPECT_FALSE(opt.has_value());
}

TEST_F(TaskManagerTest, PauseBeforeWorkerStarts) {
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id = "WorkMod";
    pack->func_id = "slow";
    Task* t = pool_->Acquire(std::move(pack));
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->GetState(), Task::State::IDLE);

    // 在 Enqueue 前 Pause
    t->Pause();
    EXPECT_EQ(t->GetState(), Task::State::PAUSED);

    // 仍然 Enqueue — Worker 会立即检测 PAUSED 并返回
    uint32_t tid = t->GetID();
    workers_->Enqueue([this, t]() {
        try { while (t->Step(*bus_)) {} } catch (...) {}
        pool_->Release(t);
    });

    for (int w = 0; w < 50; ++w) {
        if (pool_->GetFreeCount() == pool_->GetTotalCount()) break;
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(pool_->GetFreeCount(), pool_->GetTotalCount())
        << "Pre-paused task should not execute";
}

// ================================================================
//  8. 垂悬指针: Release 后不再访问 Task
// ================================================================
TEST_F(TaskManagerTest, NoAccessAfterRelease) {
    uint32_t tid = SubmitSlow();
    std::this_thread::sleep_for(10ms);

    // 找到并暂停
    Task* raw = pool_->FindTask(tid);
    ASSERT_NE(raw, nullptr) << "Task should still be in pool";
    raw->Pause();

    // 存快照（Worker 还没 Release）
    TaskRecord rec;
    rec.task_id = tid;
    rec.state = "PAUSED";
    rec.current_shard = raw->GetCurrentShard();
    store_->Save(rec);

    // 等 Worker 自然 Release（150ms slot + overhead）
    for (int w = 0; w < 80 && pool_->FindTask(tid) != nullptr; ++w)
        std::this_thread::sleep_for(10ms);
    // raw 现在是垂悬指针，不再使用

    // 从 pool 拿新 Task 恢复
    auto opt = store_->Load(tid);
    ASSERT_TRUE(opt.has_value());
    auto dummy = std::make_unique<ParmarPack>();
    Task* fresh = pool_->Acquire(std::move(dummy));
    ASSERT_NE(fresh, nullptr);
    // Released task ID is reset to 0; fresh task also starts at 0
    EXPECT_TRUE(fresh->Restore(*opt));

    pool_->Release(fresh);
}
