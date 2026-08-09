/// =================================================================
///  MetricsProtocol.h — shared memory protocol
/// =================================================================
///
///  Included by BOTH test_shell.exe AND shell_monitor.exe.
///  Defines the shared memory layout used for cross-process metrics.
///
///  Versioned snapshot pattern (simpler than seqlock, no volatile):
///    Writer: seq++ (odd) → memcpy data → seq++ (even)
///    Reader: while(seq odd){} → s1=seq → memcpy snapshot → s2=seq
///            if s1 != s2 → retry
///
///  In-process: fences ensure cross-thread visibility.
///  Cross-process: Windows guarantees MapViewOfFile coherence.
///
///  All types are trivially copyable (no std::string, no pointers).
///  Struct fits in one 4KB page.
///
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>

namespace monitor {

constexpr uint32_t kMagic       = 0xFEEDBEEF;
constexpr uint32_t kMaxEvents   = 8;
constexpr uint32_t kEventSize   = 128;
constexpr uint32_t kMaxNameLen  = 64;
constexpr const wchar_t* kShmName = L"test_shell_metrics";

#pragma pack(push, 1)
struct MetricsData
{
    uint32_t          magic;     // must == kMagic
    volatile uint32_t seq;       // odd=writing, even=stable. volatile=no register caching. fence=ordering.

    uint64_t uptime_ms;
    uint64_t working_set_kb;
    uint32_t cpu_percent;
    uint32_t thread_count;
    uint32_t worker_threads;
    uint32_t stuck_threads;

    uint32_t tasks_active;
    uint32_t tasks_queued;
    uint32_t tasks_total;
    uint32_t tasks_completed;
    uint32_t modules_loaded;
    uint32_t signals_count;
    uint32_t dll_handles;
    uint32_t zombie_signals;

    char     last_error[kMaxNameLen];
    char     top_module[kMaxNameLen];
    char     events[kMaxEvents][kEventSize];
    uint32_t event_head;
    uint64_t checksum;
};
#pragma pack(pop)

static_assert(sizeof(MetricsData) <= 4096, "Must fit in one page");

// ================================================================
//  Write helpers (with fences for cross-thread visibility)
// ================================================================
inline void BeginWrite(MetricsData* m) {
    m->seq++;                                    // odd
    std::atomic_thread_fence(std::memory_order_release);
}
inline void EndWrite(MetricsData* m) {
    std::atomic_thread_fence(std::memory_order_release);
    m->seq++;                                    // even
}

// ================================================================
//  Read helper (with fences for cross-thread visibility)
// ================================================================
inline bool TryRead(const MetricsData* m, MetricsData* out) {
    uint32_t s1;
    // Wait for stable (non-writing) state, but don't spin forever
    for (int spin = 0; spin < 1000; ++spin) {
        s1 = m->seq;
        if (!(s1 & 1)) break;         // even → stable
        std::atomic_thread_fence(std::memory_order_acquire);
    }
    if (s1 & 1) return false;          // timeout: write still in progress

    std::atomic_thread_fence(std::memory_order_acquire);
    std::memcpy(out, m, sizeof(MetricsData));   // snapshot
    std::atomic_thread_fence(std::memory_order_acquire);
    uint32_t s2 = m->seq;

    if (s1 != s2) return false;        // seq changed → tearing, caller retries
    return out->magic == kMagic;
}

}  // namespace monitor
