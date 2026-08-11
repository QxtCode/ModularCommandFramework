/// =================================================================
///  ITaskPersistence — Task 状态持久化接口 (v2.6)
/// =================================================================
///
///  可插拔后端: NullPersistence (默认) / FilePersistence / SqlitePersistence
///
///  Checkpoint 时机:
///    IDLE → RUNNING    ✗ 不写（内存热路径）
///    RUNNING → PAUSED  ✔ 必写（用户期望恢复）
///    RUNNING → FAILED  ✔ 写（可能重试）
///    COMPLETED         ✔ DeleteTask（清理）
///    优雅关闭           ✔ SaveAll（全量落盘）
///
///  线程安全: 所有方法必须线程安全。
/// =================================================================

#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct TaskRecord {
    uint32_t    task_id      = 0;
    std::string state;             // "IDLE" | "RUNNING" | "PAUSED" | "FAILED"
    size_t      current_shard = 0;
    size_t      declared_total = 0;
    std::string shards_json;
    int64_t     created_at_ms = 0;
    int64_t     scheduled_at_ms = 0;
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

    virtual void Save(const TaskRecord& record) = 0;
    virtual std::optional<TaskRecord> Load(uint32_t task_id) const = 0;
    virtual void Delete(uint32_t task_id) = 0;
    virtual std::vector<TaskRecord> LoadAll() const = 0;
    virtual void GC() = 0;
    virtual bool IsAvailable() const { return true; }
};
