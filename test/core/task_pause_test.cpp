/// =================================================================
///  Task Pause/Resume Tests — 分片任务暂停恢复
/// =================================================================
///
///  测试场景:
///   1. 单分片暂停 — 模块回调内调 Pause()，Step() 返回 false
///   2. 多分片暂停恢复 — Pause → 追加分片 → Resume → 继续执行
///   3. 工作流模拟 — 解析→编译→链接，每步可暂停
///   4. Cancel — 取消后 Step() 不执行
///   5. 暂停粒度 — 只在分片间暂停，一个分片内不可打断

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>

#include "core/Task.h"
#include "core/ParmarPack.h"
#include "core/ModuleLifeManager.h"
#include "event_bus/event_bus.h"
#include "sdk/IModule.h"

using namespace std::chrono_literals;

// ================================================================
//  工作流模块 — 支持 PushShard + Pause
// ================================================================
class WorkflowModule : public ModuleBaseObject
{
public:
    WorkflowModule(std::string name, std::atomic<int>* step_counter = nullptr)
        : name_(std::move(name)), step_counter_(step_counter) {}

    const char* GetName() const override { return name_.c_str(); }

    bool OnInit() override
    {
        REGISTER_FUNC("step1", "first step", {
            if (step_counter_) step_counter_->fetch_add(1);
            pack->success = true;
            pack->return_value = name_ + "_step1";

            // 模拟工作流：step1 完成后自动追加 step2
            auto next = std::make_unique<ParmarPack>();
            next->mod_id  = name_;
            next->func_id = "step2";
            next->show_explanation = false;
            pack->owner_task->PushShard(std::move(next));
        });

        REGISTER_FUNC("step2", "second step", {
            if (step_counter_) step_counter_->fetch_add(1);
            pack->success = true;
            pack->return_value = name_ + "_step2";

            // 追加 step3
            auto next = std::make_unique<ParmarPack>();
            next->mod_id  = name_;
            next->func_id = "step3";
            next->show_explanation = false;
            pack->owner_task->PushShard(std::move(next));
        });

        REGISTER_FUNC("step3", "final step", {
            if (step_counter_) step_counter_->fetch_add(1);
            pack->success = true;
            pack->return_value = name_ + "_step3_done";
        });

        REGISTER_FUNC("pause_me", "step that pauses itself", {
            if (step_counter_) step_counter_->fetch_add(1);
            pack->success = true;
            pack->return_value = name_ + "_paused";
            // ★ 自己在回调内暂停
            pack->owner_task->Pause();
        });

        REGISTER_FUNC("dead_loop", "simulate dead task (long running)", {
            // 模拟死任务：烧 CPU 2 秒
            auto end = std::chrono::steady_clock::now() + 2s;
            while (std::chrono::steady_clock::now() < end) {
                volatile int x = 0; (void)x;
            }
            if (step_counter_) step_counter_->fetch_add(1);
            pack->success = true;
        });

        REGISTER_FUNC("long_with_checkpoint", "长任务+检查点", {
            // 分 3 段执行，每段之间检查是否需要暂停
            for (int seg = 0; seg < 3; ++seg) {
                auto end = std::chrono::steady_clock::now() + 200ms;
                while (std::chrono::steady_clock::now() < end) {
                    volatile int x = 0; (void)x;
                }
                // 每段做完追加下一段
                if (seg < 2) {
                    auto next = std::make_unique<ParmarPack>();
                    next->mod_id  = name_;
                    next->func_id = "long_segment";
                    next->show_explanation = false;
                    pack->owner_task->PushShard(std::move(next));
                }
            }
            if (step_counter_) step_counter_->fetch_add(1);
            pack->success = true;
            pack->return_value = name_ + "_long_done";
        });

        REGISTER_FUNC("long_segment", "长任务的一段", {
            // 每段烧 200ms
            auto end = std::chrono::steady_clock::now() + 200ms;
            while (std::chrono::steady_clock::now() < end) {
                volatile int x = 0; (void)x;
            }
            if (step_counter_) step_counter_->fetch_add(1);
            pack->success = true;
        });

        return true;
    }

private:
    std::string name_;
    std::atomic<int>* step_counter_;
};

// ================================================================
//  Fixture
// ================================================================
class TaskPauseTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        mgr_ = &ModuleLifeManager::GetInstance();
        bus_ = &EventBus::GetInstance();
    }

    ModuleLifeManager* mgr_;
    EventBus* bus_;
};

