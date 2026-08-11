/// =================================================================
///  MemTaskStore — 内存持久化 + 调度 (v2.6)
/// =================================================================
///
///  进程内有效，重启丢失。用于验证 ITaskStore 架构正确性，
///  后续可替换为 FileTaskStore / SqliteTaskStore。
///
///  线程安全: mutex 保护所有 map 操作。
/// =================================================================

#pragma once
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
#include "core/ITaskPersistence.h"
#include "core/ITaskScheduler.h"

class MemPersistence : public ITaskPersistence {
public:
    void Save(const TaskRecord& r) override {
        std::lock_guard<std::mutex> lk(mutex_);
        records_[r.task_id] = r;
    }
    std::optional<TaskRecord> Load(uint32_t id) const override {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = records_.find(id);
        return (it != records_.end())
            ? std::optional<TaskRecord>(it->second) : std::nullopt;
    }
    void Delete(uint32_t id) override {
        std::lock_guard<std::mutex> lk(mutex_);
        records_.erase(id);
    }
    std::vector<TaskRecord> LoadAll() const override {
        std::lock_guard<std::mutex> lk(mutex_);
        std::vector<TaskRecord> v;
        for (auto& kv : records_)
            if (!kv.second.IsTerminal()) v.push_back(kv.second);
        return v;
    }
    void GC() override {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = records_.begin();
        while (it != records_.end()) {
            if (it->second.IsTerminal()) it = records_.erase(it);
            else ++it;
        }
    }
    size_t Size() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return records_.size();
    }
private:
    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, TaskRecord> records_;
};

class MemScheduler : public ITaskScheduler {
public:
    void Schedule(uint32_t id, std::chrono::system_clock::time_point at) override {
        std::lock_guard<std::mutex> lk(mutex_);
        entries_[at].push_back(id);
    }
    void Cancel(uint32_t id) override {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = entries_.begin();
        while (it != entries_.end()) {
            auto& v = it->second;
            v.erase(std::remove(v.begin(), v.end(), id), v.end());
            if (v.empty()) it = entries_.erase(it); else ++it;
        }
    }
    std::vector<uint32_t> PollDue(std::chrono::system_clock::time_point now) override {
        std::lock_guard<std::mutex> lk(mutex_);
        std::vector<uint32_t> due;
        auto it = entries_.begin();
        while (it != entries_.end() && it->first <= now) {
            due.insert(due.end(), it->second.begin(), it->second.end());
            it = entries_.erase(it);
        }
        return due;
    }
    std::optional<std::chrono::milliseconds> NextWakeup(
        std::chrono::system_clock::time_point now) const override {
        std::lock_guard<std::mutex> lk(mutex_);
        if (entries_.empty()) return std::nullopt;
        auto d = std::chrono::duration_cast<std::chrono::milliseconds>(
            entries_.begin()->first - now);
        return (d.count() > 0) ? d : std::chrono::milliseconds{0};
    }
    size_t PendingCount() const override {
        std::lock_guard<std::mutex> lk(mutex_);
        size_t n = 0;
        for (auto& kv : entries_) n += kv.second.size();
        return n;
    }
private:
    mutable std::mutex mutex_;
    std::map<std::chrono::system_clock::time_point, std::vector<uint32_t>> entries_;
};
