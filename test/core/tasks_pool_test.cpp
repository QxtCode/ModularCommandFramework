/// TasksPool unit tests — object pool lifecycle

#include <gtest/gtest.h>
#include <memory>
#include <set>
#include <string>
#include "core/TasksPool.h"
#include "core/ParmarPack.h"

class TasksPoolTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        pool = std::make_unique<TasksPool>(5);
    }

    void TearDown() override
    {
        pool.reset();
    }

    std::unique_ptr<TasksPool> pool;
};

// ---- 初始化 ----

TEST_F(TasksPoolTest, InitialState)
{
    EXPECT_EQ(pool->GetTotalCount(), 5u);
    EXPECT_EQ(pool->GetFreeCount(), 5u);
}

// ---- 获取 / 归还 ----

TEST_F(TasksPoolTest, AcquireOne)
{
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id = "test";

    Task* t = pool->Acquire(std::move(pack));
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(pool->GetFreeCount(), 4u);
    EXPECT_EQ(t->GetState(), Task::State::IDLE);
}

TEST_F(TasksPoolTest, AcquireAll)
{
    for (int i = 0; i < 5; i++)
    {
        auto pack = std::make_unique<ParmarPack>();
        pack->mod_id = "task_" + std::to_string(i);
        EXPECT_NE(pool->Acquire(std::move(pack)), nullptr);
        EXPECT_EQ(pool->GetFreeCount(), 4u - i);
    }
    EXPECT_EQ(pool->GetFreeCount(), 0u);

    // 池子满了 → 返回 nullptr
    auto extra = std::make_unique<ParmarPack>();
    EXPECT_EQ(pool->Acquire(std::move(extra)), nullptr);
}

TEST_F(TasksPoolTest, ReleaseReturnsToPool)
{
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id = "test";
    Task* t = pool->Acquire(std::move(pack));

    EXPECT_EQ(pool->GetFreeCount(), 4u);
    pool->Release(t);
    EXPECT_EQ(pool->GetFreeCount(), 5u);
}

TEST_F(TasksPoolTest, AcquireNullPackReturnsNull)
{
    EXPECT_EQ(pool->Acquire(nullptr), nullptr);
}

// ---- 边界 ----

TEST(TasksPoolEdge, SizeZeroThrows)
{
    EXPECT_THROW(TasksPool(0), std::invalid_argument);
}

TEST(TasksPoolEdge, SizeOne)
{
    TasksPool p(1);
    EXPECT_EQ(p.GetTotalCount(), 1u);
    EXPECT_EQ(p.GetFreeCount(), 1u);

    auto pack = std::make_unique<ParmarPack>();
    Task* t = p.Acquire(std::move(pack));
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(p.GetFreeCount(), 0u);
}
