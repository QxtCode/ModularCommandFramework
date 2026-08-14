#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include "ParmarPack.h"
#include "core/ITaskPersistence.h"  // TaskRecord（快照结构）

// =================================================================
//  Task — 分片任务（Shard-based Task）
// =================================================================
//
//  生活比喻: 快递分拣流水线
//    每个 Task = 一个包裹，每个 Shard = 一个分拣站。
//    Step() = 包裹经过一站（发射信号 → 模块处理 → 移到下一站）
//    包裹可以在站与站之间被暂停、恢复、取消。
//
//  【状态机】
//
//     IDLE ──Assign()──▶ IDLE ──Step()──▶ RUNNING
//                                            │
//                        ┌───────Resume()────┤
//                        │                   │
//                        ▼                   ▼
//                     PAUSED ◀──Pause()   COMPLETED / FAILED
//                        │
//                        └──Cancel()──▶ FAILED
//               Reset() 从任意状态回到 IDLE
//
//  【PAUSED 流程】v2.7
//   ① 模块回调调 task->Pause() → state_ = PAUSED（原子操作，瞬间完成）
//   ② Worker 在当前 shard 完成后检查 state_ → PAUSED → Step 返回 false
//   ③ Worker 退出 while 循环 → 可保存快照 → Release(task) 归还槽位
//   ④ 恢复时: 从快照 Restore → Resume → 重新 Enqueue → 从断点继续
//
//  【分片间暂停 — 关键设计】
//   暂停只在分片之间生效。当前分片不会被打断。
//   为什么？C++ 没法安全杀线程。暂停一个正在跑的 shard 是做不到的。
//   解决方案: 把长时间任务拆成小分片（<100ms），每个分片结束都是暂停点。
class EventBus;

class Task
{
public:
    enum class State { IDLE, RUNNING, PAUSED, COMPLETED, FAILED };
    using Callback = std::function<void(Task&)>;

    Task() = default;
    ~Task() = default;

    // ============================================================
    //  初始化：把参数包装载为第一个分片
    // ============================================================
    //  Assign 会清空之前的所有 shard，把新 pack 作为 shards_[0]。
    //  同时设置 pack->owner_task = this，让模块在执行时可以通过
    //  pack->owner_task->PushShard() 追加后续步骤。
    bool Assign(std::unique_ptr<ParmarPack> pack)
    {
        if (!pack) return false;
        pack->owner_task = this;
        std::lock_guard lock(shards_mutex_);
        shards_.clear();
        shards_.push_back(std::move(pack));
        current_ = 0;
        state_   = State::IDLE;
        declared_total_.store(0);  // v2.6: 新任务从零开始，防旧声明残留
        return true;
    }

    // ============================================================
    //  预声明总分片数（v2.6）
    // ============================================================
    /// 模块在执行第一个 shard 时调用，告诉 Task 一共会有多少个分片。
    /// 不调用则退化为动态模式（与 v2.5 行为完全一致）。
    void SetTotalShards(size_t total) { declared_total_.store(total); }

    // ============================================================
    //  动态追加分片（模块在 Slot 执行期间调用）
    // ============================================================
    void PushShard(std::unique_ptr<ParmarPack> pack)
    {
        if (!pack) return;
        pack->owner_task = this;
        std::lock_guard lock(shards_mutex_);
        shards_.push_back(std::move(pack));
    }

    // ============================================================
    //  核心：执行一个分片（单步推进）
    // ============================================================
    /// 取 shards_[current_] → 组装信号名 → 发射信号 → 模块 Slot 响应
    /// → 执行完毕 → current_++ → 检查暂停 → 检查是否全部完成
    /// @return true: 执行成功 / false: 暂停中、已完成、或信号未找到
    bool Step(EventBus& bus);

    // ============================================================
    //  控制接口
    // ============================================================
    void Pause()  { state_.store(State::PAUSED); }
    void Resume() { State s = state_.load(); if (s == State::PAUSED) state_.store(State::RUNNING); }
    void Cancel() { state_.store(State::FAILED); }

