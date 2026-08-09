/// =================================================================
///  ResultStore 测试 — 结果仓库完整验证
/// =================================================================
///
///  测试矩阵：
///   [Basic]       Push + Drain 基本流程
///   [Predicate]   HasResults() 在 cv predicate 中幂等（核心 Bug 修复）
///   [Concurrent]  多 Worker 并发 Push + 单 Consumer Drain
///   [Boundary]    空仓库、null pack、多次 Drain
///   [Memory]      N 轮循环后内存稳定
///   [Integration] Worker → ResultStore → 主循环 cv.wait_for 全流程
///
///  不 mock cout。不依赖 main.cpp。只测 ResultStore。
/// =================================================================

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/ResultStore.h"
#include "core/ThreadPool.h"
#include "core/LockQueue.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

using namespace std::chrono_literals;

// ================================================================
//  辅助：获取进程内存
// ================================================================
static size_t GetWorkingSetKB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX mc{};
    mc.cb = sizeof(mc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mc), sizeof(mc)))
        return mc.WorkingSetSize / 1024;
#endif
    return 0;
}

// ================================================================
//  辅助：创建测试用 ParmarPack
// ================================================================
static std::unique_ptr<ParmarPack> MakeTestPack(const std::string& msg, bool ok = true) {
    auto p = std::make_unique<ParmarPack>();
    p->mod_id  = "TestMod";
    p->func_id = "test";
    p->return_value = msg;
    p->success = ok;
    return p;
}

// ================================================================
//  Test 1: 基本 Push → Drain 流程
// ================================================================
TEST(ResultStore, PushAndDrain) {
    auto& store = ResultStore::Get();
    store.Clear();

    EXPECT_FALSE(store.HasResults());
    EXPECT_EQ(store.Size(), 0u);

    // Push 3 条
    store.PushResult(1, MakeTestPack("first"));
    store.PushResult(2, MakeTestPack("second"));
    store.PushResult(3, MakeTestPack("third"));

    EXPECT_TRUE(store.HasResults());
    EXPECT_EQ(store.Size(), 3u);

    // Drain — 应该全取出来
    auto batch = store.Drain();
    EXPECT_EQ(batch.size(), 3u);
    EXPECT_EQ(batch[0]->task_id, 1u);
    EXPECT_EQ(batch[0]->pack->return_value, "first");
    EXPECT_EQ(batch[1]->task_id, 2u);
    EXPECT_EQ(batch[1]->pack->return_value, "second");
    EXPECT_EQ(batch[2]->task_id, 3u);
    EXPECT_EQ(batch[2]->pack->return_value, "third");

    // Drain 后仓库变空
    EXPECT_FALSE(store.HasResults());
    EXPECT_EQ(store.Size(), 0u);

    // 空 Drain 返回空 vector
    auto empty = store.Drain();
    EXPECT_TRUE(empty.empty());

    store.Clear();
}

// ================================================================
//  Test 2: HasResults 无副作用（核心 Bug 修正验证）
// ================================================================
//  HasResults 必须在多次调用中保持一致，不能有"偷偷 Drain" 的行为。
TEST(ResultStore, HasResultsIsIdempotent) {
    auto& store = ResultStore::Get();
    store.Clear();

    store.PushResult(1, MakeTestPack("data"));

    // 多次调用 HasResults，数据不丢失
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(store.HasResults()) << "HasResults should stay true after " << i << " calls";
    }
    EXPECT_EQ(store.Size(), 1u);

    auto batch = store.Drain();
    EXPECT_EQ(batch.size(), 1u);

    store.Clear();
}

