/// =================================================================
///  TaskStore 隔离测试 (v2.6)
/// =================================================================
///
///  纯接口 + Mock 验证，不依赖 ShellEngine / EventBus / ThreadPool。
/// =================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/ITaskPersistence.h"
#include "core/ITaskScheduler.h"
#include "core/NullTaskStore.h"

using Clock = std::chrono::system_clock;
using namespace std::chrono_literals;

// ================================================================
//  TaskRecord
// ================================================================
TEST(TaskRecordTest, IsTerminal) {
    TaskRecord r;
    r.state = "IDLE";      EXPECT_FALSE(r.IsTerminal());
    r.state = "RUNNING";   EXPECT_FALSE(r.IsTerminal());
    r.state = "PAUSED";    EXPECT_FALSE(r.IsTerminal());
    r.state = "COMPLETED"; EXPECT_TRUE(r.IsTerminal());
    r.state = "FAILED";    EXPECT_TRUE(r.IsTerminal());
}

TEST(TaskRecordTest, IsScheduled) {
    TaskRecord r;
    EXPECT_FALSE(r.IsScheduled());
    r.scheduled_at_ms = 1;
    EXPECT_TRUE(r.IsScheduled());
}

TEST(TaskRecordTest, DefaultValues) {
    TaskRecord r;
    EXPECT_EQ(r.task_id, 0u);
    EXPECT_EQ(r.state, "");
    EXPECT_FALSE(r.IsScheduled());
    EXPECT_FALSE(r.IsTerminal());
}

// ================================================================
//  NullPersistence
// ================================================================
TEST(NullPersistenceTest, AllNoOps) {
    NullPersistence store;
    TaskRecord r; r.task_id = 1;
    EXPECT_NO_THROW(store.Save(r));
    EXPECT_FALSE(store.Load(1).has_value());
    EXPECT_NO_THROW(store.Delete(1));
    EXPECT_TRUE(store.LoadAll().empty());
    EXPECT_NO_THROW(store.GC());
    EXPECT_TRUE(store.IsAvailable());
}

// ================================================================
//  NullScheduler
// ================================================================
TEST(NullSchedulerTest, AllNoOps) {
    NullScheduler s;
    Clock::time_point now = Clock::now();
    EXPECT_NO_THROW(s.Schedule(1, now + std::chrono::hours(1)));
    EXPECT_NO_THROW(s.Cancel(1));
    EXPECT_TRUE(s.PollDue(now).empty());
    EXPECT_FALSE(s.NextWakeup(now).has_value());
    EXPECT_EQ(s.PendingCount(), 0u);
}

// ================================================================
//  接口多态
// ================================================================
TEST(TaskStorePolymorphism, ViaBasePtr) {
    std::unique_ptr<ITaskPersistence> p(new NullPersistence());
    std::unique_ptr<ITaskScheduler>   s(new NullScheduler());
    TaskRecord r; r.task_id = 1;
    p->Save(r);
    EXPECT_FALSE(p->Load(1).has_value());
    Clock::time_point now = Clock::now();
    s->Schedule(1, now);
    EXPECT_TRUE(s->PollDue(now).empty());
}

// ================================================================
//  MockPersistence — 内存后端
// ================================================================
namespace {
class MockPersistence : public ITaskPersistence {
public:
    void Save(const TaskRecord& r) override {
        std::lock_guard<std::mutex> lk(mutex_);
        map_[r.task_id] = r;
    }
    std::optional<TaskRecord> Load(uint32_t id) const override {
        std::lock_guard<std::mutex> lk(mutex_);
        std::unordered_map<uint32_t, TaskRecord>::const_iterator it = map_.find(id);
        if (it != map_.end()) return std::optional<TaskRecord>(it->second);
        return std::nullopt;
    }
    void Delete(uint32_t id) override {
        std::lock_guard<std::mutex> lk(mutex_);
        map_.erase(id);
    }
    std::vector<TaskRecord> LoadAll() const override {
        std::lock_guard<std::mutex> lk(mutex_);
        std::vector<TaskRecord> v;
        for (std::unordered_map<uint32_t, TaskRecord>::const_iterator it = map_.begin();
             it != map_.end(); ++it) {
            if (!it->second.IsTerminal())
                v.push_back(it->second);
        }
        return v;
    }
    void GC() override {
        std::lock_guard<std::mutex> lk(mutex_);
        std::unordered_map<uint32_t, TaskRecord>::iterator it = map_.begin();
        while (it != map_.end()) {
            if (it->second.IsTerminal())
                it = map_.erase(it);
            else
                ++it;
        }
    }
    size_t Size() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return map_.size();
    }
private:
    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, TaskRecord> map_;
};
} // namespace

TEST(MockPersistenceTest, SaveLoadRoundTrip) {
    MockPersistence s;
    TaskRecord r; r.task_id = 42; r.state = "PAUSED"; r.current_shard = 3;
    s.Save(r);
    std::optional<TaskRecord> v = s.Load(42);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->task_id, 42u);
    EXPECT_EQ(v->state, "PAUSED");
}

TEST(MockPersistenceTest, DeleteAndLoadAll) {
    MockPersistence s;
    TaskRecord r1; r1.task_id = 1; r1.state = "PAUSED";
    TaskRecord r2; r2.task_id = 2; r2.state = "COMPLETED";
    s.Save(r1); s.Save(r2);
    EXPECT_EQ(s.LoadAll().size(), 1u);
    s.Delete(1);
    EXPECT_FALSE(s.Load(1).has_value());
}

