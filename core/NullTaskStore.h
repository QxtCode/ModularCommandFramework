/// =================================================================
///  NullTaskStore — 默认空后端 (v2.6)
/// =================================================================
///
///  不持久化、不调度。所有方法空操作，编译器可完全内联消除。
///  行为与未引入持久化前完全一致，零运行时开销。
///  用户通过 ShellConfig 注入自定义实现后才有实际效果。
/// =================================================================

#pragma once
#include "core/ITaskPersistence.h"
#include "core/ITaskScheduler.h"

class NullPersistence : public ITaskPersistence {
public:
    void Save(const TaskRecord&) override {}
    std::optional<TaskRecord> Load(uint32_t) const override { return std::nullopt; }
    void Delete(uint32_t) override {}
    std::vector<TaskRecord> LoadAll() const override { return {}; }
    void GC() override {}
};

class NullScheduler : public ITaskScheduler {
public:
    void Schedule(uint32_t, std::chrono::system_clock::time_point) override {}
    void Cancel(uint32_t) override {}
    std::vector<uint32_t> PollDue(std::chrono::system_clock::time_point) override { return {}; }
    std::optional<std::chrono::milliseconds> NextWakeup(
        std::chrono::system_clock::time_point) const override { return std::nullopt; }
    size_t PendingCount() const override { return 0; }
};