// ================================================================
//  Test 3: cv predicate 中多次求值 HasResults 不丢数据
// ================================================================
//  模拟虚假唤醒：cv.wait_for 的 predicate 可能被额外调用多次。
//  如果 predicate 内部有副作用（如 Drain），数据会丢失。
TEST(ResultStore, PredicateSpuriousWakeupSafe) {
    auto& store = ResultStore::Get();
    store.Clear();

    std::atomic<int> predicate_calls{0};
    std::mutex dummy_mutex;

    // 另一个线程在 50ms 后 Push 数据
    std::thread producer([&]() {
        std::this_thread::sleep_for(50ms);
        store.PushResult(42, MakeTestPack("after_delay"));
    });

    // Consumer: cv.wait_for with HasResults predicate
    {
        std::unique_lock lock(dummy_mutex);
        auto& cv = store.GetCV();
        bool got_data = cv.wait_for(lock, 5000ms, [&]() {
            predicate_calls.fetch_add(1);
            // 每 5ms 虚假唤醒一次（模拟 wait_for 内部行为）
            std::this_thread::sleep_for(1ms);
            return store.HasResults();
        });
        EXPECT_TRUE(got_data) << "Should get data within 5s";
    }
    producer.join();

    // Drain — 数据应该还在
    auto batch = store.Drain();
    EXPECT_EQ(batch.size(), 1u);
    EXPECT_EQ(batch[0]->task_id, 42u);
    EXPECT_EQ(batch[0]->pack->return_value, "after_delay");

    std::cout << "[TEST] Predicate was called " << predicate_calls.load()
              << " times, data survived\n";

    EXPECT_GT(predicate_calls.load(), 1)
        << "Predicate should be called multiple times to validate test";

    store.Clear();
}

// ================================================================
//  Test 4: 并发 Push — 4 工人 + 1 消费者，无数据丢失
// ================================================================
TEST(ResultStore, ConcurrentPushNoDataLoss) {
    auto& store = ResultStore::Get();
    store.Clear();

    constexpr int WORKERS = 4;
    constexpr int PER_WORKER = 500;
    std::atomic<int> total_pushed{0};
    std::atomic<int> total_drained{0};
    std::atomic<long long> id_sum_pushed{0};
    std::atomic<long long> id_sum_drained{0};
    std::atomic<bool> stop{false};

    // 4 个工人并发 Push
    std::vector<std::thread> workers;
    for (int w = 0; w < WORKERS; ++w) {
        workers.emplace_back([&, w]() {
            for (int i = 0; i < PER_WORKER; ++i) {
                uint64_t id = static_cast<uint64_t>(w) * 100000 + i;
                store.PushResult(id, MakeTestPack("w" + std::to_string(w)));
                id_sum_pushed.fetch_add(static_cast<long long>(id));
                total_pushed.fetch_add(1);
            }
        });
    }

    // 消费者：持续 Drain
    std::thread consumer([&]() {
        while (!stop.load()) {
            auto batch = store.Drain();
            for (auto& item : batch) {
                id_sum_drained.fetch_add(static_cast<long long>(item->task_id));
                total_drained.fetch_add(1);
            }
            if (total_drained.load() >= WORKERS * PER_WORKER)
                stop.store(true);
            else
                std::this_thread::sleep_for(1ms);
        }
    });

    for (auto& t : workers) t.join();
    consumer.join();

    // 最终 drain 残留
    auto batch = store.Drain();
    for (auto& item : batch) {
        id_sum_drained.fetch_add(static_cast<long long>(item->task_id));
        total_drained.fetch_add(1);
    }

    int expected = WORKERS * PER_WORKER;
    std::cout << "[TEST] Concurrent: pushed=" << total_pushed.load()
              << " drained=" << total_drained.load()
              << " id_sum_pushed=" << id_sum_pushed.load()
              << " id_sum_drained=" << id_sum_drained.load() << "\n";

    EXPECT_EQ(total_pushed.load(), expected);
    EXPECT_EQ(total_drained.load(), expected)
        << "Every pushed result must be drained";
    EXPECT_EQ(id_sum_drained.load(), id_sum_pushed.load())
        << "Sum of task IDs must match (no duplicates, no missing)";

    store.Clear();
}

