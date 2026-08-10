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
///  析构顺序：cv_mutex_/cv_ 声明在最前，在 workers_ 之后销毁。
///  保证 workers_ 析构 join 时 worker lambda 调 cv_.notify_one() 安全。
/// =================================================================

#pragma once
#include <atomic>
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
#include "core/TasksPool.h"
#include "core/ThreadPool.h"
#include "event_bus/event_bus.h"
#include "parser/CommandParser.h"
#include "sdk/IModule.h"    // LOG_PLAIN, OutputMutex

class ShellEngine {
public:
    // ================================================================
    //  构造 / 析构
    // ================================================================
    /// @param pool_size    预分配的 Task 槽位数
    /// @param worker_count ThreadPool 工作线程数
    ShellEngine(int pool_size = 8, int worker_count = 4)
        : tasks_(std::make_unique<TasksPool>(pool_size))
        , workers_(std::make_unique<ThreadPool>(worker_count))
        , fmt_(std::make_unique<ConsoleFormatter>())
        , running_(true)
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

    /// 直接注入一条命令（绕过 stdin），Push 到 input_queue_ 并通知主循环。
    void InjectCommand(const std::string& line);

    /// 通知引擎在下一次循环迭代时停止。
    void RequestStop();

    /// @name 状态查询（测试断言用）
    /// @{
    size_t PendingResults() const { return ResultStore::Get().Size(); }
    size_t FreeTasks()      const { return tasks_->GetFreeCount(); }
    size_t TotalTasks()     const { return tasks_->GetTotalCount(); }
    /// @}

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
    /// 非阻塞 drain input_queue_ → 解析 → 提交 Task
    void ProcessInput();

    /// 从 TasksPool 获取槽位，Enqueue 到 ThreadPool
    void SubmitTask(std::unique_ptr<ParmarPack> pack);

    // ================================================================
    //  成员变量 — 声明顺序 = 析构顺序
    // ================================================================
    // cv_mutex_/cv_ 声明在最前 → 最后析构，保证 workers_ join 时可用

    std::mutex                    cv_mutex_;
    std::condition_variable       cv_;

    LockQueue<std::string>        input_queue_;
    std::thread                   input_thread_;
    std::atomic<bool>             running_;

    std::atomic<bool>             shutdown_{false};
    std::unique_ptr<ConsoleFormatter>      fmt_;
    std::unique_ptr<TasksPool>             tasks_;
    std::unique_ptr<ThreadPool>            workers_;
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

    running_.store(false);
    cv_.notify_all();
    if (input_thread_.joinable())
        // detach 而非 join：输入线程可能阻塞在 getline(std::cin)，
        // 此时 stdin 无数据则线程永不返回，join 会永久阻塞主线程。
        // detach 后线程继续阻塞在 getline，进程退出时 OS 自动回收。
        input_thread_.detach();

    // 空闲 drain：等飞行中任务完成（最多 5 秒）
    constexpr int kMaxWait = 50;
    for (int i = 0; i < kMaxWait; ++i) {
        {
            std::unique_lock lock(cv_mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(100),
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
    input_queue_.Push(std::make_unique<std::string>(line));
    cv_.notify_one();
}

inline void ShellEngine::RequestStop() {
    running_.store(false);
    cv_.notify_all();
}

// ================================================================
//  私有方法
// ================================================================

inline void ShellEngine::StartInputThread() {
    input_thread_ = std::thread([this]() {
        std::string line;
        while (running_.load()) {
            {
                std::lock_guard<std::mutex> lk(IModule::OutputMutex());
                std::cout << "> " << std::flush;
            }
            if (!std::getline(std::cin, line)) {
                running_.store(false);
                cv_.notify_one();
                break;
            }
            input_queue_.Push(std::make_unique<std::string>(std::move(line)));
            cv_.notify_one();
        }
    });
}

inline void ShellEngine::MainLoop() {
    while (running_.load()) {
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
    std::unique_lock lock(cv_mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
        return ResultStore::Get().HasResults()
            || !input_queue_.Empty()
            || !running_.load();
    });
}

inline void ShellEngine::ProcessInput() {
    std::unique_ptr<std::string> line;
    while (input_queue_.TryPop(line)) {
        const std::string& text = *line;

        if (text == "/exit") {
            running_.store(false);
            cv_.notify_one();
            return;
        }
        if (text.empty()) {
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
        auto* cp = task->CurrentPack();
        if (cp) {
            auto result = std::make_unique<ParmarPack>(*cp);
            ResultStore::Get().PushResult(task->GetID(), std::move(result));
        }
        tasks_->Release(task);
        cv_.notify_one();
    });
}
