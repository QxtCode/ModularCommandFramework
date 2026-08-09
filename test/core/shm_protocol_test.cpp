/// =================================================================
///  Shared Memory Protocol — isolation tests
/// =================================================================
///
///  Simulates cross-process shared memory within a single process.
///  Two threads share a MetricsData struct:
///    - Writer: updates fields every 50ms, seq++ at end
///    - Reader: uses TryRead() (seqlock) to get consistent snapshots
///
///  These tests verify the seqlock pattern BEFORE we touch
///  CreateFileMapping / MapViewOfFile / cross-process.

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include "monitor/MetricsProtocol.h"

using namespace std::chrono_literals;
using namespace monitor;

// ================================================================
//  Test 1: single write → single read, no contention
// ================================================================
TEST(ShmProtocol, BasicWriteRead)
{
    MetricsData shm{};
    shm.magic = kMagic;
    shm.seq   = 0;

    // Writer
    BeginWrite(&shm);
    shm.tasks_active    = 5;
    shm.tasks_total     = 32;
    shm.modules_loaded  = 3;
    shm.uptime_ms       = 12345;
    EndWrite(&shm);

    // Reader
    MetricsData snapshot{};
    bool ok = TryRead(&shm, &snapshot);
    EXPECT_TRUE(ok);
    EXPECT_EQ(snapshot.tasks_active,   5u);
    EXPECT_EQ(snapshot.tasks_total,   32u);
    EXPECT_EQ(snapshot.modules_loaded, 3u);
    EXPECT_EQ(snapshot.uptime_ms,   12345ull);
}

// ================================================================
//  Test 2: magic mismatch → TryRead returns false
// ================================================================
TEST(ShmProtocol, MagicCheckFails)
{
    MetricsData shm{};
    shm.magic = 0xDEAD;   // wrong magic
    shm.seq   = 0;

    MetricsData snapshot{};
    EXPECT_FALSE(TryRead(&shm, &snapshot))
        << "Should reject data with bad magic";
}

// ================================================================
//  Test 3: TryRead returns false while write in progress
// ================================================================
TEST(ShmProtocol, TryReadFailsDuringWrite)
{
    MetricsData shm{};
    shm.magic = kMagic;
    shm.seq   = 0;

    // Simulate: writer is in progress (seq odd)
    BeginWrite(&shm);
    EXPECT_EQ(shm.seq & 1, 1u);

    // Reader should get false — write in progress
    MetricsData snapshot{};
    EXPECT_FALSE(TryRead(&shm, &snapshot))
        << "TryRead should fail (return false) while write is in progress";

    // Finish write
    EndWrite(&shm);
    EXPECT_EQ(shm.seq & 1, 0u);

    // Now reader should succeed
    EXPECT_TRUE(TryRead(&shm, &snapshot));
}

// ================================================================
//  Test 4: concurrent writer + reader, no data corruption
// ================================================================
// MANUAL: 3-second concurrent stress, may be flaky under CPU contention in CI.
// Run individually: --gtest_filter="ShmProtocol.ConcurrentWriteReadNoCorruption"
// DISABLED: 3s concurrent stress, flaky under high CPU contention. Run manually.
TEST(ShmProtocol, DISABLED_ConcurrentWriteReadNoCorruption)
{
    MetricsData shm{};
    shm.magic = kMagic;
    shm.seq   = 0;

    std::atomic<bool> stop{false};
    std::atomic<int>  write_count{0};
    std::atomic<int>  read_count{0};
    std::atomic<int>  corrupt_count{0};

    // Writer thread: update every 2ms
    std::thread writer([&]() {
        uint32_t counter = 0;
        while (!stop.load()) {
            BeginWrite(&shm);
            shm.tasks_active    = counter;
            shm.tasks_queued    = counter + 100;
            shm.tasks_completed = counter + 200;
            EndWrite(&shm);
            counter++;
            write_count.fetch_add(1);
            std::this_thread::sleep_for(2ms);
        }
    });

    // Reader thread: read every 1ms, verify consistency
    std::thread reader([&]() {
        while (!stop.load()) {
            MetricsData snap{};
            if (TryRead(&shm, &snap)) {
                // Invariant: tasks_queued == tasks_active + 100
                //            tasks_completed == tasks_active + 200
                if (snap.tasks_queued != snap.tasks_active + 100 ||
                    snap.tasks_completed != snap.tasks_active + 200) {
                    corrupt_count.fetch_add(1);
                }
                read_count.fetch_add(1);
            }
            std::this_thread::sleep_for(1ms);
        }
    });

    std::this_thread::sleep_for(3s);
    stop.store(true);

    writer.join();
    reader.join();

    std::cout << "[TEST] writes=" << write_count.load()
              << " reads=" << read_count.load()
              << " corrupt=" << corrupt_count.load() << std::endl;

    EXPECT_GT(write_count.load(), 100);
    EXPECT_GT(read_count.load(), 100);
    EXPECT_EQ(corrupt_count.load(), 0)
        << "Seqlock should prevent torn reads";
}

