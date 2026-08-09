/// =================================================================
///  InputThread 测试 — 异步输入 + cv.wait_for 事件驱动
/// =================================================================
///
///  在改动 main.cpp 之前充分验证新模式。
///
///  测试矩阵：
///    [Basic]      输入线程 → LockQueue → 主循环消费
///    [Predicate]  cv.wait_for 同时等 done_queue + input_queue
///    [Concurrent]  输入线程 + 工人线程同时推送
///    [Boundary]    空行、长行、EOF、burst 输入
///    [Memory]      N 轮循环后内存不增长
///    [Shutdown]    输入线程 / 主循环协调退出
///
///  不依赖 cin/cout — 模拟输入源直接 Push 到 LockQueue。
/// =================================================================

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "core/LockQueue.h"
#include "core/ThreadPool.h"
#include "core/TasksPool.h"
#include "core/Task.h"
#include "core/ModuleLifeManager.h"
#include "core/ParmarPack.h"
#include "parser/CommandParser.h"
#include "sdk/IModule.h"
#include "event_bus/event_bus.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

using namespace std::chrono_literals;

// ================================================================
//  辅助：获取进程内存
// ================================================================
static size_t GetWorkingSetKB()
{
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
//  辅助：drain done_queue
// ================================================================
static void DrainDoneQueue(std::mutex& m, std::queue<Task*>& q, TasksPool& pool)
{
    std::unique_lock lock(m);
    while (!q.empty())
    {
        Task* t = q.front(); q.pop();
        lock.unlock();
        pool.Release(t);
        lock.lock();
    }
}

// ================================================================
//  Test 1: 输入线程 → LockQueue → 消费者，基本正确性
// ================================================================
//  模拟输入线程推送 N 行，主循环消费全部。验证顺序和数量。

TEST(InputThread, BasicProducerConsumer)
{
    constexpr int N = 500;
    LockQueue<std::string> input_queue;
    std::atomic<bool> running{true};

    // 模拟输入线程：推送 N 行
    std::thread producer([&]() {
        for (int i = 0; i < N; ++i)
        {
            if (!running.load()) break;
            input_queue.Push(std::make_unique<std::string>(
                "line_" + std::to_string(i)));
            std::this_thread::sleep_for(100us);  // 模拟输入间隔
        }
        running.store(false);
    });

    // 消费者：逐行取出
    std::vector<std::string> received;
    while (running.load())
    {
        std::unique_ptr<std::string> line;
        if (input_queue.TryPop(line))
            received.push_back(*line);
        else
            std::this_thread::sleep_for(1ms);  // 没数据时短暂休眠
    }

    // drain 剩余
    {
        std::unique_ptr<std::string> line;
        while (input_queue.TryPop(line))
            received.push_back(*line);
    }

    producer.join();

    EXPECT_EQ(received.size(), N);
    for (int i = 0; i < N; ++i)
        EXPECT_EQ(received[i], "line_" + std::to_string(i));
}

// ================================================================
//  Test 2: cv.wait_for 同时等待两个队列（核心模式验证）
// ================================================================
//  模式：done_cv.wait_for(lock, 100ms, predicate 检查两个队列)
//  这是改动后 main.cpp 的核心等待模式。

TEST(InputThread, PredicateWaitsOnTwoQueues)
{
    std::mutex done_mutex;
    std::condition_variable done_cv;
    std::queue<int> done_queue;
    LockQueue<std::string> input_queue;

    std::atomic<bool> running{true};
    std::atomic<int> total_consumed{0};

    // 消费者（模拟主循环）
    std::thread consumer([&]() {
        while (running.load())
        {
            bool had_work = false;

            {
                std::unique_lock lock(done_mutex);

                // ★ 核心模式：一个 cv 谓词同时检查两个队列
                done_cv.wait_for(lock, 100ms, [&]() {
                    return !done_queue.empty() || !input_queue.Empty() || !running.load();
                });

                while (!done_queue.empty())
                {
                    done_queue.pop();
                    total_consumed.fetch_add(1);
                    had_work = true;
                }
            }

            // 取输入（不需要 done_mutex，LockQueue 自己保护）
            std::unique_ptr<std::string> line;
            while (input_queue.TryPop(line))
            {
                total_consumed.fetch_add(1);
                had_work = true;
            }

            if (!had_work)
                std::this_thread::sleep_for(1ms);  // timeout 时短暂 yield
        }
    });

    // 生产者 1：done_queue（模拟工人线程）
    std::thread worker([&]() {
        for (int i = 0; i < 200; ++i)
        {
            {
                std::lock_guard lock(done_mutex);
                done_queue.push(i);
            }
            done_cv.notify_one();
            std::this_thread::sleep_for(500us);
        }
    });

    // 生产者 2：input_queue（模拟输入线程）
    std::thread input_thread([&]() {
        for (int i = 0; i < 200; ++i)
        {
            input_queue.Push(std::make_unique<std::string>("cmd_" + std::to_string(i)));
            done_cv.notify_one();
            std::this_thread::sleep_for(500us);
        }
    });

    worker.join();
    input_thread.join();
    running.store(false);
    done_cv.notify_one();
    consumer.join();

    EXPECT_EQ(total_consumed.load(), 400)
        << "Should consume 200 tasks + 200 inputs = 400 total";
}

// ================================================================
//  Test 3: 多生产者并发压测（done_queue + input_queue 同时高压推送）
// ================================================================
//  4 个工人 + 2 个输入源，全部通知同一个 cv。确保无数据丢失。

TEST(InputThread, MultiProducerStress)
{
    std::mutex done_mutex;
    std::condition_variable done_cv;
    std::queue<int> done_queue;
    LockQueue<std::string> input_queue;

    std::atomic<bool> running{true};
    std::atomic<int> done_consumed{0};
    std::atomic<int> input_consumed{0};
    std::atomic<int> worker_produced{0};
    std::atomic<int> input_produced{0};

    constexpr int N_PER_WORKER = 500;
    constexpr int N_PER_INPUT  = 500;
    constexpr int WORKERS = 4;
    constexpr int INPUTS  = 2;

    // 消费者（主循环）
    std::thread consumer([&]() {
        while (running.load())
        {
            {
                std::unique_lock lock(done_mutex);
                done_cv.wait_for(lock, 5ms, [&]() {
                    return !done_queue.empty() || !input_queue.Empty() || !running.load();
                });
                while (!done_queue.empty())
                {
                    done_queue.pop();
                    done_consumed.fetch_add(1);
                }
            }

            std::unique_ptr<std::string> line;
            while (input_queue.TryPop(line))
                input_consumed.fetch_add(1);

            // 检查是否全部完成
            if (worker_produced.load() >= WORKERS * N_PER_WORKER &&
                input_produced.load() >= INPUTS * N_PER_INPUT &&
                done_consumed.load() >= WORKERS * N_PER_WORKER &&
                input_consumed.load() >= INPUTS * N_PER_INPUT)
            {
                running.store(false);
            }
        }
    });

    // 工人线程
    std::vector<std::thread> workers;
    for (int w = 0; w < WORKERS; ++w)
    {
        workers.emplace_back([&, w]() {
            for (int i = 0; i < N_PER_WORKER; ++i)
            {
                {
                    std::lock_guard lock(done_mutex);
                    done_queue.push(w * 10000 + i);
                }
                done_cv.notify_one();
                worker_produced.fetch_add(1);
            }
        });
    }

    // 输入线程
    std::vector<std::thread> inputs;
    for (int t = 0; t < INPUTS; ++t)
    {
        inputs.emplace_back([&, t]() {
            for (int i = 0; i < N_PER_INPUT; ++i)
            {
                input_queue.Push(std::make_unique<std::string>(
                    "src" + std::to_string(t) + "_" + std::to_string(i)));
                done_cv.notify_one();
                input_produced.fetch_add(1);
            }
        });
    }

    for (auto& t : workers) t.join();
    for (auto& t : inputs) t.join();

    // 最终 drain
    running.store(false);
    done_cv.notify_one();
    consumer.join();

    // 最终清空
    {
        std::unique_lock lock(done_mutex);
        while (!done_queue.empty())
        {
            done_queue.pop();
            done_consumed.fetch_add(1);
        }
    }
    {
        std::unique_ptr<std::string> line;
        while (input_queue.TryPop(line))
            input_consumed.fetch_add(1);
    }

    int expected_worker = WORKERS * N_PER_WORKER;
    int expected_input  = INPUTS * N_PER_INPUT;

    std::cout << "[TEST] MultiProducer: done=" << done_consumed.load()
              << "/" << expected_worker
              << " input=" << input_consumed.load()
              << "/" << expected_input << "\n";

    EXPECT_EQ(done_consumed.load(), expected_worker)
        << "All worker items must be consumed";
    EXPECT_EQ(input_consumed.load(), expected_input)
        << "All input items must be consumed";
}

// ================================================================
//  Test 4: 边界 — 空行正确处理
// ================================================================
TEST(InputThread, BoundaryEmptyLine)
{
    LockQueue<std::string> input_queue;

    // 空字符串也是合法输入（按回车列出模块）
    input_queue.Push(std::make_unique<std::string>(""));

    std::unique_ptr<std::string> line;
    ASSERT_TRUE(input_queue.TryPop(line));
    EXPECT_TRUE(line->empty()) << "Empty line should be preserved";
}

// ================================================================
//  Test 5: 边界 — 超长行（>10KB）
// ================================================================
TEST(InputThread, BoundaryLongLine)
{
    LockQueue<std::string> input_queue;

    std::string long_line(10000, 'x');
    input_queue.Push(std::make_unique<std::string>(long_line));

    std::unique_ptr<std::string> line;
    ASSERT_TRUE(input_queue.TryPop(line));
    EXPECT_EQ(line->size(), 10000u);
    EXPECT_EQ(*line, long_line);
}

// ================================================================
//  Test 6: 边界 — 快速连续输入（burst）
// ================================================================
TEST(InputThread, BoundaryBurstInput)
{
    LockQueue<std::string> input_queue;
    std::atomic<bool> running{true};
    std::atomic<int> consumed{0};

    constexpr int BURST = 1000;

    std::thread consumer([&]() {
        while (running.load())
        {
            std::unique_ptr<std::string> line;
            if (input_queue.TryPop(line))
                consumed.fetch_add(1);
            else if (consumed.load() >= BURST)
                running.store(false);
        }
    });

    // 快速连续推送（不 sleep）
    for (int i = 0; i < BURST; ++i)
        input_queue.Push(std::make_unique<std::string>("burst_" + std::to_string(i)));

    consumer.join();

    // drain 剩余
    std::unique_ptr<std::string> line;
    while (input_queue.TryPop(line))
        consumed.fetch_add(1);

    EXPECT_EQ(consumed.load(), BURST)
        << "All burst inputs must be consumed";
}

// ================================================================
//  Test 7: 关闭竞态 — 消费者在 wait_for 中时，输入线程退出
// ================================================================
TEST(InputThread, ShutdownWhileWaiting)
{
    std::mutex done_mutex;
    std::condition_variable done_cv;
    std::queue<int> done_queue;
    LockQueue<std::string> input_queue;
    std::atomic<bool> running{true};

    std::atomic<bool> consumer_exited_cleanly{false};

    std::thread consumer([&]() {
        while (running.load())
        {
            std::unique_lock lock(done_mutex);
            // 长超时 — 如果 running 没被正确通知，会等满 5 秒
            done_cv.wait_for(lock, 5000ms, [&]() {
                return !done_queue.empty() || !input_queue.Empty() || !running.load();
            });
        }
        consumer_exited_cleanly.store(true);
    });

    // 让消费者进入 wait
    std::this_thread::sleep_for(100ms);

    // 模拟关闭：设置 running=false + notify
    running.store(false);
    done_cv.notify_one();

    auto start = std::chrono::steady_clock::now();
    consumer.join();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    EXPECT_TRUE(consumer_exited_cleanly.load());
    EXPECT_LT(elapsed, 1000)
        << "Consumer should wake within 1s of shutdown, took " << elapsed << "ms";
}

// ================================================================
//  Test 8: 关闭竞态 — 输入线程先退出，主循环随后退出
// ================================================================
TEST(InputThread, InputThreadExitsFirst)
{
    LockQueue<std::string> input_queue;
    std::mutex done_mutex;
    std::condition_variable done_cv;
    std::queue<int> done_queue;
    std::atomic<bool> input_running{true};
    std::atomic<bool> main_running{true};

    std::atomic<int> inputs_pushed{0};
    std::atomic<int> inputs_consumed{0};

    // 输入线程：推送一些行后退出
    std::thread input_thread([&]() {
        for (int i = 0; i < 100; ++i)
        {
            input_queue.Push(std::make_unique<std::string>("cmd_" + std::to_string(i)));
            done_cv.notify_one();
            inputs_pushed.fetch_add(1);
            std::this_thread::sleep_for(1ms);
        }
        input_running.store(false);
        done_cv.notify_one();
    });

    // 主循环：drain + 检查 input_running
    std::thread main_loop([&]() {
        while (main_running.load())
        {
            {
                std::unique_lock lock(done_mutex);
                done_cv.wait_for(lock, 100ms, [&]() {
                    return !done_queue.empty() || !input_queue.Empty() || !input_running.load();
                });
                while (!done_queue.empty()) done_queue.pop();
            }

            std::unique_ptr<std::string> line;
            while (input_queue.TryPop(line))
                inputs_consumed.fetch_add(1);

            // 输入线程已停 + 队列已空 → 退出
            if (!input_running.load() && input_queue.Empty())
                main_running.store(false);
        }
    });

    input_thread.join();
    main_loop.join();

    EXPECT_EQ(inputs_consumed.load(), inputs_pushed.load())
        << "All pushed inputs must be consumed before main loop exits";
}

// ================================================================
//  Test 9: 集成 — 完整模拟：输入线程 + Task 系统 + cv 等待
// ================================================================
//  最接近真实 main.cpp 的模拟：
//    输入线程推送命令 → 主循环解析 → 提交 Task → 工人执行 → done_queue
//    主循环用 cv.wait_for 统一等待 done_queue 或 input_queue。

TEST(InputThread, IntegrationFullMainLoop)
{
    auto& mgr = ModuleLifeManager::GetInstance();
    auto& bus = EventBus::GetInstance();

    // 注册测试模块
    class TestMod : public ModuleBaseObject
    {
    public:
        TestMod(std::string name, std::atomic<int>* count)
            : name_(std::move(name)), count_(count) {}
        const char* GetName() const override { return name_.c_str(); }
        bool OnInit() override
        {
            REGISTER_FUNC("echo", "echo back", {
                std::string msg = pack->GetOr("msg", "no_msg");
                count_->fetch_add(1);
                pack->return_value = name_ + ":" + msg;
                pack->success = true;
            });
            REGISTER_FUNC("help", "help", { pack->success = true; });
            return true;
        }
    private:
        std::string name_;
        std::atomic<int>* count_;
    };

    std::atomic<int> exec_count{0};
    mgr.AddModule(std::make_unique<TestMod>("ITest", &exec_count));

    // ---- 框架组件 ----
    ThreadPool workers(4);
    TasksPool tasks(8);
    std::mutex done_mutex;
    std::queue<Task*> done_queue;
    std::condition_variable done_cv;
    LockQueue<std::string> input_queue;

    std::atomic<bool> running{true};
    std::atomic<int> commands_submitted{0};
    std::atomic<int> commands_completed{0};
    std::atomic<bool> all_done{false};

    // 输入线程
    std::thread input_thread([&]() {
        for (int i = 0; i < 200; ++i)
        {
            if (!running.load()) break;
            std::string cmd = "-m:ITest -f:echo -v:msg|test_" + std::to_string(i);
            input_queue.Push(std::make_unique<std::string>(std::move(cmd)));
            done_cv.notify_one();
            std::this_thread::sleep_for(2ms);
        }
        all_done.store(true);
        done_cv.notify_one();
    });

    // 主循环
    std::thread main_loop([&]() {
        auto& parser = CommandParser::Get();

        while (running.load())
        {
            // ---- Drain done_queue ----
            {
                std::unique_lock lock(done_mutex);
                while (!done_queue.empty())
                {
                    Task* t = done_queue.front();
                    done_queue.pop();
                    lock.unlock();
                    tasks.Release(t);
                    commands_completed.fetch_add(1);
                    lock.lock();
                }
            }

            // ---- 等数据 ----
            bool got_work = false;
            {
                std::unique_lock lock(done_mutex);
                got_work = done_cv.wait_for(lock, 100ms, [&]() {
                    return !done_queue.empty() || !input_queue.Empty() || !running.load();
                });
                // drain done_queue（可能在 wait 期间有新数据）
                while (!done_queue.empty())
                {
                    Task* t = done_queue.front();
                    done_queue.pop();
                    lock.unlock();
                    tasks.Release(t);
                    commands_completed.fetch_add(1);
                    lock.lock();
                }
            }

            // ---- 处理输入 ----
            std::unique_ptr<std::string> line;
            while (input_queue.TryPop(line))
            {
                if (*line == "/exit")
                {
                    running.store(false);
                    break;
                }
                if (line->empty()) continue;

                // 发送给 parser
                parser.SendCommand("TXT", std::any(std::string(*line)));
                auto pack = parser.PopPack();
                if (!pack) continue;

                Task* task = tasks.Acquire(std::move(pack));
                if (!task) continue;

                workers.Enqueue([task, &bus, &done_mutex, &done_queue, &done_cv]() {
                    try { while (task->Step(bus)) {} }
                    catch (...) {}
                    {
                        std::lock_guard lock(done_mutex);
                        done_queue.push(task);
                    }
                    done_cv.notify_one();
                });
                commands_submitted.fetch_add(1);
            }

            // 退出条件：所有输入处理完 + 所有任务完成
            if (all_done.load() &&
                commands_completed.load() >= commands_submitted.load() &&
                input_queue.Empty())
            {
                running.store(false);
            }
        }
    });

    input_thread.join();
    main_loop.join();

    // 最终 drain
    DrainDoneQueue(done_mutex, done_queue, tasks);

    std::cout << "[TEST] Integration: submitted=" << commands_submitted.load()
              << " completed=" << commands_completed.load()
              << " executed=" << exec_count.load()
              << " pool=" << tasks.GetFreeCount() << "/" << tasks.GetTotalCount()
              << "\n";

    EXPECT_GT(commands_submitted.load(), 0) << "Should have submitted some commands";
    EXPECT_EQ(commands_completed.load(), commands_submitted.load())
        << "All submitted commands should complete";
    EXPECT_EQ(exec_count.load(), commands_submitted.load())
        << "Each command should execute module function once";
    EXPECT_EQ(tasks.GetFreeCount(), tasks.GetTotalCount())
        << "TasksPool should be fully returned (no leak)";

    mgr.UnloadModule("ITest");
}

// ================================================================
//  Test 10: OutputMutex 不锁住 "getline"
// ================================================================
//  核心安全保证：输入线程只在打印提示符时短暂持 OutputMutex，
//  "getline" 期间完全不持锁。这里模拟：
//    - 工人线程频繁 LOG（持 OutputMutex 写 cout）
//    - 输入线程推送（等价于真实 getline 返回后的操作）
//    - 验证两者不互相阻塞

TEST(InputThread, OutputMutexDoesNotBlockInput)
{
    LockQueue<std::string> input_queue;
    std::atomic<bool> running{true};
    std::atomic<int> worker_writes{0};
    std::atomic<int> input_pushes{0};
    std::atomic<long long> max_input_wait_us{0};

    // 工人线程：频繁拿 OutputMutex 写日志（模拟）
    // 用 raw std::thread 而非 ThreadPool，因为 job 是死循环，
    // ThreadPool 析构会 join 工人线程 → 挂死。
    std::vector<std::thread> worker_threads;
    for (int i = 0; i < 4; ++i)
    {
        worker_threads.emplace_back([&]() {
            while (running.load())
            {
                {
                    std::lock_guard lock(IModule::OutputMutex());
                    // 模拟 LOG_PLAIN 的输出
                    volatile int dummy = 0;
                    for (int j = 0; j < 1000; ++j) dummy += j;
                    (void)dummy;
                }
                worker_writes.fetch_add(1);
                std::this_thread::sleep_for(500us);  // 高频
            }
        });
    }

    // 输入线程：模拟真实节奏 — 短暂持 OutputMutex 打印提示符 → 释放 → "getline"（sleep 模拟）→ Push
    std::thread input_thread([&]() {
        while (running.load())
        {
            // ① 短暂持 OutputMutex 打印 "> "（模拟真实 getline 前的 prompt）
            {
                std::lock_guard lock(IModule::OutputMutex());
                volatile int dummy = 0;
                for (int j = 0; j < 500; ++j) dummy += j;
                (void)dummy;
            }

            // ② "getline" — 完全不持任何锁
            //    模拟用户思考时间（随机 0~5ms）
            auto wait_us = (rand() % 50) * 100;
            std::this_thread::sleep_for(std::chrono::microseconds(wait_us));

            // ③ Push 到 input_queue（几十纳秒）
            auto t0 = std::chrono::steady_clock::now();
            input_queue.Push(std::make_unique<std::string>("test_input"));
            auto t1 = std::chrono::steady_clock::now();
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            if (elapsed_us > max_input_wait_us.load())
                max_input_wait_us.store(elapsed_us);

            input_pushes.fetch_add(1);
        }
    });

    std::this_thread::sleep_for(3s);
    running.store(false);
    input_thread.join();
    for (auto& t : worker_threads) t.join();

    std::cout << "[TEST] OutputMutex: worker_writes=" << worker_writes.load()
              << " input_pushes=" << input_pushes.load()
              << " max_push_us=" << max_input_wait_us.load()
              << "\n";

    EXPECT_GT(worker_writes.load(), 100)
        << "Workers should have completed many writes";
    EXPECT_GT(input_pushes.load(), 100)
        << "Input thread should have pushed many inputs";
    // LockQueue::Push 是 O(1) + 一次锁，通常 < 10us。如果 max > 1000us 说明有阻塞。
    EXPECT_LT(max_input_wait_us.load(), 5000)
        << "Input push should not be blocked for >5ms by OutputMutex";
}

// ================================================================
//  Test 11: 内存泄漏 — N 轮完整循环后内存稳定
// ================================================================
TEST(InputThread, MemoryNoLeakOverCycles)
{
    size_t mem_before = GetWorkingSetKB();

    for (int round = 0; round < 5; ++round)
    {
        LockQueue<std::string> input_queue;
        std::mutex done_mutex;
        std::condition_variable done_cv;
        std::queue<int> done_queue;
        std::atomic<bool> running{true};

        constexpr int N = 200;

        // 输入线程
        std::thread input_thread([&]() {
            for (int i = 0; i < N; ++i)
            {
                input_queue.Push(std::make_unique<std::string>("r" + std::to_string(round) + "_" + std::to_string(i)));
                done_cv.notify_one();
            }
            running.store(false);
            done_cv.notify_one();
        });

        // 主循环
        std::thread main_loop([&]() {
            int consumed = 0;
            while (consumed < N)
            {
                {
                    std::unique_lock lock(done_mutex);
                    done_cv.wait_for(lock, 100ms, [&]() {
                        return !done_queue.empty() || !input_queue.Empty() || !running.load();
                    });
                    while (!done_queue.empty()) done_queue.pop();
                }
                std::unique_ptr<std::string> line;
                while (input_queue.TryPop(line))
                    consumed++;
            }
        });

        input_thread.join();
        main_loop.join();
    }

    size_t mem_after = GetWorkingSetKB();
    long long delta = static_cast<long long>(mem_after) - static_cast<long long>(mem_before);

    std::cout << "[TEST] Memory: before=" << mem_before << " KB → after="
              << mem_after << " KB (delta=" << delta << " KB)\n";

    // 5 轮循环，每轮创建 LockQueue/mutex/cv/2 threads + 200 strings
    // 全部局部变量析构后，内存应该回到基线附近。
    // 保守阈值：OS 可能不立即回收，给 2MB 余量。
    EXPECT_LT(delta, 2048)
        << "Memory should return to baseline after 5 cycles, leaked " << delta << " KB";
}

// ================================================================
//  Test 12: 不丢失通知 — notify 在 wait 之前发出
// ================================================================
//  边界场景：输入线程在消费者进入 wait_for 之前就 push + notify。
//  wait_for 苏醒后 predicate 检查 Queue::Empty() 应返回 false。

TEST(InputThread, NoLostWakeup)
{
    std::mutex done_mutex;
    std::condition_variable done_cv;
    std::queue<int> done_queue;
    LockQueue<std::string> input_queue;

    // 在消费者 wait 之前就 push
    input_queue.Push(std::make_unique<std::string>("early_bird"));
    done_cv.notify_one();

    // 消费者 wait — predicate 检测到 input_queue 已有数据，立即返回
    {
        std::unique_lock lock(done_mutex);
        bool got_work = done_cv.wait_for(lock, 5000ms, [&]() {
            return !done_queue.empty() || !input_queue.Empty();
        });

        EXPECT_TRUE(got_work) << "Should wake immediately because input_queue already has data";
    }

    std::unique_ptr<std::string> line;
    EXPECT_TRUE(input_queue.TryPop(line));
    EXPECT_EQ(*line, "early_bird");
}

// ================================================================
//  Test 13: 高并发压力 — 不丢数据、不死锁、不崩溃
// ================================================================
TEST(InputThread, HighConcurrencyNoDataLoss)
{
    std::mutex done_mutex;
    std::condition_variable done_cv;
    std::queue<int64_t> done_queue;
    LockQueue<std::string> input_queue;
    std::atomic<bool> running{true};

    std::atomic<int64_t> done_sum{0};     // 从 done_queue 消费的值的和
    std::atomic<int64_t> worker_sum{0};   // 工人推送的值的和
    std::atomic<int> input_count{0};      // 输入消费计数

    constexpr int WORKERS = 4;
    constexpr int INPUTS  = 2;
    constexpr int PER_THREAD = 1000;

    // 消费者
    std::thread consumer([&]() {
        while (running.load())
        {
            {
                std::unique_lock lock(done_mutex);
                done_cv.wait_for(lock, 5ms, [&]() {
                    return !done_queue.empty() || !input_queue.Empty() || !running.load();
                });
                while (!done_queue.empty())
                {
                    done_sum.fetch_add(done_queue.front());
                    done_queue.pop();
                }
            }
            std::unique_ptr<std::string> line;
            while (input_queue.TryPop(line))
                input_count.fetch_add(1);
        }
    });

    // 工人
    std::vector<std::thread> worker_threads;
    for (int w = 0; w < WORKERS; ++w)
    {
        worker_threads.emplace_back([&, w]() {
            for (int i = 0; i < PER_THREAD; ++i)
            {
                int64_t val = static_cast<int64_t>(w) * 1000000 + i;
                {
                    std::lock_guard lock(done_mutex);
                    done_queue.push(val);
                }
                done_cv.notify_one();
                worker_sum.fetch_add(val);
            }
        });
    }

    // 输入
    std::vector<std::thread> input_threads;
    for (int t = 0; t < INPUTS; ++t)
    {
        input_threads.emplace_back([&]() {
            for (int i = 0; i < PER_THREAD; ++i)
            {
                input_queue.Push(std::make_unique<std::string>("x"));
                done_cv.notify_one();
            }
        });
    }

    for (auto& t : worker_threads) t.join();
    for (auto& t : input_threads) t.join();

    // 最终 drain
    running.store(false);
    done_cv.notify_one();
    consumer.join();

    // 清空残留
    {
        std::unique_lock lock(done_mutex);
        while (!done_queue.empty())
        {
            done_sum.fetch_add(done_queue.front());
            done_queue.pop();
        }
    }
    {
        std::unique_ptr<std::string> line;
        while (input_queue.TryPop(line))
            input_count.fetch_add(1);
    }

    std::cout << "[TEST] HighConcurrency: done_sum=" << done_sum.load()
              << " worker_sum=" << worker_sum.load()
              << " input_count=" << input_count.load()
              << "\n";

    EXPECT_EQ(done_sum.load(), worker_sum.load())
        << "Sum of consumed values must match sum of produced (no data loss)";
    EXPECT_EQ(input_count.load(), INPUTS * PER_THREAD)
        << "All inputs must be consumed";
}
