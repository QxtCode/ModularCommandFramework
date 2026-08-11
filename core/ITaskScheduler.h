/// =================================================================
///  ITaskScheduler — 延迟任务调度接口 (v2.6)
/// =================================================================
///
///  与 ITaskPersistence 独立组合: 内存时间轮（轻量, 重启丢失延迟任务）
///  或持久化后端（重启恢复延迟任务）。主循环 WaitForWork 通过
///  NextWakeup() 动态计算 cv 超时替代固定 100ms。
///
///  线程安全: 所有方法必须线程安全。
/// =================================================================

#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

class ITaskScheduler {
public:
    virtual ~ITaskScheduler() = default;

    virtual void Schedule(uint32_t task_id,
                          std::chrono::system_clock::time_point at_time) = 0;
    virtual void Cancel(uint32_t task_id) = 0;
    virtual std::vector<uint32_t> PollDue(
        std::chrono::system_clock::time_point now) = 0;
    virtual std::optional<std::chrono::milliseconds> NextWakeup(
        std::chrono::system_clock::time_point now) const = 0;
    virtual size_t PendingCount() const = 0;
};
