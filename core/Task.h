#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include "ParmarPack.h"

// =================================================================
//  Task — EventBus 驱动的"分片任务"（Shard-based Task）
// =================================================================
//
//  【设计思想：为什么要有"分片"？】
//
//  传统的 Task 执行一个函数就结束了，无法暂停、无法追加后续步骤。
//  分片模型把一个大任务拆成多个小步骤（shard），每个 shard 就是
//  一次 EventBus 信号发射。每执行完一个 shard，Task 会检查是否
//  被暂停，然后再执行下一个。
//
//  【工作流程】
//
//  初始化:
//    Task* t = pool.Acquire(pack);   // 从对象池拿 Task，传入参数包
//    // Assign() 把 pack 存为 shards_[0]
//
//  单步执行:
//    t->Step(bus);
//    // ① 取 shards_[current_] 作为当前参数包
//    // ② 组装信号名 = pack->mod_id + "." + pack->func_id
//    //    例如 "Calculator.add"
//    // ③ bus.Emit("Calculator.add", pack)
//    //    → EventBus 找到信号 → 遍历 Slot → 模块的 Execute() 被调用
//    // ④ 模块执行完 → current_++ → 移到下一个 shard
//    // ⑤ 检查是否全部完成 → COMPLETED
//
//  分片间暂停:
//    模块在执行期间可以调用 task->Pause()
//    → 当前 shard 执行完后，Step() 检测到 PAUSED → 返回 false
//    → 外部调 Resume() → 下次 Step() 继续
//
//  动态追加:
//    模块在 slot 内可以 task->PushShard(new_pack)
//    → 追加到 shards_ 末尾
//    → 比如"编译"任务：第1步解析 → 第2步编译 → 第3步链接
//      每步都是模块 Push 进来的
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
//  【与 v1 的区别】
//   v1: Task 只是一个壳，Dispatch(pack) 转发给 ModuleLifeManager，
//       执行权不在 Task 手里，无法暂停/取消/查进度。
//   v2: Task 掌握执行节奏，每次 Step() 只推进一步，
//       分片之间可以插入暂停/取消/回调。
//       模块变成被动响应（通过 EventBus Slot），不主动控制流程。
// =================================================================

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
        return true;
    }

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
    size_t   GetTotalShards()   const { std::lock_guard lock(shards_mutex_); return shards_.size(); }
    void     SetID(uint32_t id)       { id_.store(id); }

    /// 进度 = 已完成分片数 / 总分片数
    float GetProgress() const
    {
        std::lock_guard lock(shards_mutex_);
        size_t n = shards_.size();
        return n > 0 ? static_cast<float>(current_) / n : 0.0f;
    }

    /// 获取当前正在执行的分片参数包（执行后读取结果用）
    ParmarPack* CurrentPack()
    {
        std::lock_guard lock(shards_mutex_);
        if (shards_.empty()) return nullptr;
        if (current_ >= shards_.size()) return shards_.back().get();
        return shards_[current_].get();
    }

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

    // ---- 对象池索引（O(1) 归还）----
    // pool_index_ 在 TasksPool::CreateTasks() 时设置，
    // Release() 时通过这个索引直接放回 free_indices_。
    size_t pool_index_{ 0 };

    // ---- 生命周期回调 ----
    Callback on_complete_;
    Callback on_error_;
};