TEST(MockPersistenceTest, GC) {
    MockPersistence s;
    for (uint32_t i = 0; i < 5; ++i) {
        TaskRecord r; r.task_id = i;
        r.state = (i < 2) ? "PAUSED" : "COMPLETED";
        s.Save(r);
    }
    EXPECT_EQ(s.Size(), 5u);
    s.GC();
    EXPECT_EQ(s.Size(), 2u);
}

// ================================================================
//  MockScheduler — 内存时间轮
// ================================================================
namespace {
class MockScheduler : public ITaskScheduler {
public:
    void Schedule(uint32_t id, Clock::time_point at) override {
        std::lock_guard<std::mutex> lk(mutex_);
        entries_[at].push_back(id);
    }
    void Cancel(uint32_t id) override {
        std::lock_guard<std::mutex> lk(mutex_);
        std::map<Clock::time_point, std::vector<uint32_t>>::iterator it = entries_.begin();
        while (it != entries_.end()) {
            std::vector<uint32_t>& vec = it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
            if (vec.empty())
                it = entries_.erase(it);
            else
                ++it;
        }
    }
    std::vector<uint32_t> PollDue(Clock::time_point now) override {
        std::lock_guard<std::mutex> lk(mutex_);
        std::vector<uint32_t> due;
        std::map<Clock::time_point, std::vector<uint32_t>>::iterator it = entries_.begin();
        while (it != entries_.end() && it->first <= now) {
            due.insert(due.end(), it->second.begin(), it->second.end());
            it = entries_.erase(it);
        }
        return due;
    }
    std::optional<std::chrono::milliseconds> NextWakeup(Clock::time_point now) const override {
        std::lock_guard<std::mutex> lk(mutex_);
        if (entries_.empty()) return std::nullopt;
        std::chrono::milliseconds d =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                entries_.begin()->first - now);
        if (d.count() > 0) return d;
        return std::chrono::milliseconds{0};
    }
    size_t PendingCount() const override {
        std::lock_guard<std::mutex> lk(mutex_);
        size_t n = 0;
        for (std::map<Clock::time_point, std::vector<uint32_t>>::const_iterator
                 it = entries_.begin(); it != entries_.end(); ++it)
            n += it->second.size();
        return n;
    }
private:
    mutable std::mutex mutex_;
    std::map<Clock::time_point, std::vector<uint32_t>> entries_;
};
} // namespace

TEST(MockSchedulerTest, SchedulePollCancel) {
    MockScheduler s;
    Clock::time_point now = Clock::now();
    s.Schedule(1, now - std::chrono::seconds(1));
    s.Schedule(2, now + std::chrono::hours(1));
    EXPECT_EQ(s.PendingCount(), 2u);
    std::vector<uint32_t> due = s.PollDue(now);
    ASSERT_EQ(due.size(), 1u);
    EXPECT_EQ(due[0], 1u);
    EXPECT_EQ(s.PendingCount(), 1u);
    s.Cancel(2);
    EXPECT_EQ(s.PendingCount(), 0u);
}

TEST(MockSchedulerTest, NextWakeup) {
    MockScheduler s;
    Clock::time_point now = Clock::now();
    EXPECT_FALSE(s.NextWakeup(now).has_value());
    s.Schedule(1, now + std::chrono::milliseconds(500));
    std::optional<std::chrono::milliseconds> w = s.NextWakeup(now);
    ASSERT_TRUE(w.has_value());
    EXPECT_GT(w->count(), 0);
}

// ================================================================
//  并发安全
// ================================================================
TEST(TaskStoreConcurrency, ConcurrentSave) {
    MockPersistence s;
    std::vector<std::thread> th;
    for (int t = 0; t < 4; ++t)
        th.emplace_back([&s, t]() {
            for (int i = 0; i < 250; ++i) {
                TaskRecord r; r.task_id = static_cast<uint32_t>(t * 1000 + i);
                r.state = "PAUSED";
                s.Save(r);
            }
        });
    for (size_t i = 0; i < th.size(); ++i) th[i].join();
    EXPECT_EQ(s.Size(), 1000u);
}

TEST(TaskStoreConcurrency, SaveLoadMixed) {
    MockPersistence s;
    for (uint32_t i = 0; i < 100; ++i) {
        TaskRecord r; r.task_id = i; r.state = "PAUSED"; s.Save(r);
    }
    std::atomic<bool> run{true};
    std::thread writer([&]() {
        for (uint32_t i = 100; run.load(); ++i) {
            TaskRecord r; r.task_id = i; r.state = "PAUSED"; s.Save(r);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::thread reader([&]() {
        while (run.load()) { s.Load(static_cast<uint32_t>(rand() % 500)); }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    run.store(false);
    writer.join(); reader.join();
    SUCCEED();
}

// ================================================================
//  边界
// ================================================================
TEST(TaskStoreEdgeCases, All) {
    MockPersistence p;
    TaskRecord r; r.task_id = 1; r.state = "PAUSED";
    p.Save(r);
    EXPECT_TRUE(p.Load(1).has_value());
    EXPECT_NO_THROW(p.Delete(99999));
    EXPECT_NO_THROW(p.Delete(1));
    EXPECT_FALSE(p.Load(1).has_value());
    EXPECT_TRUE(p.LoadAll().empty());
    EXPECT_NO_THROW(p.GC());

    MockScheduler s;
    Clock::time_point now = Clock::now();
    s.Schedule(1, now - std::chrono::hours(1));
    EXPECT_EQ(s.PollDue(now).size(), 1u);
}
