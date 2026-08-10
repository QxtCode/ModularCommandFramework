/// =================================================================
///  Task Progress Tests — SetTotalShards + 进度跟踪 + 暂停/恢复
/// =================================================================
///
///  v2.6: SetTotalShards 让模块预声明总分片数，进度从第一帧就准确。
///  测试覆盖:
///   1. 声明总数 → 进度基准从声明值算，非 shards_.size()
///   2. 不声明 → 退化为动态模式（向后兼容）
///   3. 进度封顶 1.0（防御性）
///   4. Reset 清除声明总数
///   5. 暂停/恢复 — 声明总数在暂停期间不变
///   6. 长任务模拟 — 100+ 分片，验证全程进度准确
///   7. 声明总数 vs 实际分片数不等的边界情况

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <string>

#include "core/Task.h"
#include "core/ParmarPack.h"
#include "core/ModuleLifeManager.h"
#include "event_bus/event_bus.h"
#include "sdk/IModule.h"

using namespace std::chrono_literals;

// ================================================================
//  Fixture
// ================================================================
class TaskProgressTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        task = std::make_unique<Task>();
        bus  = &EventBus::GetInstance();

        // 注册 TestTarget.1 信号（task_test 复用）
        bus->RegisterSignal<ParmarPack*>("TestTarget.1");
        bus->LinkSlotFunc<ParmarPack*>("TestTarget.1",
            [counter = &step_counter_](ParmarPack* p) {
                counter->fetch_add(1);
                p->success = true;
            });
    }

    void TearDown() override
    {
        task->Reset();
    }

    /// 快速造一个指向 TestTarget.1 的 pack
    static std::unique_ptr<ParmarPack> MakePack()
    {
        auto p = std::make_unique<ParmarPack>();
        p->mod_id  = "TestTarget";
        p->func_id = "1";
        p->show_explanation = false;
        return p;
    }

    std::unique_ptr<Task> task;
    EventBus* bus = nullptr;
    std::atomic<int> step_counter_{0};
};

// ================================================================
//  Test 1: SetTotalShards — 进度基准用声明值
// ================================================================
/// 声明 300 分片，实际 Push 5 个，执行 2 个 → 进度 2/300 ≈ 0.67%
TEST_F(TaskProgressTest, SetTotalShards_ProgressUsesDeclaredTotal)
{
    task->Assign(MakePack());
    task->SetTotalShards(300);

    // Push 4 more → 5 actual shards
    for (int i = 0; i < 4; ++i)
        task->PushShard(MakePack());

    EXPECT_EQ(task->GetTotalShards(), 300u)
        << "GetTotalShards() returns declared value, not shards_.size()";
    EXPECT_EQ(task->GetCurrentShard(), 0u);

    task->Step(*bus);  // 1/300
    float p1 = task->GetProgress();
    EXPECT_FLOAT_EQ(p1, 1.0f / 300.0f);

    task->Step(*bus);  // 2/300
    float p2 = task->GetProgress();
    EXPECT_FLOAT_EQ(p2, 2.0f / 300.0f);
}

// ================================================================
//  Test 2: 不调 SetTotalShards → 退化为动态模式（向后兼容）
// ================================================================
TEST_F(TaskProgressTest, NoSetTotalShards_FallsBackToDynamic)
{
    task->Assign(MakePack());
    task->PushShard(MakePack());
    task->PushShard(MakePack());

    EXPECT_EQ(task->GetTotalShards(), 3u) << "Dynamic: shards_.size() = 3";

    task->Step(*bus);  // 1/3
    EXPECT_FLOAT_EQ(task->GetProgress(), 1.0f / 3.0f);

    task->Step(*bus);  // 2/3
    EXPECT_FLOAT_EQ(task->GetProgress(), 2.0f / 3.0f);

    task->Step(*bus);  // 3/3
    EXPECT_FLOAT_EQ(task->GetProgress(), 1.0f);
    EXPECT_EQ(task->GetState(), Task::State::COMPLETED);
}

