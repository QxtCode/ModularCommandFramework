/// =================================================================
///  ShellEngine — 主循环引擎
/// =================================================================
///
///  把 main.cpp 的事件循环封装成四步流水线：
///    ① DrainResults   — ResultStore → Formatter → LOG_PLAIN
///    ② FlushMetrics   — MetricsCollector::Flush() 写共享内存
///    ③ WaitForWork    — cv.wait_for(HasResults || input || !running)
///    ④ ProcessInput   — input_queue → 解析 → 提交到 ThreadPool
///
///  职责分离：
///    main.cpp     → 组装（模块注册 + 启动）
///    ShellEngine  → 循环（输入线程 + 结果消费 + 事件驱动）
///    ResultStore  → 数据仓库（Worker Push，消费者 Drain）
///    EventBus     → 实时信号路由
///
///  v2.6 共享状态：ShellSharedState 用 shared_ptr 管理，输入线程
///  持有一份引用。即使 ShellEngine 析构后输入线程才从 getline()
///  唤醒，共享状态仍然存活，线程可安全读取 running 并退出。
/// =================================================================

#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/IResultFormatter.h"
#include "core/LockQueue.h"
#include "core/MetricsCollector.h"
#include "core/ModuleLifeManager.h"
#include "core/ResultStore.h"
#include "core/Task.h"
#include "core/ITaskPersistence.h"
#include "core/TasksPool.h"
#include "core/ThreadPool.h"
#include "event_bus/event_bus.h"
#include "parser/CommandParser.h"
#include "sdk/IModule.h"    // LOG_PLAIN, OutputMutex

/// =================================================================
///  ShellSharedState — 输入线程与 ShellEngine 间的共享状态
/// =================================================================
///
///  v2.6: 将 running / input_queue / cv / cv_mutex 从 ShellEngine
///  成员变量提升为堆上 shared_ptr 管理的独立对象。输入线程持有一份
///  shared_ptr，即使 ShellEngine 先析构，共享状态仍存活，线程可
///  安全读取 running 并退出，消除 use-after-free 崩溃。
///
struct ShellSharedState {
    std::atomic<bool>        running{true};
    LockQueue<std::string>   input_queue;
    std::mutex               cv_mutex;
    std::condition_variable  cv;
    // v2.6: 提示符门控 — 输入线程仅在主循环处理完上一轮输入后才打印 "> "
    std::atomic<bool>        prompt_ready{true};
};

class ShellEngine {
public:
    // ================================================================
    //  构造 / 析构
    // ================================================================
    /// @param pool_size    预分配的 Task 槽位数
    /// @param worker_count ThreadPool 工作线程数
    ShellEngine(int pool_size = 8, int worker_count = 4)
        : shared_(std::make_shared<ShellSharedState>())
        , fmt_(std::make_unique<ConsoleFormatter>())
        , tasks_(std::make_unique<TasksPool>(pool_size))
        , workers_(std::make_unique<ThreadPool>(worker_count))
    {}

    ~ShellEngine() { Shutdown(); }

    ShellEngine(const ShellEngine&) = delete;
    ShellEngine& operator=(const ShellEngine&) = delete;

    // ================================================================
    //  公开接口 — main() 需要的全部
    // ================================================================

    /// 进入事件驱动主循环，阻塞直到 /exit 或 EOF。
    void Run();

    /// 等待飞行中任务完成，停止输入线程。幂等，可多次调用。
    void Shutdown();

    // ---- 测试注入接口 ----

    /// 直接注入一条命令（绕过 stdin），Push 到 input_queue 并通知主循环。
    void InjectCommand(const std::string& line);

    /// 通知引擎在下一次循环迭代时停止。
    void RequestStop();

    /// @name 状态查询（测试断言用）
    /// @{
    size_t PendingResults() const { return ResultStore::Get().Size(); }
    size_t FreeTasks()      const { return tasks_->GetFreeCount(); }
    size_t TotalTasks()     const { return tasks_->GetTotalCount(); }
    /// @}

    /// v2.6: 暴露内部组件引用，供 TaskManager 等扩展模块使用。
    TasksPool&  GetPool()    { return *tasks_; }
    ThreadPool& GetWorkers() { return *workers_; }