    /// Reset: 回到 IDLE，清空所有 shard。TasksPool::Release() 会调用。
    void Reset()
    {
        state_.store(State::IDLE);
        std::lock_guard lock(shards_mutex_);
        shards_.clear();
        current_ = 0;
        id_.store(0);
        declared_total_.store(0);
    }

    // ============================================================
    //  回调（生命周期钩子）
    // ============================================================
    void SetOnComplete(Callback cb) { on_complete_ = cb; }
    void SetOnError(Callback cb)    { on_error_    = cb; }

    // ============================================================
    //  查询接口
    // ============================================================
    State    GetState()         const { return state_.load(); }
    uint32_t GetID()            const { return id_.load(); }
    size_t   GetCurrentShard()  const { std::lock_guard lock(shards_mutex_); return current_; }
    size_t   GetTotalShards()   const {
        size_t dt = declared_total_.load();
        if (dt > 0) return dt;
        std::lock_guard lock(shards_mutex_);
        return shards_.size();
    }
    void     SetID(uint32_t id)       { id_.store(id); }

    /// 进度 = 已完成分片数 / max(声明总数, 实际分片数)，封顶 1.0
    float GetProgress() const
    {
        size_t total = declared_total_.load();
        std::lock_guard lock(shards_mutex_);
        if (total == 0) total = shards_.size();
        if (total == 0) return 0.0f;
        float p = static_cast<float>(current_) / total;
        return p > 1.0f ? 1.0f : p;
    }

    /// 获取当前正在执行的分片参数包（执行后读取结果用）
    ParmarPack* CurrentPack()
    {
        std::lock_guard lock(shards_mutex_);
        if (shards_.empty()) return nullptr;
        if (current_ >= shards_.size()) return shards_.back().get();
        return shards_[current_].get();
    }

    // ============================================================
    //  v2.7: 快照 / 恢复（ITaskPersistence 用）
    // ============================================================
    /// 导出当前状态为 TaskRecord。调用方负责序列化 shards_json。
    /// 线程安全: 内部加锁读取 shards_ + current_。
    std::string StateName() const {
        switch (state_.load()) {
            case State::IDLE:      return "IDLE";
            case State::RUNNING:   return "RUNNING";
            case State::PAUSED:    return "PAUSED";
            case State::COMPLETED: return "COMPLETED";
            case State::FAILED:    return "FAILED";
        }
        return "IDLE";
    }

    /// 从 TaskRecord 恢复状态。调用前 task 必须处于 IDLE 状态。
    /// shards 从 shards_json 反序列化填充。
    /// @return false 如果 task 不在 IDLE 状态或恢复失败。
    bool Restore(const TaskRecord& record);

    /// 导出当前状态为 TaskRecord 快照（含序列化的全部分片）。
    /// 线程安全：内部加锁读取 shards_ / current_ / declared_total。
    TaskRecord ExportRecord() const;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

private:
    friend class TasksPool;  // TasksPool 需要访问 pool_index_

    // ---- 分片存储（v2.4: shards_mutex_ 保护多线程访问）----
    mutable std::mutex shards_mutex_;
    std::vector<std::unique_ptr<ParmarPack>> shards_;
    size_t current_ = 0;   // 当前执行到第几个 shard（0-based）

    // ---- 状态（原子变量，线程安全）----
    std::atomic<State>    state_{ State::IDLE };
    std::atomic<uint32_t> id_   { 0 };

    // ---- 预声明总分片数（0 = 未声明，走动态模式）----
    std::atomic<size_t> declared_total_{0};

    // ---- 对象池索引（O(1) 归还）----
    // pool_index_ 在 TasksPool::CreateTasks() 时设置，
    // Release() 时通过这个索引直接放回 free_indices_。
    size_t pool_index_{ 0 };

    // ---- 生命周期回调 ----
    Callback on_complete_;
    Callback on_error_;
};
