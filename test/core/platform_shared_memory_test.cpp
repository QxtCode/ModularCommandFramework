/// platform::SharedMemory 独立测试
/// 覆盖: Create, Open, Data, Size, 析构, 跨"进程"读写, 并发

#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "core/platform/shared_memory.h"

using namespace platform;

// ================================================================
//  基本生命周期
// ================================================================
TEST(SharedMemoryTest, Create_Success) {
    auto shm = SharedMemory::Create("test_shm_basic", 1024);
    ASSERT_NE(shm, nullptr);
    EXPECT_NE(shm->Data(), nullptr);
    EXPECT_EQ(shm->Size(), 1024u);
}

TEST(SharedMemoryTest, Create_ZeroSize) {
    auto shm = SharedMemory::Create("test_shm_zero", 0);
    EXPECT_EQ(shm, nullptr);
}

TEST(SharedMemoryTest, Open_SameName) {
    auto owner = SharedMemory::Create("test_shm_open", 512);
    ASSERT_NE(owner, nullptr);

    auto reader = SharedMemory::Open("test_shm_open", 512);
    ASSERT_NE(reader, nullptr);
    EXPECT_NE(reader->Data(), nullptr);
    EXPECT_EQ(reader->Size(), 512u);
}

TEST(SharedMemoryTest, Open_NonExistent) {
    auto shm = SharedMemory::Open("__nonexistent_shm__", 256);
    EXPECT_EQ(shm, nullptr);
}

// ================================================================
//  数据读写（模拟跨进程）
// ================================================================
TEST(SharedMemoryTest, WriteRead_SameProcess) {
    auto owner = SharedMemory::Create("test_shm_rw", 256);
    ASSERT_NE(owner, nullptr);

    // 写入
    const char* msg = "Hello Shared Memory!";
    std::memcpy(owner->Data(), msg, std::strlen(msg) + 1);

    // 从另一个 handle 读取
    auto reader = SharedMemory::Open("test_shm_rw", 256);
    ASSERT_NE(reader, nullptr);

    const char* read_back = static_cast<const char*>(reader->Data());
    EXPECT_STREQ(read_back, msg);
}

TEST(SharedMemoryTest, Data_IndependentAddresses) {
    auto owner = SharedMemory::Create("test_shm_indep", 128);
    auto reader = SharedMemory::Open("test_shm_indep", 128);
    ASSERT_NE(owner, nullptr);
    ASSERT_NE(reader, nullptr);

    // 两个不同的映射地址，但指向同一物理内存
    EXPECT_NE(owner->Data(), reader->Data());

    // 写入 owner
    int* p = static_cast<int*>(owner->Data());
    *p = 42;

    // reader 应该看到
    const int* q = static_cast<const int*>(reader->Data());
    EXPECT_EQ(*q, 42);
}

// ================================================================
//  析构清理验证
// ================================================================
TEST(SharedMemoryTest, Destructor_RecreateSameName) {
    // 第一次
    {
        auto shm1 = SharedMemory::Create("test_shm_recreate", 256);
        ASSERT_NE(shm1, nullptr);
        int* p = static_cast<int*>(shm1->Data());
        *p = 123;
    }  // 析构 → unlink

    // 第二次 — 同名字应该可以重新创建
    {
        auto shm2 = SharedMemory::Create("test_shm_recreate", 256);
        ASSERT_NE(shm2, nullptr);
        int* p = static_cast<int*>(shm2->Data());
        // 全新映射，应该已清零
        EXPECT_NE(*p, 123);  // Windows 初始化为 0
    }
}

// ================================================================
//  并发创建/打开/销毁
// ================================================================
TEST(SharedMemoryTest, Stress_ConcurrentOpen) {
    // Owner 先创建
    auto owner = SharedMemory::Create("test_shm_concurrent", 4096);
    ASSERT_NE(owner, nullptr);

    constexpr int kThreads = 8;
    constexpr int kIterations = 200;
    std::atomic<int> ok{0};
    std::atomic<int> fail{0};

    auto worker = [&]() {
        for (int i = 0; i < kIterations; ++i) {
            auto reader = SharedMemory::Open("test_shm_concurrent", 4096);
            if (reader && reader->Data()) {
                ok.fetch_add(1);
            } else {
                fail.fetch_add(1);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    EXPECT_EQ(fail.load(), 0);
    EXPECT_EQ(ok.load(), kThreads * kIterations);
}

TEST(SharedMemoryTest, Stress_CreateOpenDestroyCycles) {
    constexpr int kCycles = 100;

    for (int i = 0; i < kCycles; ++i) {
        std::string name = "test_shm_cycle_" + std::to_string(i);

        auto owner = SharedMemory::Create(name, 128);
        ASSERT_NE(owner, nullptr) << "Cycle " << i;

        int* p = static_cast<int*>(owner->Data());
        *p = i;

        auto reader = SharedMemory::Open(name, 128);
        ASSERT_NE(reader, nullptr) << "Cycle " << i;
        EXPECT_EQ(*static_cast<int*>(reader->Data()), i) << "Cycle " << i;
    }  // 每个 cycle 自动析构
}

// ================================================================
//  MetricsProtocol 兼容性 — 验证 struct 布局
// ================================================================
struct TestMetrics {
    uint32_t magic;
    uint32_t seq;
    uint64_t uptime_ms;
    uint32_t tasks_active;
};

TEST(SharedMemoryTest, MetricsStructCompatibility) {
    auto shm = SharedMemory::Create("test_shm_metrics", sizeof(TestMetrics));
    ASSERT_NE(shm, nullptr);

    auto* m = static_cast<TestMetrics*>(shm->Data());
    m->magic  = 0xFEEDBEEF;
    m->seq    = 0;
    m->uptime_ms = 12345;
    m->tasks_active = 7;

    // 从 reader 验证
    auto reader = SharedMemory::Open("test_shm_metrics", sizeof(TestMetrics));
    ASSERT_NE(reader, nullptr);

    const auto* r = static_cast<const TestMetrics*>(reader->Data());
    EXPECT_EQ(r->magic, 0xFEEDBEEFu);
    EXPECT_EQ(r->seq, 0u);
    EXPECT_EQ(r->uptime_ms, 12345ull);
    EXPECT_EQ(r->tasks_active, 7u);
}