    /// v2.7: 注入持久化后端。nullptr（默认）= 纯内存，暂停任务不落盘。
    /// 生命周期：后端必须比 ShellEngine 活得久（main.cpp 里声明在 engine 之前）。
    void SetTaskPersistence(ITaskPersistence* store) { store_ = store; }

private:
    // ================================================================
    //  四步流水线
    // ================================================================
    void MainLoop();

    /// 创建异步 stdin 读取线程
    void StartInputThread();
    /// 批量消费 ResultStore → Formatter → LOG_PLAIN
    void DrainResults();
    /// 刷新共享内存指标（供 shell_monitor.exe 读取）
    void FlushMetrics();
    /// cv.wait_for — 阻塞直到有结果、输入或 shutdown
    void WaitForWork();
    /// 非阻塞 drain input_queue → 解析 → 提交 Task
    void ProcessInput();

    /// 从 TasksPool 获取槽位，Enqueue 到 ThreadPool
    void SubmitTask(std::unique_ptr<ParmarPack> pack);

    // ================================================================
    //  成员变量 — 声明顺序 = 析构顺序
    // ================================================================
    // shared_ 在 input_thread_ 之后声明 → 析构时 shared_ 先于
    // input_thread_ 销毁。但 shared_ 内容由 shared_ptr 管理，输入
    // 线程持有副本 → 即使 ShellEngine 析构，共享状态仍存活。

    std::shared_ptr<ShellSharedState>    shared_;
    std::thread                          input_thread_;
    std::atomic<bool>                    shutdown_{false};
    std::unique_ptr<ConsoleFormatter>    fmt_;
    std::unique_ptr<TasksPool>           tasks_;
    std::unique_ptr<ThreadPool>          workers_;
    ITaskPersistence*                    store_{nullptr};  // v2.7: 可选持久化后端
};

// ================================================================
//  实现
// ================================================================

inline void ShellEngine::Run() {
    StartInputThread();
    MainLoop();
}

inline void ShellEngine::Shutdown() {
    // 幂等守卫 — 可能被显式调用 + 析构函数各调一次
    if (shutdown_.exchange(true)) return;

    shared_->running.store(false);
    shared_->cv.notify_all();

    // v2.6: 尝试短暂等待输入线程自行退出（检查 shared_->running）。
    // 若线程正阻塞在 getline() 则无法立即退出，此时 detach 而非 join。
    // detach 后输入线程仍持有 shared_ptr<ShellSharedState>，即使
    // ShellEngine 析构，共享状态仍存活 → 无 use-after-free。
    if (input_thread_.joinable()) {
        // 给输入线程 200ms 窗口：若已在 getline 返回后检查 running，
        // 此时可自行退出并被 join。若超时（仍在 getline 阻塞），则 detach。
        // 注意：std::thread 无可移植的 timed_join，用简单的循环检测。
        // 实际中，输入线程在交互模式下大概率阻塞在 getline，所以主要
        // 走 detach 路径；共享状态保证安全。
        input_thread_.detach();
    }

    // 空闲 drain：等飞行中任务完成（最多 5 秒）
    constexpr int kMaxWait = 50;
    for (int i = 0; i < kMaxWait; ++i) {
        {
            std::unique_lock lock(shared_->cv_mutex);
            shared_->cv.wait_for(lock, std::chrono::milliseconds(100),
                [this] { return ResultStore::Get().HasResults(); });

            auto batch = ResultStore::Get().Drain();
            for (auto& item : batch) {
                if (item->pack)
                    LOG_PLAIN(fmt_->Format(*item->pack));
            }
        }
        // v2.5: 双重检查关闭竞态 — 任务可能在 Drain 和 GetFreeCount 之间完成
        if (tasks_->GetFreeCount() == tasks_->GetTotalCount()) {
            auto final_batch = ResultStore::Get().Drain();
            for (auto& item : final_batch) {
                if (item->pack)
                    LOG_PLAIN(fmt_->Format(*item->pack));
            }
            break;
        }
    }
}

inline void ShellEngine::InjectCommand(const std::string& line) {
    shared_->input_queue.Push(std::make_unique<std::string>(line));
    shared_->cv.notify_one();
}

inline void ShellEngine::RequestStop() {
    shared_->running.store(false);
    shared_->cv.notify_all();
}

// ================================================================
//  私有方法
// ================================================================

