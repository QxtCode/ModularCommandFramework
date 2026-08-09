/// =================================================================
///  MetricsCollector — writes framework metrics to shared memory
/// =================================================================
///
///  Plug-and-play IModule. Add it in main.cpp:
///    mgr.AddModule(std::make_unique<MetricsCollector>());
///
///  Every ~500ms it snapshots framework state into a MetricsData
///  struct in shared memory. An external shell_monitor.exe reads it.
///
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "core/ModuleBaseObject.h"
#include "monitor/MetricsProtocol.h"
#include <cstdarg>

class MetricsCollector : public ModuleBaseObject
{
public:
    const char* GetName() const override { return "MetricsCollector"; }

    ~MetricsCollector() override {
        // Stop timer thread first, then clean up
        StopTimer();
        if (shm_data_) { shm_data_->magic = 0; }
    }

    bool OnInit() override
    {
#ifdef _WIN32
        // Create named shared memory (one 4KB page)
        // If it already exists (from shell_monitor or simulate_load),
        // just open it — don't fail.
        shm_handle_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, 4096, monitor::kShmName);
        if (!shm_handle_) {
            // Try opening existing read-only mapping
            shm_handle_ = OpenFileMappingW(FILE_MAP_WRITE, FALSE, monitor::kShmName);
            if (!shm_handle_) {
                std::cerr << "[MetricsCollector] Cannot create or open shared memory (err="
                          << GetLastError() << ")" << std::endl;
                return false;
            }
        }

        shm_data_ = static_cast<monitor::MetricsData*>(
            MapViewOfFile(shm_handle_, FILE_MAP_WRITE, 0, 0, 4096));
        if (!shm_data_) {
            std::cerr << "[MetricsCollector] MapViewOfFile failed (err="
                      << GetLastError() << ")" << std::endl;
            return false;
        }

        // Init header
        std::memset(shm_data_, 0, sizeof(monitor::MetricsData));
        shm_data_->magic = monitor::kMagic;
        shm_data_->seq   = 0;

        auto t0 = std::chrono::steady_clock::now();
        start_ms_ = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                t0.time_since_epoch()).count());

        // v2.5: Metrics updated from main loop via SetTasks/etc.
        // Background timer disabled — main loop FlushMetrics is sufficient.

        REGISTER_FUNC("show", "Show in-terminal metrics dashboard", {
            auto* m = GetMetrics();
            if (!m) { pack->success = false; pack->error.message = "Metrics not available"; return; }
            // Read a fresh snapshot from shared memory
            monitor::MetricsData snap{};
            if (!monitor::TryRead(m, &snap)) {
                pack->success = false;
                pack->error.message = "Cannot read metrics (try again)";
                return;
            }
            char buf[1024];
            snprintf(buf, sizeof(buf),
                "=== test_shell v2.5 Dashboard ===\n"
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
            // 获取 shell_monitor.exe 路径（与 test_shell.exe 同目录）
            char exe_path[MAX_PATH];
            GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
            std::string dir(exe_path);
            dir = dir.substr(0, dir.find_last_of("\\/") + 1);
            std::string monitor_path = dir + "shell_monitor.exe";

            // 检查文件是否存在
            if (GetFileAttributesA(monitor_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                pack->success = false;
                pack->error.code = ErrorCode::INTERNAL_ERROR;
                pack->error.message = "shell_monitor.exe not found at: " + monitor_path;
                return;
            }

            // 启动独立进程（新控制台窗口）
            STARTUPINFOA si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            std::string cmd = monitor_path;  // CreateProcess 可能修改，拷贝一份

            if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr,
                              FALSE, CREATE_NEW_CONSOLE, nullptr,
                              dir.c_str(), &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                pack->success = true;
                pack->return_value = "Dashboard launched in new window (close with q)";
            } else {
                pack->success = false;
                pack->error.code = ErrorCode::INTERNAL_ERROR;
                pack->error.message = "Failed to launch shell_monitor.exe (err="
                    + std::to_string(GetLastError()) + ")";
            }
        });

        return true;
#else
        return false;  // shared memory = Windows only
#endif
    }

    void OnShutdown() override {
        StopTimer();
        if (shm_data_) shm_data_->magic = 0;
    }

    /// Expose shared memory pointer for direct read by external code (tests)
    monitor::MetricsData* GetMetrics() const { return shm_data_; }

    // Setters for framework stats (called by main loop / other components)

    /// Push an event into the ring buffer (thread-safe via BeginWrite/EndWrite)
    void PushEvent(const char* fmt, ...)
    {
#ifdef _WIN32
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
#endif
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
#ifdef _WIN32
        if (!shm_data_) return;
        using namespace std::chrono;
        auto now = steady_clock::now().time_since_epoch();
        auto ms  = duration_cast<milliseconds>(now).count();
        monitor::BeginWrite(shm_data_);
        shm_data_->uptime_ms       = static_cast<uint64_t>(ms - start_ms_);
        shm_data_->cpu_percent     = 0;
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
#endif
    }

private:
    void StopTimer() {
        running_.store(false);
        if (timer_.joinable()) timer_.join();
    }

#ifdef _WIN32
    void TimerLoop()
    {
        if (!shm_data_) return;  // safety: shared memory not initialized
        using namespace std::chrono;
        auto t0 = steady_clock::now();
        uint32_t c = 0;
        while (running_.load())
        {
            std::this_thread::sleep_for(milliseconds(500));
            if (!running_.load() || !shm_data_) break;
            auto now = steady_clock::now();
            c++;
            monitor::BeginWrite(shm_data_);
            // CPU via GetProcessTimes
            FILETIME fk, fu;
            GetProcessTimes(GetCurrentProcess(), nullptr, nullptr, &fk, &fu);
            static uint64_t lk = 0, lu = 0;
            uint64_t k = ((uint64_t)fk.dwHighDateTime << 32) | fk.dwLowDateTime;
            uint64_t u = ((uint64_t)fu.dwHighDateTime << 32) | fu.dwLowDateTime;
            uint32_t cpu = 0;
            if (lk) cpu = uint32_t(((k - lk) + (u - lu)) / (500 * 100));
            lk = k; lu = u;

            shm_data_->uptime_ms       = duration_cast<milliseconds>(now - t0).count();
            shm_data_->cpu_percent     = cpu;
            shm_data_->working_set_kb  = 0;  // TODO: GetProcessMemoryInfo
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

    HANDLE shm_handle_ = nullptr;
    monitor::MetricsData* shm_data_ = nullptr;
#endif

    std::thread timer_;
    std::atomic<bool> running_{false};
    uint64_t start_ms_ = 0;

    std::atomic<uint32_t> tasks_active_{0};
    std::atomic<uint32_t> tasks_queued_{0};
    std::atomic<uint32_t> tasks_total_{0};
    std::atomic<uint32_t> tasks_completed_{0};
    std::atomic<uint32_t> modules_loaded_{0};
    std::atomic<uint32_t> signals_count_{0};
    std::atomic<uint32_t> worker_threads_{0};
    std::atomic<uint32_t> stuck_threads_{0};
};
