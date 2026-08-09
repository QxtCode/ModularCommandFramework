/// =================================================================
///  shell_monitor.exe — external dashboard for test_shell framework
/// =================================================================
#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "MetricsProtocol.h"

using namespace ftxui;
using namespace monitor;
using namespace std::chrono_literals;

// ---- Connect to shared memory ----
// Keep handle alive so shared memory persists even if framework exits
static HANDLE g_shm_handle = nullptr;

static MetricsData* ConnectToSharedMemory() {
#ifdef _WIN32
    g_shm_handle = OpenFileMappingW(FILE_MAP_READ, FALSE, kShmName);
    if (!g_shm_handle) return nullptr;
    auto* d = (MetricsData*)MapViewOfFile(g_shm_handle, FILE_MAP_READ, 0, 0, 4096);
    // DON'T CloseHandle — keep it alive so the kernel object persists
    return d;
#else
    return nullptr;
#endif
}

// ---- Format uptime ----
static std::string FmtUptime(uint64_t ms) {
    uint64_t s = ms / 1000, m = s / 60, h = m / 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lluh%llum%llus",
             (unsigned long long)h, (unsigned long long)(m % 60), (unsigned long long)(s % 60));
    return buf;
}

// ---- Color gauge ----
static Color GColor(float v) {
    if (v < 0.5f) return Color::Green;
    if (v < 0.8f) return Color::Yellow;
    return Color::Red;
}

static Element Bar(const std::string& label, float val) {
    auto b = gauge(val) | size(WIDTH, GREATER_THAN, 18);
    auto t = text(" " + std::to_string((int)(val * 100)) + "%");
    return hbox({ text(label + " ") | size(WIDTH, EQUAL, 8), b | color(GColor(val)), t });
}

static Element Line(const std::string& label, const std::string& val, Color c = Color::Default) {
    return hbox({ text(label + " ") | dim, text(val) | color(c) });
}

// ---- Main ----
int main(int argc, char** argv) {
    int refresh_ms = 500;
    if (argc > 1) refresh_ms = atoi(argv[1]);
    if (refresh_ms < 100) refresh_ms = 100;

    MetricsData* shm = ConnectToSharedMemory();
    if (!shm) {
        std::cerr << "[monitor] Shared memory not found. Is test_shell.exe running?\n";
        return 1;
    }
    std::cout << "[monitor] Connected. q=quit p=pause\n";

    MetricsData snap{};
    bool     snap_valid = false;
    uint32_t last_seq   = 0;
    int      stale_cnt  = 0;
    int      no_sig_cnt = 0;
    bool     paused     = false;

    auto screen = ScreenInteractive::Fullscreen();
    std::atomic<bool> running{true};

    // ---- Render function (v2.4: raw magic + snap_valid) ----
    auto render = [&] {
        // ★ Step 1: liveness check from RAW shared memory (not cached snap)
        uint32_t raw_magic = shm->magic;

        if (raw_magic != kMagic) {
            no_sig_cnt++;
            if (no_sig_cnt >= 4) {
                snap_valid = false;
                memset(&snap, 0, sizeof(snap));
            }
        } else {
            no_sig_cnt = 0;

            // Step 2: try to read fresh data
            bool got_fresh = false;
            for (int r = 0; r < 3; ++r) {
                MetricsData tmp{};
                if (TryRead(shm, &tmp) && tmp.seq != last_seq) {
                    snap = tmp;
                    snap_valid = true;
                    last_seq = tmp.seq;
                    stale_cnt = 0;
                    got_fresh = true;
                    break;
                }
            }
            if (!got_fresh) {
                stale_cnt++;
                // Keep old snap — don't clear on temporary failure
                if (stale_cnt >= 8) {
                    snap_valid = false;
                    memset(&snap, 0, sizeof(snap));
                }
            }
        }

        // ---- If no valid data, show NO SIGNAL ----
        if (!snap_valid) {
            auto msg = text(" NO SIGNAL — framework not running ")
                     | color(Color::Red) | bold | center;
            return vbox({ text(" test_shell Monitor ") | bold | center,
                          separator(), msg }) | border;
        }

        float cpu    = snap.cpu_percent / 10000.0f;
        if (cpu > 1.0f) cpu = 1.0f;
        float mem    = snap.working_set_kb / 1024.0f / 100.0f;
        if (mem > 1.0f) mem = 1.0f;
        float tRatio = snap.tasks_total > 0
            ? (float)snap.tasks_active / snap.tasks_total : 0.0f;

        // Alert line
        Element alert = emptyElement();
        if (snap.stuck_threads > 0)
            alert = text("STUCK: " + std::to_string(snap.stuck_threads) + " threads") | color(Color::Red) | bold;
        else if (tRatio > 0.8f)
            alert = text("POOL PRESSURE") | color(Color::Yellow) | bold;

        // Status line
        std::string st;
        if (paused)                     st = " PAUSED ";
        else if (stale_cnt >= 4)        st = " STALE (seq stuck " + std::to_string(stale_cnt) + " frames) ";
        else                            st = " LIVE seq=" + std::to_string(snap.seq);
        auto status = text(st) | dim | center;

        // Resource panel
        auto res = window(text(" Resources "), vbox({
            Bar("CPU", cpu),
            Bar("MEM", mem),
            Line("Threads:", std::to_string(snap.worker_threads) + "/" + std::to_string(snap.thread_count)),
        }));

        // Framework panel
        auto fw = window(text(" Framework "), vbox({
            Bar("Tasks", tRatio),
            Line("Active:", std::to_string(snap.tasks_active) + "/" + std::to_string(snap.tasks_total)),
            Line("Done:", std::to_string(snap.tasks_completed)),
            Line("Modules:", std::to_string(snap.modules_loaded)),
            Line("Signals:", std::to_string(snap.signals_count)),
            Line("Uptime:", FmtUptime(snap.uptime_ms)),
        }));

        // Event log
        Elements ev;
        for (uint32_t i = 0; i < kMaxEvents; ++i) {
            uint32_t idx = (snap.event_head > 0)
                ? (snap.event_head - 1 - i) % kMaxEvents : 0;
            if (snap.events[idx][0])
                ev.push_back(text(std::string(snap.events[idx])) | dim);
        }
        auto evp = window(text(" Events "), vbox(std::move(ev)) | size(HEIGHT, GREATER_THAN, 5));

        return vbox({
            text(" test_shell v2.4 Monitor ") | bold | center,
            separator(),
            alert,
            hbox({ res | flex, fw | flex }),
            evp | flex,
            separator(),
            status,
        }) | border;
    };

    // ---- Component with keyboard ----
    auto comp = Renderer(render);
    comp = CatchEvent(comp, [&](Event e) {
        if (e == Event::Character('q') || e == Event::Escape) {
            screen.Exit(); return true;
        }
        if (e == Event::Character('p')) {
            paused = !paused; return true;
        }
        // Event::Custom = refresh tick from background thread.
        // Must return true so FTXUI re-renders the screen.
        if (e == Event::Custom) return true;
        return false;
    });

    // ---- Refresh thread ----
    std::thread refresh([&]() {
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(refresh_ms));
            if (!paused) screen.PostEvent(Event::Custom);
        }
    });

    screen.Loop(comp);

    running.store(false);
    if (refresh.joinable()) refresh.join();
    if (shm) UnmapViewOfFile(shm);
    std::cout << "[monitor] Bye.\n";
    return 0;
}