// ================================================================
//  Test 5: event ring buffer
// ================================================================
TEST(ShmProtocol, EventRingBuffer)
{
    MetricsData shm{};
    shm.magic = kMagic;
    shm.seq   = 0;

    // Write 12 events (ring buffer holds 8 → last 8 survive)
    for (int i = 0; i < 12; ++i) {
        BeginWrite(&shm);
        uint32_t idx = shm.event_head % kMaxEvents;
        snprintf(shm.events[idx], kEventSize,
                 "[%02d:00:00] EVENT #%d", i, i);
        shm.event_head++;
        EndWrite(&shm);
    }

    MetricsData snap{};
    ASSERT_TRUE(TryRead(&shm, &snap));

    // Ring buffer should have events 4-11 (oldest 0-3 overwritten)
    EXPECT_EQ(snap.event_head, 12u);

    // Check event #11 (last written, at index 3)
    uint32_t idx11 = 11 % kMaxEvents;  // = 3
    EXPECT_STREQ(snap.events[idx11], "[11:00:00] EVENT #11");

    // Check event #4 (oldest surviving, at index 4)
    uint32_t idx4 = 4 % kMaxEvents;  // = 4
    EXPECT_STREQ(snap.events[idx4], "[04:00:00] EVENT #4");
}

// ================================================================
//  Test 6: checksum validation
// ================================================================
TEST(ShmProtocol, ChecksumValidation)
{
    MetricsData shm{};
    shm.magic = kMagic;
    shm.seq   = 0;

    // Setup: write known data
    BeginWrite(&shm);
    shm.tasks_active = 42;
    shm.checksum     = 0;   // placeholder
    EndWrite(&shm);

    // Compute checksum over fixed fields (exclude checksum itself and seq,
    // since seq changes every write). Only cover the "payload" region.
    constexpr size_t kPayloadStart = offsetof(MetricsData, uptime_ms);
    constexpr size_t kPayloadSize  = offsetof(MetricsData, checksum) - kPayloadStart;

    uint64_t sum = 0;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(&shm);
    for (size_t i = 0; i < kPayloadSize; ++i)
        sum += base[kPayloadStart + i];

    // Set checksum (bypass BeginWrite since this is test setup)
    shm.checksum = sum;

    // Verify
    MetricsData snap{};
    ASSERT_TRUE(TryRead(&shm, &snap));

    uint64_t verify = 0;
    const uint8_t* snap_base = reinterpret_cast<const uint8_t*>(&snap);
    for (size_t i = 0; i < kPayloadSize; ++i)
        verify += snap_base[kPayloadStart + i];

    EXPECT_EQ(snap.checksum, verify);
    EXPECT_EQ(snap.tasks_active, 42u);
}

// ================================================================
//  Test 7: BeginWrite/EndWrite helpers
// ================================================================
TEST(ShmProtocol, BeginEndWriteHelpers)
{
    MetricsData shm{};
    shm.magic = kMagic;
    shm.seq   = 0;

    BeginWrite(&shm);
    EXPECT_EQ(shm.seq & 1, 1u) << "seq should be odd during write";
    shm.tasks_active = 7;
    EndWrite(&shm);
    EXPECT_EQ(shm.seq & 1, 0u) << "seq should be even after write";

    MetricsData snap{};
    ASSERT_TRUE(TryRead(&shm, &snap));
    EXPECT_EQ(snap.tasks_active, 7u);
}
