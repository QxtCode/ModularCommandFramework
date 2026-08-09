/// Task unit tests — EventBus-driven shard pipeline

#include <gtest/gtest.h>
#include "core/Task.h"
#include "core/ParmarPack.h"
#include "event_bus/event_bus.h"

class TaskTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        task = std::make_unique<Task>();
        bus  = &EventBus::GetInstance();

        // 注册一个测试用的 signal
        bus->RegisterSignal<ParmarPack*>("TestTarget.1");
        bus->LinkSlotFunc<ParmarPack*>("TestTarget.1",
            [](ParmarPack* p) { p->success = true; p->return_value = "ok"; });
    }

    void TearDown() override { task.reset(); }

    std::unique_ptr<Task> task;
    EventBus* bus = nullptr;
};

// ---- 初始状态 ----

TEST_F(TaskTest, InitialStateIsIdle)
{
    EXPECT_EQ(task->GetState(), Task::State::IDLE);
    EXPECT_EQ(task->GetTotalShards(), 0u);
    EXPECT_EQ(task->GetCurrentShard(), 0u);
}

// ---- Assign ----

TEST_F(TaskTest, AssignCreatesOneShard)
{
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id = "TestTarget";
    pack->func_id = "1";

    EXPECT_TRUE(task->Assign(std::move(pack)));
    EXPECT_EQ(task->GetTotalShards(), 1u);
    EXPECT_STREQ(task->CurrentPack()->mod_id.c_str(), "TestTarget");
}

TEST_F(TaskTest, AssignNullReturnsFalse)
{
    EXPECT_FALSE(task->Assign(nullptr));
}

// ---- PushShard ----

TEST_F(TaskTest, PushShardAddsToQueue)
{
    auto p1 = std::make_unique<ParmarPack>();
    p1->mod_id = "TestTarget";
    task->Assign(std::move(p1));

    auto p2 = std::make_unique<ParmarPack>();
    p2->mod_id = "TestTarget";
    task->PushShard(std::move(p2));

    EXPECT_EQ(task->GetTotalShards(), 2u);
}

// ---- Step 执行 ----

TEST_F(TaskTest, StepEmitsSignal)
{
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id  = "TestTarget";
    pack->func_id = "1";

    task->Assign(std::move(pack));
    bool ok = task->Step(*bus);

    EXPECT_TRUE(ok);
    EXPECT_EQ(task->GetCurrentShard(), 1u);
}

TEST_F(TaskTest, StepFailsOnUnknownSignal)
{
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id  = "NoSuchSignal";

    task->Assign(std::move(pack));
    bool ok = task->Step(*bus);
    EXPECT_FALSE(ok);
}

// ---- 分片间暂停 ----

TEST_F(TaskTest, PauseBetweenShards)
{
    auto p1 = std::make_unique<ParmarPack>();
    p1->mod_id = "TestTarget"; p1->func_id = "1";
    task->Assign(std::move(p1));

    auto p2 = std::make_unique<ParmarPack>();
    p2->mod_id = "TestTarget"; p2->func_id = "1";
    task->PushShard(std::move(p2));

    EXPECT_TRUE(task->Step(*bus));   // shard 0
    EXPECT_EQ(task->GetCurrentShard(), 1u);

    task->Pause();
    EXPECT_FALSE(task->Step(*bus));  // 跳过 shard 1
    EXPECT_EQ(task->GetCurrentShard(), 1u);  // 没动
}

TEST_F(TaskTest, ResumeContinuesShards)
{
    auto p1 = std::make_unique<ParmarPack>();
    p1->mod_id = "TestTarget"; p1->func_id = "1";
    task->Assign(std::move(p1));

    auto p2 = std::make_unique<ParmarPack>();
    p2->mod_id = "TestTarget"; p2->func_id = "1";
    task->PushShard(std::move(p2));

    task->Step(*bus);       // shard 0
    task->Pause();
    task->Resume();
    EXPECT_TRUE(task->Step(*bus));   // shard 1
}

TEST_F(TaskTest, AllShardsComplete)
{
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id = "TestTarget"; pack->func_id = "1";
    task->Assign(std::move(pack));

    task->Step(*bus);
    EXPECT_EQ(task->GetState(), Task::State::COMPLETED);
}

// ---- Cancel / Reset ----

TEST_F(TaskTest, CancelSetsFailedState)
{
    task->Cancel();
    EXPECT_EQ(task->GetState(), Task::State::FAILED);
}

TEST_F(TaskTest, ResetClearsEverything)
{
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id = "TestTarget";
    task->Assign(std::move(pack));
    task->PushShard(std::make_unique<ParmarPack>());

    task->Reset();
    EXPECT_EQ(task->GetState(), Task::State::IDLE);
    EXPECT_EQ(task->GetTotalShards(), 0u);
    EXPECT_EQ(task->GetCurrentShard(), 0u);
}

// ---- Progress ----

TEST_F(TaskTest, ProgressTracksShards)
{
    auto m = [&]() {
        auto p = std::make_unique<ParmarPack>();
        p->mod_id = "TestTarget"; p->func_id = "1";
        return p;
    };

    task->Assign(m());
    task->PushShard(m());
    task->PushShard(m());
    EXPECT_EQ(task->GetTotalShards(), 3u);

    task->Step(*bus);  // 1/3
    task->Step(*bus);  // 2/3
    task->Step(*bus);  // 3/3 → COMPLETED
    EXPECT_EQ(task->GetState(), Task::State::COMPLETED);
}