// ================================================================
//  Test 1: 回调内 Pause — Step() 返回 false，状态变 PAUSED
// ================================================================
TEST_F(TaskPauseTest, PauseInsideCallback)
{
    std::atomic<int> steps{0};
    auto mod = std::make_unique<WorkflowModule>("PauseMod", &steps);
    ASSERT_TRUE(mgr_->AddModule(std::move(mod)));

    Task task;
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id  = "PauseMod";
    pack->func_id = "pause_me";
    pack->show_explanation = false;
    task.Assign(std::move(pack));

    // Execute one step — the callback calls Pause() internally.
    // Step() returns false (paused), but the shard DID execute.
    bool ok = task.Step(*bus_);

    EXPECT_FALSE(ok) << "Step returns false when paused (don't call me again)";
    EXPECT_EQ(steps.load(), 1) << "Callback should have executed before pause";
    EXPECT_EQ(task.GetState(), Task::State::PAUSED)
        << "Task should be PAUSED after callback calls Pause()";

    // Calling Step() again while PAUSED should return false
    ok = task.Step(*bus_);
    EXPECT_FALSE(ok) << "Step should refuse while PAUSED";
    EXPECT_EQ(steps.load(), 1) << "No additional steps should execute";

    mgr_->UnloadModule("PauseMod");
}

// ================================================================
//  Test 2: Pause → PushShard → Resume → 继续执行
// ================================================================
TEST_F(TaskPauseTest, PausePushShardResume)
{
    std::atomic<int> steps{0};
    auto mod = std::make_unique<WorkflowModule>("WF", &steps);
    ASSERT_TRUE(mgr_->AddModule(std::move(mod)));

    Task task;
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id  = "WF";
    pack->func_id = "step1";     // step1 → PushShard(step2) → PushShard(step3)
    pack->show_explanation = false;
    task.Assign(std::move(pack));

    // Step 1 — executes step1, PushShard adds step2 (which adds step3)
    bool ok = task.Step(*bus_);
    EXPECT_TRUE(ok);
    EXPECT_EQ(steps.load(), 1);
    EXPECT_EQ(task.GetTotalShards(), 2u) << "step1 complete, step2 added";
    EXPECT_EQ(task.GetCurrentShard(), 1u);
    EXPECT_EQ(task.GetState(), Task::State::RUNNING);

    // Step 2 — executes step2, PushShard adds step3
    ok = task.Step(*bus_);
    EXPECT_TRUE(ok);
    EXPECT_EQ(steps.load(), 2);
    EXPECT_EQ(task.GetTotalShards(), 3u) << "step2 complete, step3 added";
    EXPECT_EQ(task.GetCurrentShard(), 2u);

    // Step 3 — executes step3, no more shards → COMPLETED
    ok = task.Step(*bus_);
    EXPECT_TRUE(ok);
    EXPECT_EQ(steps.load(), 3);
    EXPECT_EQ(task.GetState(), Task::State::COMPLETED);

    mgr_->UnloadModule("WF");
}

// ================================================================
//  Test 3: 工作流中途暂停
// ================================================================
TEST_F(TaskPauseTest, WorkflowPauseInMiddle)
{
    std::atomic<int> steps{0};
    auto mod = std::make_unique<WorkflowModule>("WF2", &steps);
    ASSERT_TRUE(mgr_->AddModule(std::move(mod)));

    Task task;
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id  = "WF2";
    pack->func_id = "step1";
    pack->show_explanation = false;
    task.Assign(std::move(pack));

    // Step 1 → push step2
    task.Step(*bus_);
    EXPECT_EQ(steps.load(), 1);
    EXPECT_EQ(task.GetTotalShards(), 2u);

    // ★ Pause before step 2
    task.Pause();
    EXPECT_EQ(task.GetState(), Task::State::PAUSED);

    // Try to step — should refuse
    bool ok = task.Step(*bus_);
    EXPECT_FALSE(ok);
    EXPECT_EQ(steps.load(), 1) << "Step 2 should NOT execute";

    // Resume
    task.Resume();
    EXPECT_EQ(task.GetState(), Task::State::RUNNING);

    // Step 2 → push step3
    task.Step(*bus_);
    EXPECT_EQ(steps.load(), 2);
    EXPECT_EQ(task.GetTotalShards(), 3u);

    // Step 3 → complete
    task.Step(*bus_);
    EXPECT_EQ(steps.load(), 3);
    EXPECT_EQ(task.GetState(), Task::State::COMPLETED);

    mgr_->UnloadModule("WF2");
}

