/// =================================================================
///  MetricsCollector — 跨平台框架指标收集器
/// =================================================================
///
///  即插即用 IModule。在 main.cpp 中：
///    mgr.AddModule(std::make_unique<MetricsCollector>());
///
///  主循环每次迭代通过 Flush() 将框架状态快照写入共享内存。
///  外部 shell_monitor 进程通过 platform::SharedMemory 读取。
///
///  跨平台：Windows 用 File Mapping，Linux/macOS 用 shm_open+mmap，
///  由 core/platform/shared_memory.h 统一封装。
///
#pragma once
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <thread>

#include "core/ModuleBaseObject.h"
#include "core/platform/platform.h"
#include "core/platform/file_system.h"
#include "core/platform/process.h"
#include "core/platform/shared_memory.h"
#include "monitor/MetricsProtocol.h"

class MetricsCollector : public ModuleBaseObject
{
public:
    const char* GetName() const override { return "MetricsCollector"; }

    ~MetricsCollector() override {
        StopTimer();
        // shm_ destructor handles cleanup; OnShutdown() already set magic=0
    }

    bool OnInit() override
    {
        // Create or open named shared memory (one 4KB page)
        shm_ = platform::SharedMemory::Create(monitor::kShmName, 4096);
        if (!shm_) {
            std::cerr << "[MetricsCollector] Cannot create shared memory" << std::endl;
            return false;
        }

        shm_data_ = static_cast<monitor::MetricsData*>(shm_->Data());

        // Init header
        std::memset(shm_data_, 0, sizeof(monitor::MetricsData));
        shm_data_->magic = monitor::kMagic;
        shm_data_->seq   = 0;

        auto t0 = std::chrono::steady_clock::now();
        start_ms_ = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                t0.time_since_epoch()).count());

        // v2.7: CPU 采集已从 TimerLoop 搬到 Flush（主循环调用）。
        // 首次 Flush 不计算（无前值），cpu_percent = 0 是正常的。

        REGISTER_FUNC("show", "Show in-terminal metrics dashboard", {
            auto* m = GetMetrics();
            if (!m) { pack->success = false; pack->error.message = "Metrics not available"; return; }
            monitor::MetricsData snap{};
            if (!monitor::TryRead(m, &snap)) {
                pack->success = false;
                pack->error.message = "Cannot read metrics (try again)";
                return;
            }
            char buf[1024];
            snprintf(buf, sizeof(buf),
                "=== test_shell v2.7 Dashboard ===\n"
                "  CPU:     %u.%02u%%\n"
                "  Memory:  %llu KB\n"
                "  Threads: %u workers / %u total\n"
                "  Tasks:   %u active / %u total (%u completed)\n"
                "  Modules: %u loaded\n"
                "  Signals: %u registered\n"
                "  Uptime:  %llus\n"
                "  Stuck:   %u threads\n"
                "=== Events ===\n%s",
                snap.cpu_percent / 100, snap.cpu_percent % 100,
                (unsigned long long)snap.working_set_kb,
                snap.worker_threads, snap.thread_count,
                snap.tasks_active, snap.tasks_total, snap.tasks_completed,
                snap.modules_loaded,
                snap.signals_count,
                (unsigned long long)(snap.uptime_ms / 1000),
                snap.stuck_threads,
                snap.events[0]
            );
            pack->return_value = buf;
            pack->success = true;
        });

        REGISTER_FUNC("metrics", "Show current metrics snapshot", {
            auto* m = GetMetrics();
            if (!m) { pack->success = false; pack->error.message = "MetricsCollector not initialized"; return; }
            char buf[512];
            snprintf(buf, sizeof(buf),
                "Uptime:%llus Tasks:%u/%u Modules:%u Signals:%u Threads:%u Stuck:%u",
                (unsigned long long)(m->uptime_ms / 1000),
                m->tasks_active, m->tasks_total,
                m->modules_loaded, m->signals_count,
                m->worker_threads, m->stuck_threads);
            pack->return_value = buf;
            pack->success = true;
        });

        REGISTER_FUNC("dashboard", "Launch external FTXUI dashboard (opens new window)", {
            std::string dir = platform::Process::ExeDir();
            std::string monitor_path = dir + "shell_monitor";
            if (PLATFORM_WINDOWS) monitor_path += ".exe";  // compile-time constant

            if (!platform::FileExists(monitor_path)) {
                pack->success = false;
                pack->error.code = ErrorCode::INTERNAL_ERROR;
                pack->error.message = "shell_monitor not found at: " + monitor_path;
                return;
            }

            if (platform::Process::Launch(monitor_path, "", dir,
                                          PLATFORM_WINDOWS)) {  // newConsole on Windows
                pack->success = true;
                pack->return_value = "Dashboard launched";
            } else {
                pack->success = false;
                pack->error.code = ErrorCode::INTERNAL_ERROR;
                pack->error.message = "Failed to launch shell_monitor";
            }
        });

        return true;
    }

    void OnShutdown() override {
        StopTimer();
        if (shm_data_) {
            shm_data_->magic = 0;
            shm_data_ = nullptr;  // prevent dangling pointer access in dtor
        }
        shm_.reset();
    }

    /// Expose shared memory pointer for direct read by external code (tests)
    monitor::MetricsData* GetMetrics() const { return shm_data_; }

    // Setters for framework stats (called by main loop / other components)

    /// Push an event into the ring buffer (thread-safe via BeginWrite/EndWrite)
    void PushEvent(const char* fmt, ...)
    {
        if (!shm_data_) return;
        char buf[monitor::kEventSize];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        monitor::BeginWrite(shm_data_);
        uint32_t idx = shm_data_->event_head % monitor::kMaxEvents;
        std::strncpy(shm_data_->events[idx], buf, monitor::kEventSize - 1);
        shm_data_->event_head++;
        monitor::EndWrite(shm_data_);
    }

    // Setters for framework stats (called by main loop / other components)
    void SetTasks(uint32_t active, uint32_t queued, uint32_t total, uint32_t completed) {
        tasks_active_    = active;
        tasks_queued_    = queued;
        tasks_total_     = total;
        tasks_completed_ = completed;
    }
    void SetModules(uint32_t loaded)  { modules_loaded_  = loaded; }
    void SetSignals(uint32_t count)   { signals_count_   = count; }
    void SetThreads(uint32_t workers, uint32_t stuck) {
        worker_threads_ = workers;
        stuck_threads_  = stuck;
    }

    /// Write current state to shared memory (called from main loop)
    void Flush() {
        if (!shm_data_) return;
        using namespace std::chrono;
        auto now = steady_clock::now().time_since_epoch();
        auto ms  = duration_cast<milliseconds>(now).count();

        // ---- CPU 采集（v2.7: 从 TimerLoop 搬到主循环） ----
        uint32_t cpu = 0;
        uint64_t cur_cpu = platform::Process::CpuTimeUs();
        if (last_cpu_us_ > 0 && cur_cpu >= last_cpu_us_ && last_flush_ms_ > 0)
        {
            uint64_t delta_cpu_us  = cur_cpu - last_cpu_us_;
            uint64_t delta_wall_ms = static_cast<uint64_t>(ms) - last_flush_ms_;
            if (delta_wall_ms > 0)
            {
                // cpu_percent 是百分之一单位（如 2450 = 24.50%）
                // = cpu_used_us / wall_us * 10000
                uint64_t delta_wall_us = delta_wall_ms * 1000;
                cpu = static_cast<uint32_t>(
                    (static_cast<double>(delta_cpu_us) / delta_wall_us) * 10000.0);
            }
        }
        last_cpu_us_  = cur_cpu;
        last_flush_ms_ = static_cast<uint64_t>(ms);

        monitor::BeginWrite(shm_data_);
        shm_data_->uptime_ms       = static_cast<uint64_t>(ms - start_ms_);
        shm_data_->cpu_percent     = cpu;
        shm_data_->working_set_kb  = 0;
        shm_data_->thread_count    = static_cast<uint32_t>(std::thread::hardware_concurrency());
        shm_data_->worker_threads  = worker_threads_;
        shm_data_->stuck_threads   = stuck_threads_;
        shm_data_->tasks_active    = tasks_active_;
        shm_data_->tasks_queued    = tasks_queued_;
        shm_data_->tasks_total     = tasks_total_;
        shm_data_->tasks_completed = tasks_completed_;
        shm_data_->modules_loaded  = modules_loaded_;
        shm_data_->signals_count   = signals_count_;
        shm_data_->dll_handles     = 0;
        shm_data_->zombie_signals  = 0;
        shm_data_->last_error[0]   = '\0';
        shm_data_->top_module[0]   = '\0';
        monitor::EndWrite(shm_data_);
    }