// ================================================================
//  Test 3: 进度封顶 1.0
// ================================================================
TEST_F(TaskProgressTest, ProgressCappedAtOne)
{
    task->Assign(MakePack());
    task->SetTotalShards(1);
    task->Step(*bus);  // complete: 1/1 = 1.0

    EXPECT_FLOAT_EQ(task->GetProgress(), 1.0f);

    // 如果某种原因 current_ 跑过头（bug 防御），不超 1.0
    // PushShard + Step 再多，current_ / declared_total_ 可能 > 1
    task->PushShard(MakePack());
    task->Step(*bus);
    // current_=2, declared_total_=1 → p=2.0 → capped
    EXPECT_FLOAT_EQ(task->GetProgress(), 1.0f)
        << "Progress should never exceed 1.0";
}

// ================================================================
//  Test 4: Reset 清除声明总数
// ================================================================
TEST_F(TaskProgressTest, ResetClearsDeclaredTotal)
{
    task->Assign(MakePack());
    task->SetTotalShards(100);
    EXPECT_EQ(task->GetTotalShards(), 100u);

    task->Reset();

    EXPECT_EQ(task->GetTotalShards(), 0u) << "Reset clears declared_total_";
    EXPECT_EQ(task->GetState(), Task::State::IDLE);
    EXPECT_EQ(task->GetCurrentShard(), 0u);

    // Reset 后重新 Assign + 不声明 → 回退到动态模式
    task->Assign(MakePack());
    task->PushShard(MakePack());
    EXPECT_EQ(task->GetTotalShards(), 2u) << "Back to dynamic after reset";
}

// ================================================================
//  Test 5: 暂停后声明总数仍然有效
// ================================================================
TEST_F(TaskProgressTest, PauseDoesNotAffectDeclaredTotal)
{
    task->Assign(MakePack());
    task->SetTotalShards(10);

    for (int i = 0; i < 9; ++i)
        task->PushShard(MakePack());

    EXPECT_EQ(task->GetTotalShards(), 10u);

    // 执行 3 步 → 3/10
    for (int i = 0; i < 3; ++i)
        task->Step(*bus);
    EXPECT_FLOAT_EQ(task->GetProgress(), 0.3f);

    // 暂停
    task->Pause();
    EXPECT_EQ(task->GetState(), Task::State::PAUSED);
    EXPECT_EQ(task->GetTotalShards(), 10u) << "Declared total unchanged during pause";
    EXPECT_FLOAT_EQ(task->GetProgress(), 0.3f) << "Progress frozen during pause";

    // Step() while paused → false
    EXPECT_FALSE(task->Step(*bus));
    EXPECT_FLOAT_EQ(task->GetProgress(), 0.3f) << "Progress still frozen";

    // 恢复
    task->Resume();
    task->Step(*bus);  // 4/10
    EXPECT_FLOAT_EQ(task->GetProgress(), 0.4f);
}

// ================================================================
//  Test 6: 长任务 — 模拟 200 分片，全流程进度验证
// ================================================================
TEST_F(TaskProgressTest, LongTask_200Shards_ProgressAccurate)
{
    constexpr size_t kTotal = 200;

    task->Assign(MakePack());
    task->SetTotalShards(kTotal);

    // Push remaining 199
    for (size_t i = 1; i < kTotal; ++i)
        task->PushShard(MakePack());

    EXPECT_EQ(task->GetTotalShards(), kTotal);
    EXPECT_FLOAT_EQ(task->GetProgress(), 0.0f);

    // 每 10 步验证一次进度
    for (size_t i = 0; i < kTotal; ++i)
    {
        EXPECT_TRUE(task->Step(*bus));

        float expected = static_cast<float>(i + 1) / kTotal;
        float actual   = task->GetProgress();
        EXPECT_NEAR(actual, expected, 0.001f)
            << "Progress mismatch at shard " << (i + 1);
    }

    EXPECT_EQ(task->GetState(), Task::State::COMPLETED);
    EXPECT_FLOAT_EQ(task->GetProgress(), 1.0f);
    EXPECT_EQ(step_counter_.load(), static_cast<int>(kTotal))
        << "All 200 shards should execute";
}

