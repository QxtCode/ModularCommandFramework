/// =================================================================
///  ITaskPersistence — Task 状态持久化接口 (v2.7)
/// =================================================================
///
///  解决 PAUSED 任务和未执行命令在进程退出时丢失的问题。
///  可插拔后端: NullPersistence (默认, 零开销) / FilePersistence / SqlitePersistence
///
///  调用方不持有 Task 对象的所有权 — ITaskPersistence 保存的是
///  TaskRecord 快照，与 TasksPool 中的实际 Task 生命周期无关。
///
///  线程安全: 所有实现必须线程安全（Worker 和主线程可并发调用）。
///  异常安全: Save/Load/Delete 失败应静默处理，不抛异常。
///  Checkpoint 策略由 ShellEngine 控制（不在接口层强制）。
/// =================================================================

#pragma once
#include <cstdint>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/// Task 快照 — 与 TasksPool 中的 Task 对象生命周期解耦。
/// 仅在关键状态迁移点（PAUSE / FAIL / Shutdown）创建快照，
/// 不参与热路径。
struct TaskRecord {
    uint32_t    task_id      = 0;
    std::string state;             // "IDLE" | "RUNNING" | "PAUSED" | "FAILED" | "COMPLETED"
    size_t      current_shard = 0;
    size_t      declared_total = 0;
    std::string shards_json;       // 分片序列化数据
    int64_t     created_at_ms = 0; // system_clock 毫秒时间戳
    int64_t     scheduled_at_ms = 0; // 0 = 非延迟任务
    int         retry_count  = 0;
    int         max_retries  = 0;

    bool IsScheduled() const { return scheduled_at_ms > 0; }
    bool IsTerminal() const {
        return state == "COMPLETED" || state == "FAILED";
    }
};

class ITaskPersistence {
public:
    virtual ~ITaskPersistence() = default;

    /// 保存/更新 Task 快照。幂等，可重复调用覆盖同一 task_id。
    virtual void Save(const TaskRecord& record) = 0;

    /// 读取 Task 快照。不存在返回 nullopt。
    virtual std::optional<TaskRecord> Load(uint32_t task_id) const = 0;

    /// 删除 Task 快照（COMPLETED 后调用）。
    virtual void Delete(uint32_t task_id) = 0;

    /// 加载所有非终态记录（启动恢复用）。
    virtual std::vector<TaskRecord> LoadAll() const = 0;

    /// 清理终态记录（定期调用或 Shutdown 时）。
    virtual void GC() = 0;

    /// 后端是否可用。不可用时框架退化为纯内存模式，不丢任务。
    virtual bool IsAvailable() const { return true; }
};
