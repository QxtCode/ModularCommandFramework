/// simulate_load — 负载场景模拟器（跨平台）
/// 循环切换负载场景供 shell_monitor 仪表盘观察。
/// 通过 platform::SharedMemory 写入 MetricsData。
/// 构建: cmake --build --preset debug --target simulate_load

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>

#include "core/platform/platform.h"
#include "core/platform/shared_memory.h"
#include "MetricsProtocol.h"

using namespace monitor;
using namespace std::chrono_literals;

int main() {
    std::cout << "=== Load Simulator ===\n";
    std::cout << "Cycling through scenarios. Watch your shell_monitor.exe dashboard!\n\n";

    // Connect to existing shared memory (test_shell.exe must be running)
    auto shm_obj = platform::SharedMemory::Create(kShmName, 4096);
    if (!shm_obj) {
        std::cerr << "Cannot create shared memory\n";
        return 1;
    }
    auto* shm = static_cast<MetricsData*>(shm_obj->Data());
    memset(shm, 0, sizeof(MetricsData));
    shm->magic = kMagic;
    shm->seq   = 0;

    uint64_t uptime = 0;
    uint32_t completed = 0;

    struct Scene {
        const char* name;
        uint32_t active, total, modules, workers, stuck;
        int seconds;
    };

    Scene scenes[] = {
        {"IDLE (no load)",            0,  8, 3, 4, 0, 5},
        {"MODERATE (few tasks)",      3,  8, 3, 4, 0, 5},
        {"HEAVY (many tasks)",        7,  8, 3, 4, 0, 5},
        {"POOL PRESSURE (almost full)",8, 8, 4, 4, 0, 5},
        {"STUCK THREAD!",            5,  8, 4, 4, 2, 5},
        {"RECOVERED (back to normal)",2,  8, 4, 4, 0, 5},
    };

    for (auto& sc : scenes) {
        std::cout << "  [" << sc.name << "] " << sc.seconds << "s\n";
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(sc.seconds);

        while (std::chrono::steady_clock::now() < end) {
            BeginWrite(shm);
            shm->uptime_ms       = uptime;
            shm->tasks_active    = sc.active;
            shm->tasks_total     = sc.total;
            shm->tasks_completed = completed;
            shm->modules_loaded  = sc.modules;
            shm->signals_count   = sc.modules * 2 + 2;
            shm->worker_threads  = sc.workers;
            shm->stuck_threads   = sc.stuck;
            shm->thread_count    = 16;
            shm->cpu_percent     = sc.active * 1200;  // rough %
            shm->working_set_kb = 6000 + sc.active * 200;

            // Push event
            uint32_t ei = (shm->event_head) % kMaxEvents;
            snprintf(shm->events[ei], kEventSize,
                     "[%02llu:%02llu:%02llu] %s",
                     (uptime / 3600000) % 24,
                     (uptime / 60000) % 60,
                     (uptime / 1000) % 60,
                     sc.name);
            shm->event_head++;

            EndWrite(shm);
            uptime += 500;
            completed += sc.active;
            std::this_thread::sleep_for(500ms);
        }
    }

    // Signal shutdown
    shm->magic = 0;
    // shm_obj destructor handles cleanup

    std::cout << "\nDone! Monitor should show NO SIGNAL now.\n";
    std::cout << "(Restart test_shell.exe to clear it)\n";
    return 0;
}
