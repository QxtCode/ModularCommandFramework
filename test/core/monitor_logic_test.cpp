/// =================================================================
///  Monitor Logic — isolation tests for the render loop logic
/// =================================================================
///
///  Tests the monitor's decision logic WITHOUT FTXUI or shared memory:
///   1. Raw magic detection (not cached snap.magic)
///   2. snap_valid → false clears stale snap data
///   3. Temporary TryRead failures don't clear snap (avoid flicker)
///   4. no_sig_cnt threshold triggers "NO SIGNAL"
///   5. stale_cnt threshold triggers "STALE" but keeps old data

#include <gtest/gtest.h>
#include <cstring>
#include "monitor/MetricsProtocol.h"

using namespace monitor;

// ================================================================
//  Simulates the monitor's decision logic (pure functions, no IO)
// ================================================================
struct MonitorState {
    MetricsData snap{};
    bool        snap_valid = false;
    uint32_t    last_seq   = 0;
    int         no_sig_cnt = 0;
    int         stale_cnt  = 0;

    static constexpr int kNoSigThreshold = 4;   // 4 frames → NO SIGNAL
    static constexpr int kStaleThreshold = 8;   // 8 frames → clear stale

    /// Called each frame. Returns display state string.
    const char* Update(const MetricsData* raw_shm, bool try_read_ok, const MetricsData* fresh) {
        uint32_t raw_magic = raw_shm->magic;  // ★ direct read, NOT from snap

        if (raw_magic != kMagic) {
            // ---- Framework is gone ----
            no_sig_cnt++;
            if (no_sig_cnt >= kNoSigThreshold) {
                snap_valid = false;
                std::memset(&snap, 0, sizeof(snap));
                return "NO_SIGNAL_CLEARED";
            }
            // keep old snap for a few frames, show STALE first
            return "NO_SIGNAL_WAITING";
        }

        // ---- Framework is alive ----
        no_sig_cnt = 0;

        if (try_read_ok && fresh->seq != last_seq) {
            // Fresh data
            snap = *fresh;
            snap_valid = true;
            last_seq = fresh->seq;
            stale_cnt = 0;
            return "LIVE";
        }

        // TryRead failed or seq unchanged
        stale_cnt++;
        if (stale_cnt >= kStaleThreshold) {
            // Too many failures → clear old data
            snap_valid = false;
            std::memset(&snap, 0, sizeof(snap));
            return "STALE_CLEARED";
        }
        // Keep old snap, just flag as stale
        return "STALE";
    }
};

// ================================================================
//  Test 1: raw magic=0 → NO SIGNAL eventually triggers
// ================================================================
TEST(MonitorLogic, RawMagicZeroTriggersNoSignal)
{
    MonitorState s;
    MetricsData raw{};
    raw.magic = 0;  // framework gone

    // Frame 1-3: waiting
    EXPECT_STREQ("NO_SIGNAL_WAITING", s.Update(&raw, false, nullptr));
    EXPECT_STREQ("NO_SIGNAL_WAITING", s.Update(&raw, false, nullptr));
    EXPECT_STREQ("NO_SIGNAL_WAITING", s.Update(&raw, false, nullptr));

    // Frame 4: threshold reached → cleared
    EXPECT_STREQ("NO_SIGNAL_CLEARED", s.Update(&raw, false, nullptr));
    EXPECT_FALSE(s.snap_valid);
    EXPECT_EQ(s.snap.tasks_active, 0u) << "Stale snap cleared";
}

// ================================================================
//  Test 2: magic restored → immediately recovers
// ================================================================
TEST(MonitorLogic, MagicRestoredRecoversInstantly)
{
    MonitorState s;
    MetricsData raw{};
    raw.magic = 0;

    // Trigger NO SIGNAL
    for (int i = 0; i < 4; ++i) s.Update(&raw, false, nullptr);
    EXPECT_FALSE(s.snap_valid);

    // Framework restarts
    raw.magic = kMagic;
    raw.seq = 10;
    raw.tasks_active = 3;

    EXPECT_STREQ("LIVE", s.Update(&raw, true, &raw));
    EXPECT_TRUE(s.snap_valid);
    EXPECT_EQ(s.snap.tasks_active, 3u);
    EXPECT_EQ(s.no_sig_cnt, 0);
}

