/// =================================================================
///  MetricsProtocol.h — 共享内存协议（跨平台）
/// =================================================================
///
///  test_shell 和 shell_monitor 共同引用，定义跨进程指标共享内存布局。
///  跨平台：Windows File Mapping 和 POSIX shm_open+mmap 均支持。
///
///  SeqLock 快照模式（无锁）:
///    Writer: seq++ (奇数) → 写数据 → seq++ (偶数)
///    Reader: 等 seq 偶数 → s1=seq → memcpy 快照 → s2=seq
///            if s1 != s2 → 重试
///
///  跨线程: atomic_thread_fence 保证可见性。
///  跨进程: MapViewOfFile (Windows) / mmap MAP_SHARED (POSIX) 保证一致性。
///
///  所有类型可平凡复制（无 std::string，无指针），结构体 ≤ 4KB。
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
constexpr const char* kShmName = "test_shell_metrics";

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