// ================================================================
//  Test 5: 高频率 Push + 并发 HasResults 查询（不崩溃）
// ================================================================
TEST(ResultStore, HighFrequencyPushWithConcurrentQuery) {
    auto& store = ResultStore::Get();
    store.Clear();

    std::atomic<bool> stop{false};
    std::atomic<int> push_count{0};
    std::atomic<int> query_count{0};
    constexpr int TARGET = 10000;

    // 快速 Pusher
    std::thread pusher([&]() {
        int i = 0;
        while (!stop.load() && i < TARGET) {
            store.PushResult(i, MakeTestPack("fast"));
            push_count.fetch_add(1);
            ++i;
        }
        stop.store(true);
    });

    // 并发查询者（频繁调 HasResults + Size，不应崩溃）
    std::thread querier([&]() {
        while (!stop.load()) {
            store.HasResults();
            store.Size();
            query_count.fetch_add(1);
        }
    });

    pusher.join();
    querier.join();

    std::cout << "[TEST] HighFreq: pushed=" << push_count.load()
              << " queries=" << query_count.load() << "\n";

    // Drain 所有
    int drained = 0;
    while (drained < TARGET) {
        auto batch = store.Drain();
        drained += static_cast<int>(batch.size());
        if (batch.empty()) std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(drained, TARGET);

    store.Clear();
}

// ================================================================
//  Test 6: 边界 — 空仓库行为
// ================================================================
TEST(ResultStore, BoundaryEmptyStore) {
    auto& store = ResultStore::Get();
    store.Clear();

    EXPECT_FALSE(store.HasResults());
    EXPECT_EQ(store.Size(), 0u);

    auto batch = store.Drain();
    EXPECT_TRUE(batch.empty());

    store.Clear();  // 空 Clear 不崩溃
}

// ================================================================
//  Test 7: 边界 — Push nullptr pack 安全
// ================================================================
TEST(ResultStore, BoundaryNullPack) {
    auto& store = ResultStore::Get();
    store.Clear();

    // Push nullptr pack — 不应崩溃
    store.PushResult(1, nullptr);
    EXPECT_TRUE(store.HasResults());
    EXPECT_EQ(store.Size(), 1u);

    auto batch = store.Drain();
    EXPECT_EQ(batch.size(), 1u);
    EXPECT_EQ(batch[0]->task_id, 1u);
    EXPECT_EQ(batch[0]->pack, nullptr);  // pak 就是 nullptr

    store.Clear();
}

// ================================================================
//  Test 8: 边界 — 大量数据（10000 条）
// ================================================================
TEST(ResultStore, BoundaryLargeBatch) {
    auto& store = ResultStore::Get();
    store.Clear();

    constexpr int N = 10000;
    for (int i = 0; i < N; ++i)
        store.PushResult(i, MakeTestPack("item_" + std::to_string(i)));

    EXPECT_EQ(store.Size(), static_cast<size_t>(N));

    auto batch = store.Drain();
    EXPECT_EQ(batch.size(), static_cast<size_t>(N));

    // 顺序检查（FIFO）
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(batch[i]->task_id, static_cast<uint64_t>(i));
        EXPECT_EQ(batch[i]->pack->return_value, "item_" + std::to_string(i));
    }

    store.Clear();
}

// ================================================================
//  Test 9: Drain 的 swap 不会遗留数据
// ================================================================
TEST(ResultStore, DrainSwapsCleanly) {
    auto& store = ResultStore::Get();
    store.Clear();

    store.PushResult(1, MakeTestPack("a"));
    store.PushResult(2, MakeTestPack("b"));

    auto batch1 = store.Drain();
    EXPECT_EQ(batch1.size(), 2u);
    EXPECT_FALSE(store.HasResults());
    EXPECT_EQ(store.Size(), 0u);

    // 再 Push，再 Drain
    store.PushResult(3, MakeTestPack("c"));
    auto batch2 = store.Drain();
    EXPECT_EQ(batch2.size(), 1u);
    EXPECT_FALSE(store.HasResults());

    store.Clear();
}