private:
    void StopTimer() {
        running_.store(false);
        if (timer_.joinable()) timer_.join();
    }

    void TimerLoop()
    {
        if (!shm_data_) return;
        using namespace std::chrono;
        auto t0 = steady_clock::now();
        uint64_t last_cpu = platform::Process::CpuTimeUs();
        uint32_t c = 0;
        while (running_.load())
        {
            std::this_thread::sleep_for(milliseconds(500));
            if (!running_.load() || !shm_data_) break;
            auto now = steady_clock::now();
            c++;
            monitor::BeginWrite(shm_data_);
            // CPU via platform::Process
            uint64_t cur_cpu = platform::Process::CpuTimeUs();
            uint32_t cpu = 0;
            if (last_cpu) {
                uint64_t delta_us = cur_cpu - last_cpu;
                // Original formula: delta_100ns / 50000
                // delta_us = delta_100ns / 10, so: delta_us / 5000
                cpu = static_cast<uint32_t>(delta_us / 5000);
            }
            last_cpu = cur_cpu;

            shm_data_->uptime_ms       = duration_cast<milliseconds>(now - t0).count();
            shm_data_->cpu_percent     = cpu;
            shm_data_->working_set_kb  = 0;
            shm_data_->thread_count    = static_cast<uint32_t>(std::thread::hardware_concurrency());
            shm_data_->worker_threads  = worker_threads_;
            shm_data_->stuck_threads   = stuck_threads_;
            shm_data_->tasks_active    = tasks_active_;
            shm_data_->tasks_queued    = tasks_queued_;
            shm_data_->tasks_total     = tasks_total_;
            shm_data_->tasks_completed = c;
            shm_data_->modules_loaded  = modules_loaded_;
            shm_data_->signals_count   = signals_count_;
            shm_data_->dll_handles     = 0;
            shm_data_->zombie_signals  = 0;
            shm_data_->last_error[0]   = '\0';
            shm_data_->top_module[0]   = '\0';
            monitor::EndWrite(shm_data_);
        }
    }

    std::unique_ptr<platform::SharedMemory> shm_;
    monitor::MetricsData* shm_data_ = nullptr;

    std::thread timer_;
    std::atomic<bool> running_{false};
    uint64_t start_ms_      = 0;
    uint64_t last_cpu_us_   = 0;  // v2.7: CPU 差量采样前值
    uint64_t last_flush_ms_ = 0;  // v2.7: 上次 Flush 时间戳

    std::atomic<uint32_t> tasks_active_{0};
    std::atomic<uint32_t> tasks_queued_{0};
    std::atomic<uint32_t> tasks_total_{0};
    std::atomic<uint32_t> tasks_completed_{0};
    std::atomic<uint32_t> modules_loaded_{0};
    std::atomic<uint32_t> signals_count_{0};
    std::atomic<uint32_t> worker_threads_{0};
    std::atomic<uint32_t> stuck_threads_{0};
};