// ================================================================
//  Test 3: TryRead fails temporarily → keeps old snap (no flicker)
// ================================================================
TEST(MonitorLogic, TryReadFailureKeepsOldSnap)
{
    MonitorState s;
    MetricsData raw{};
    raw.magic = kMagic;
    raw.seq = 2;
    raw.tasks_active = 5;
    s.Update(&raw, true, &raw);  // seed valid data
    EXPECT_TRUE(s.snap_valid);
    EXPECT_EQ(s.snap.tasks_active, 5u);

    // TryRead fails (write in progress) — keep old data
    for (int i = 0; i < 5; ++i) {
        EXPECT_STREQ("STALE", s.Update(&raw, false, nullptr));
        EXPECT_TRUE(s.snap_valid) << "Should keep old snap (no flicker)";
        EXPECT_EQ(s.snap.tasks_active, 5u) << "Data preserved";
    }

    // After 8 failures → clear
    for (int i = 5; i < 8; ++i) s.Update(&raw, false, nullptr);
    EXPECT_FALSE(s.snap_valid) << "Should clear after 8 failures";
    EXPECT_EQ(s.snap.tasks_active, 0u);
}

// ================================================================
//  Test 4: stale old snap is NOT used for magic check
// ================================================================
TEST(MonitorLogic, StaleSnapMagicNotUsed)
{
    MonitorState s;
    MetricsData raw{};
    raw.magic = kMagic;
    raw.seq = 2;
    s.Update(&raw, true, &raw);

    // Set snap.magic to garbage via direct write (simulating stale data)
    s.snap.magic = 0xDEAD;

    // TryRead fails, raw magic is kMagic → should NOT trigger NO SIGNAL
    // even though snap.magic is wrong
    raw.magic = kMagic;
    EXPECT_STREQ("STALE", s.Update(&raw, false, nullptr));
    EXPECT_EQ(s.no_sig_cnt, 0) << "no_sig_cnt should be 0: raw magic is valid";
    EXPECT_NE(s.snap.magic, kMagic) << "snap.magic is stale, but NOT used for detection";
}

// ================================================================
//  Test 5: full lifecycle simulation
// ================================================================
TEST(MonitorLogic, FullLifecycle)
{
    MonitorState s;
    MetricsData raw{};
    raw.magic = kMagic;
    raw.seq = 0;
    raw.tasks_active = 0;

    // ---- Phase 1: LIVE with data ----
    for (int i = 0; i < 5; ++i) {
        raw.seq += 2;
        raw.tasks_active = i;
        EXPECT_STREQ("LIVE", s.Update(&raw, true, &raw));
        EXPECT_TRUE(s.snap_valid);
        EXPECT_EQ(s.snap.tasks_active, (uint32_t)i);
    }

    // ---- Phase 2: Framework crashes (magic=0) ----
    raw.magic = 0;
    for (int i = 0; i < 3; ++i) {
        EXPECT_STREQ("NO_SIGNAL_WAITING", s.Update(&raw, false, nullptr));
        EXPECT_TRUE(s.snap_valid) << "Still showing old data";
    }
    EXPECT_STREQ("NO_SIGNAL_CLEARED", s.Update(&raw, false, nullptr));
    EXPECT_FALSE(s.snap_valid);

    // ---- Phase 3: Framework restarts ----
    raw.magic = kMagic;
    raw.seq = 100;
    raw.tasks_active = 7;
    EXPECT_STREQ("LIVE", s.Update(&raw, true, &raw));
    EXPECT_TRUE(s.snap_valid);
    EXPECT_EQ(s.snap.tasks_active, 7u);
    EXPECT_EQ(s.no_sig_cnt, 0);
}

// ================================================================
//  Test 6: Event ring buffer survives stale frames
// ================================================================
TEST(MonitorLogic, EventBufferPreservedDuringStale)
{
    MonitorState s;
    MetricsData raw{};
    raw.magic = kMagic;
    raw.seq = 2;
    raw.event_head = 1;
    snprintf(raw.events[0], kEventSize, "test event");

    s.Update(&raw, true, &raw);
    EXPECT_EQ(std::string(s.snap.events[0]), "test event");

    // TryRead fails → events preserved from last good snap
    raw.magic = kMagic;
    for (int i = 0; i < 3; ++i) {
        s.Update(&raw, false, nullptr);
        EXPECT_EQ(std::string(s.snap.events[0]), "test event")
            << "Events preserved during short stale period";
    }
}