// ================================================================
//  Test 10: Clear 后全新开始
// ================================================================
TEST(ResultStore, ClearResetsCompletely) {
    auto& store = ResultStore::Get();
    store.Clear();

    store.PushResult(1, MakeTestPack("test"));
    store.Clear();

    EXPECT_FALSE(store.HasResults());
    EXPECT_EQ(store.Size(), 0u);
    EXPECT_TRUE(store.Drain().empty());
}

// ================================================================
//  Test 11: 内存泄漏 — 连续 Push + Drain 循环
// ================================================================
TEST(ResultStore, MemoryNoLeak) {
    auto& store = ResultStore::Get();
    store.Clear();

    size_t mem_before = GetWorkingSetKB();

    for (int round = 0; round < 10; ++round) {
        for (int i = 0; i < 100; ++i)
            store.PushResult(i, MakeTestPack(std::string(200, 'x')));
        auto batch = store.Drain();
        // batch 在这里析构 → 释放所有 unique_ptr<ParmarPack>
    }

    size_t mem_after = GetWorkingSetKB();
    long long delta = static_cast<long long>(mem_after) - static_cast<long long>(mem_before);

    std::cout << "[TEST] Memory: before=" << mem_before << " KB → after="
              << mem_after << " KB (delta=" << delta << " KB)\n";

    EXPECT_LT(delta, 2048)
        << "Memory should return to baseline, leaked " << delta << " KB";

    store.Clear();
}

// ================================================================
//  Test 12: 集成 — Worker → ResultStore → MainLoop cv 等待全流程
// ================================================================
//  最接近真实 main.cpp 的模拟：
//    工人执行任务 → PushResult → notify → 主循环 cv.wait_for
//    predicate 用 HasResults() 检测 → Drain → 验证结果

TEST(ResultStore, IntegrationWorkerToMainLoop) {
    auto& store = ResultStore::Get();
    store.Clear();

    std::mutex main_mutex;
    std::condition_variable main_cv;
    std::atomic<bool> running{true};

    ThreadPool workers(4);

    constexpr int TOTAL = 200;
    std::atomic<int> submitted{0};
    std::atomic<int> processed{0};

    // 主循环（模拟 main.cpp）
    std::thread main_loop([&]() {
        while (running.load()) {
            // Drain results
            auto batch = store.Drain();
            for (auto& item : batch) {
                if (item->pack) {
                    EXPECT_EQ(item->pack->success, true);
                    EXPECT_FALSE(item->pack->return_value.empty());
                }
                processed.fetch_add(1);
            }

            // cv.wait_for with HasResults predicate
            {
                std::unique_lock lock(main_mutex);
                main_cv.wait_for(lock, 100ms, [&]() {
                    return store.HasResults() || !running.load();
                });
            }

            if (processed.load() >= TOTAL && submitted.load() >= TOTAL)
                running.store(false);
        }
    });

    // 工人线程：执行"任务" → PushResult
    for (int i = 0; i < TOTAL; ++i) {
        workers.Enqueue([i, &store, &main_cv, &submitted]() {
            // 模拟工作
            std::this_thread::sleep_for(1ms);
            auto pack = std::make_unique<ParmarPack>();
            pack->mod_id  = "Worker";
            pack->func_id = "task";
            pack->return_value = "result_" + std::to_string(i);
            pack->success = true;

            store.PushResult(i, std::move(pack));
            main_cv.notify_one();
            submitted.fetch_add(1);
        });
    }

    main_loop.join();

    // 最终 drain 残留
    auto batch = store.Drain();
    processed.fetch_add(static_cast<int>(batch.size()));

    std::cout << "[TEST] Integration: submitted=" << submitted.load()
              << " processed=" << processed.load() << "\n";

    EXPECT_EQ(submitted.load(), TOTAL);
    EXPECT_EQ(processed.load(), TOTAL)
        << "Every submitted result must be processed";

    store.Clear();
}