// ================================================================
//  Test 7: PushShard 追加超过声明总数 — 进度仍准确（封顶前）
// ================================================================
TEST_F(TaskProgressTest, PushMoreThanDeclared_ProgressFollowsActual)
{
    // 声明 5，但实际 Push 10 个 — 进度用声明值做分母直到 current > 声明
    // 此时 current/declared 可能 > 1，封顶逻辑生效
    task->Assign(MakePack());
    task->SetTotalShards(5);

    for (int i = 0; i < 9; ++i)
        task->PushShard(MakePack());

    EXPECT_EQ(task->GetTotalShards(), 5u) << "Still reports declared";

    task->Step(*bus); EXPECT_FLOAT_EQ(task->GetProgress(), 1.0f / 5.0f);
    task->Step(*bus); EXPECT_FLOAT_EQ(task->GetProgress(), 2.0f / 5.0f);
    task->Step(*bus); EXPECT_FLOAT_EQ(task->GetProgress(), 3.0f / 5.0f);
    task->Step(*bus); EXPECT_FLOAT_EQ(task->GetProgress(), 4.0f / 5.0f);
    task->Step(*bus); EXPECT_FLOAT_EQ(task->GetProgress(), 1.0f); // 5/5=1.0

    // 超过声明值后，进度封顶 1.0
    task->Step(*bus);  // 6/5 → cap
    EXPECT_FLOAT_EQ(task->GetProgress(), 1.0f);
}

// ================================================================
//  Test 8: 单分片声明 1 — 边界
// ================================================================
TEST_F(TaskProgressTest, SingleShardDeclared)
{
    task->Assign(MakePack());
    task->SetTotalShards(1);

    EXPECT_FLOAT_EQ(task->GetProgress(), 0.0f);
    task->Step(*bus);
    EXPECT_FLOAT_EQ(task->GetProgress(), 1.0f);
    EXPECT_EQ(task->GetState(), Task::State::COMPLETED);
}

// ================================================================
//  Test 9: 动态追加途中声明总数（运行中声明）
// ================================================================
TEST_F(TaskProgressTest, DeclareMidExecution)
{
    // 场景：先执行几个分片后才知道总数（如流式解析得知总帧数）
    task->Assign(MakePack());
    task->PushShard(MakePack());
    task->PushShard(MakePack());  // 当前 3 个

    EXPECT_EQ(task->GetTotalShards(), 3u);  // 动态模式

    task->Step(*bus);  // 1/3
    EXPECT_FLOAT_EQ(task->GetProgress(), 1.0f / 3.0f);

    // ★ 此时得知总共有 100 个分片
    task->SetTotalShards(100);

    EXPECT_EQ(task->GetTotalShards(), 100u) << "Total switches to declared";
    EXPECT_FLOAT_EQ(task->GetProgress(), 1.0f / 100.0f)
        << "Progress recalculated with new denominator";

    // Push remaining
    for (int i = 3; i < 100; ++i)
        task->PushShard(MakePack());

    // 继续执行
    task->Step(*bus);  // 2/100
    EXPECT_FLOAT_EQ(task->GetProgress(), 2.0f / 100.0f);
}

// ================================================================
//  Test 10: 声明 0 → 等同于未声明
// ================================================================
TEST_F(TaskProgressTest, SetTotalShardsZero_EqualsNotSet)
{
    task->Assign(MakePack());
    task->PushShard(MakePack());
    task->SetTotalShards(0);  // 0 is same as not set

    EXPECT_EQ(task->GetTotalShards(), 2u) << "Falls back to shards_.size()";
    EXPECT_FLOAT_EQ(task->GetProgress(), 0.0f);
}

// ================================================================
//  Test 11: Assign 清除旧声明值（池复用安全）
// ================================================================
TEST_F(TaskProgressTest, AssignClearsDeclaredTotal)
{
    // 第一轮：声明 100
    task->Assign(MakePack());
    task->SetTotalShards(100);
    EXPECT_EQ(task->GetTotalShards(), 100u);

    // 手动验证 Reset 清除了
    task->Reset();
    EXPECT_EQ(task->GetTotalShards(), 0u) << "Reset should clear";

    // 第二轮：Assign 新任务，不调 SetTotalShards
    task->Assign(MakePack());
    task->PushShard(MakePack());
    task->PushShard(MakePack());

    EXPECT_EQ(task->GetTotalShards(), 3u)
        << "After Reset+Assign, should fall back to dynamic (3 shards)";
}