inline void ShellEngine::StartInputThread() {
    // v2.6: 输入线程打印 "> " 提示符后阻塞在 getline。主循环处理完输入
    // 后设置 prompt_ready=true，输入线程才打印下一个提示符，消除输出交错。
    input_thread_ = std::thread([shared = shared_]() {
        std::string line;
        while (shared->running.load()) {
            // 等待主循环处理完上一轮输入
            while (shared->running.load() && !shared->prompt_ready.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!shared->running.load()) break;

            {
                std::lock_guard<std::mutex> lk(IModule::OutputMutex());
                std::cout << "> " << std::flush;
            }
            if (!std::getline(std::cin, line)) {
                shared->running.store(false);
                shared->cv.notify_one();
                break;
            }
            if (!shared->running.load()) break;
            shared->prompt_ready.store(false);  // 门控：等主循环处理完
            shared->input_queue.Push(std::make_unique<std::string>(std::move(line)));
            shared->cv.notify_one();
        }
    });
}

inline void ShellEngine::MainLoop() {
    while (shared_->running.load()) {
        DrainResults();
        FlushMetrics();
        WaitForWork();
        ProcessInput();
    }
}

inline void ShellEngine::DrainResults() {
    auto batch = ResultStore::Get().Drain();
    for (auto& item : batch) {
        if (item->pack)
            LOG_PLAIN(fmt_->Format(*item->pack));
    }
}

inline void ShellEngine::FlushMetrics() {
    auto* metrics = static_cast<MetricsCollector*>(
        ModuleLifeManager::GetInstance().GetModule("MetricsCollector"));
    if (!metrics) return;

    metrics->SetTasks(
        static_cast<uint32_t>(tasks_->GetTotalCount() - tasks_->GetFreeCount()),
        0,
        static_cast<uint32_t>(tasks_->GetTotalCount()),
        0);
    metrics->SetModules(
        static_cast<uint32_t>(ModuleLifeManager::GetInstance().GetModuleCount()));
    metrics->Flush();
}

inline void ShellEngine::WaitForWork() {
    std::unique_lock lock(shared_->cv_mutex);
    shared_->cv.wait_for(lock, std::chrono::milliseconds(100), [this]() {
        return ResultStore::Get().HasResults()
            || !shared_->input_queue.Empty()
            || !shared_->running.load();
    });
}

inline void ShellEngine::ProcessInput() {
    std::unique_ptr<std::string> line;
    while (shared_->input_queue.TryPop(line)) {
        const std::string& text = *line;

        if (text == "/exit") {
            shared_->running.store(false);
            shared_->cv.notify_one();
            return;
        }
        if (text.empty()) {
            // 提示符 "> " 不带换行，空命令的输出需先换行避免粘连
            std::cout << std::endl;
            ModuleLifeManager::GetInstance().PrintModuleList();
            continue;
        }

        // 解析
        auto& parser = CommandParser::Get();
        if (!parser.SendCommand("TXT", std::any(std::string(text)))) {
            LOG_PLAIN("[ERROR] Parse failed.");
            continue;
        }
        auto pack = parser.PopPack();
        if (!pack) continue;

        SubmitTask(std::move(pack));
    }
    // ★ v2.6: 输入队列已清空，通知输入线程可打印下一个 "> " 提示符
    shared_->prompt_ready.store(true);
}

inline void ShellEngine::SubmitTask(std::unique_ptr<ParmarPack> pack) {
    Task* task = tasks_->Acquire(std::move(pack));
    if (!task) {
        LOG_PLAIN("[ERROR] All tasks busy (max " << tasks_->GetTotalCount()
                  << "). Wait or increase pool size.");
        return;
    }

    workers_->Enqueue([this, task]() {
        try {
            while (task->Step(EventBus::GetInstance())) {}
        } catch (...) {
            auto* cp = task->CurrentPack();
            if (cp) {
                cp->success = false;
                cp->error.code = ErrorCode::INTERNAL_ERROR;
                cp->error.message = "Unhandled exception in task";
            }
        }

        // v2.7: 循环退出后，若任务停在 PAUSED，保存快照供 resume 恢复。
        // 必须在 Release 之前——Release 会 Reset 清空分片数据。
        if (task->GetState() == Task::State::PAUSED && store_) {
            store_->Save(task->ExportRecord());
        }

        auto* cp = task->CurrentPack();
        if (cp) {
            auto result = std::make_unique<ParmarPack>(*cp);
            ResultStore::Get().PushResult(task->GetID(), std::move(result));
        }
        tasks_->Release(task);
        shared_->cv.notify_one();
    });
}