// ================================================================
//  Test 13: HasResults 与 cv.notify_one 竞态 — 不丢通知
// ================================================================
//  场景：PushResult 在消费者进入 wait 之前就已经完成。
//  HasResults 应返回 true，cv.wait_for 应立刻返回。
TEST(ResultStore, NoLostNotification) {
    auto& store = ResultStore::Get();
    store.Clear();

    std::mutex dummy_mutex;
    auto& cv = store.GetCV();

    // 在消费者 wait 之前 Push
    store.PushResult(1, MakeTestPack("early"));

    // 消费者 wait — predicate 检测到 HasResults，立即返回
    {
        std::unique_lock lock(dummy_mutex);
        bool got_data = cv.wait_for(lock, 5000ms, [&]() {
            return store.HasResults();
        });
        EXPECT_TRUE(got_data);
    }

    auto batch = store.Drain();
    EXPECT_EQ(batch.size(), 1u);

    store.Clear();
}

// ================================================================
//  Test 14: 并发 Push + Drain（不同锁语义）不死锁
// ================================================================
//  验证 PushResult 的锁和 Drain 的锁不形成死锁。
TEST(ResultStore, NoDeadlockPushAndDrain) {
    auto& store = ResultStore::Get();
    store.Clear();

    std::atomic<bool> stop{false};
    std::atomic<int> push_ok{0};
    std::atomic<int> drain_ok{0};
    std::atomic<int> errors{0};

    // 两个 Pusher
    std::thread p1([&]() {
        while (!stop.load()) {
            try {
                store.PushResult(1, MakeTestPack("p1"));
                push_ok.fetch_add(1);
            } catch (...) { errors.fetch_add(1); }
        }
    });

    std::thread p2([&]() {
        while (!stop.load()) {
            try {
                store.PushResult(2, MakeTestPack("p2"));
                push_ok.fetch_add(1);
            } catch (...) { errors.fetch_add(1); }
        }
    });

    // 两个 Drainer
    std::thread d1([&]() {
        while (!stop.load()) {
            try {
                auto batch = store.Drain();
                drain_ok.fetch_add(static_cast<int>(batch.size()));
            } catch (...) { errors.fetch_add(1); }
        }
    });

    std::thread d2([&]() {
        while (!stop.load()) {
            try {
                auto batch = store.Drain();
                drain_ok.fetch_add(static_cast<int>(batch.size()));
            } catch (...) { errors.fetch_add(1); }
        }
    });

    std::this_thread::sleep_for(2s);
    stop.store(true);

    p1.join(); p2.join(); d1.join(); d2.join();

    // 最终 drain 残留
    auto final_batch = store.Drain();
    drain_ok.fetch_add(static_cast<int>(final_batch.size()));

    std::cout << "[TEST] NoDeadlock: push_ok=" << push_ok.load()
              << " drain_ok=" << drain_ok.load()
              << " errors=" << errors.load() << "\n";

    EXPECT_EQ(errors.load(), 0) << "No exceptions should be thrown";
    EXPECT_GT(push_ok.load(), 0);
    EXPECT_GE(drain_ok.load(), push_ok.load())
        << "Every pushed item should eventually be drained";

    store.Clear();
}

// ================================================================
//  Test 15: done_time 时间戳合理性
// ================================================================
TEST(ResultStore, TimestampIsReasonable) {
    auto& store = ResultStore::Get();
    store.Clear();

    auto before = std::chrono::steady_clock::now();
    store.PushResult(1, MakeTestPack("ts_test"));
    auto after = std::chrono::steady_clock::now();

    auto batch = store.Drain();
    ASSERT_EQ(batch.size(), 1u);

    auto ts = batch[0]->done_time;
    // 时间戳应在 Push 前后之间
    EXPECT_GE(ts, before);
    EXPECT_LE(ts, after);

    store.Clear();
}