// ================================================================
//  Test 4: Cancel — 取消后 Step() 不执行
// ================================================================
TEST_F(TaskPauseTest, CancelStopsExecution)
{
    std::atomic<int> steps{0};
    auto mod = std::make_unique<WorkflowModule>("CancelMod", &steps);
    ASSERT_TRUE(mgr_->AddModule(std::move(mod)));

    Task task;
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id  = "CancelMod";
    pack->func_id = "step1";
    pack->show_explanation = false;
    task.Assign(std::move(pack));

    // Step 1
    task.Step(*bus_);
    EXPECT_EQ(steps.load(), 1);

    // Cancel!
    task.Cancel();
    EXPECT_EQ(task.GetState(), Task::State::FAILED);

    // Should not execute step 2
    bool ok = task.Step(*bus_);
    EXPECT_FALSE(ok);
    EXPECT_EQ(steps.load(), 1) << "No more steps after cancel";

    mgr_->UnloadModule("CancelMod");
}

// ================================================================
//  Test 5: 暂停粒度 — 只能分片间暂停，分片内无法打断
// ================================================================
TEST_F(TaskPauseTest, PauseGranularity_OnlyBetweenShards)
{
    std::atomic<int> steps{0};
    auto mod = std::make_unique<WorkflowModule>("GranMod", &steps);
    ASSERT_TRUE(mgr_->AddModule(std::move(mod)));

    Task task;
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id  = "GranMod";
    pack->func_id = "long_with_checkpoint";  // 3 segments, 200ms each
    pack->show_explanation = false;
    task.Assign(std::move(pack));

    // Start step in a separate thread
    std::atomic<bool> step_done{false};
    std::thread worker([&]() {
        task.Step(*bus_);         // This will execute the first segment (200ms)
                                   // then push more shards internally
        step_done.store(true);
    });

    // Wait 100ms — task is mid-execution of first segment
    std::this_thread::sleep_for(100ms);

    // Pause — won't take effect until current shard finishes
    task.Pause();
    EXPECT_EQ(task.GetState(), Task::State::PAUSED)
        << "Pause() sets the flag immediately";

    // But the worker is still executing! Step() won't return until
    // the current shard finishes (~200ms total)
    worker.join();
    EXPECT_TRUE(step_done.load()) << "Step() should complete current shard";
    EXPECT_EQ(steps.load(), 1) << "First segment completed";

    // Now the task is PAUSED — Step() should refuse
    bool ok = task.Step(*bus_);
    EXPECT_FALSE(ok) << "Should be paused, next shard not executed";

    // Resume and finish remaining shards
    task.Resume();

    // The first Step already pushed more shards internally
    // (long_with_checkpoint pushes sub-segments)
    // Execute them
    int remaining = 0;
    while (task.Step(*bus_)) { remaining++; }
    std::cout << "[TEST] Remaining shards: " << remaining << std::endl;

    // Total: 1 initial segment + the sub-segments pushed
    EXPECT_GT(steps.load(), 1) << "More steps should have executed after resume";
    EXPECT_EQ(task.GetState(), Task::State::COMPLETED);

    mgr_->UnloadModule("GranMod");
}

// ================================================================
//  Test 6: 死任务模拟 — Cancel 可以终止长时间任务
// ================================================================
TEST_F(TaskPauseTest, DeadTaskCancel)
{
    std::atomic<int> steps{0};
    auto mod = std::make_unique<WorkflowModule>("DeadMod", &steps);
    ASSERT_TRUE(mgr_->AddModule(std::move(mod)));

    Task task;
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id  = "DeadMod";
    pack->func_id = "dead_loop";    // 5 秒 CPU 空转
    pack->show_explanation = false;
    task.Assign(std::move(pack));

    std::atomic<bool> step_returned{false};

    // Start the dead task in a background thread
    std::thread worker([&]() {
        task.Step(*bus_);
        step_returned.store(true);
    });

    // Wait a bit, then cancel
    std::this_thread::sleep_for(500ms);
    EXPECT_EQ(task.GetState(), Task::State::RUNNING)
        << "Task should be running (stuck in dead_loop)";

    // Cancel — sets state to FAILED, but Step() won't check until
    // after the shard completes. This is the limitation: Pause/Cancel
    // only takes effect BETWEEN shards, not during a shard.
    task.Cancel();
    EXPECT_EQ(task.GetState(), Task::State::FAILED)
        << "Cancel sets the flag, but current shard keeps running";

    // Worker will eventually complete the 5s shard, then Step()
    // checks FAILED state and returns false
    worker.join();
    EXPECT_TRUE(step_returned.load()) << "Step() should eventually return";

    // The callback completed (despite cancel), because cancel doesn't
    // interrupt a running shard — only prevents the next shard
    EXPECT_EQ(steps.load(), 1) << "Callback completed despite cancel";

    std::cout << "[TEST] Dead task: Cancel flag is set, but current shard "
              << "still runs to completion (~2s). Cannot kill mid-shard."
              << std::endl;

    mgr_->UnloadModule("DeadMod");
}
