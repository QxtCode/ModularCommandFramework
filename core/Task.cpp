/// =================================================================
///  Task::Step() — 分片任务的核心执行函数
/// =================================================================
///
///  这是整个框架的"心跳"。每次调用 Step() 推进一个分片。
///
///  【执行流程图解】
///
///  假设命令: "-m:Calculator -f:add -v:a|1,b|2"
///
///   shards_[0] = ParmarPack { mod_id="Calculator", func_id="add", params={a:[1],b:[2]} }
///   current_ = 0
///
///   Step(bus) 被调用:
///     │
///     ├─ ① 检查状态：COMPLETED/FAILED/PAUSED 直接返回 false
///     ├─ ② IDLE → RUNNING（首次执行）
///     ├─ ③ 取 shards_[0] → 组装信号名 = "Calculator" + "." + "add" = "Calculator.add"
///     ├─ ④ bus.Emit("Calculator.add", pack)
///     │     │
///     │     └─ EventBus 内部:
///     │         signals_.find("Calculator.add") → 找到 Signal<ParmarPack*>
///     │         Signal::TraverseSlots(pack)
///     │           → 遍历 slots_: {1: Slot(Execute)}
///     │           → Slot::Run(pack) → func_(pack)
///     │           → 即 CalculatorModule::Execute(pack)
///     │           → 查 funcs_["add"] → lambda: a+b → pack->return_value = "3"
///     │           → pack->success = true
///     │
///     ├─ ⑤ 发射 "task.shard_done" 通知（外部可监听进度）
///     ├─ ⑥ current_++ → current_ = 1
///     ├─ ⑦ 检查 PAUSED（分片间暂停点）
///     └─ ⑧ current_ >= shards_.size() → 全部完成
///           → state_ = COMPLETED
///           → 发射 "task.completed" 通知
///           → 触发 on_complete_ 回调（如果有）
///
///  【信号找不到时】
///  如果 bus.Emit() 返回 false（信号未注册），说明没有模块处理这个命令:
///    pack->success = false
///    pack->error = { code: 404, message: "Signal not found: ..." }
///    state_ = FAILED → 触发 on_error_ 回调
///
///  【分片间暂停机制】
///  在 current_++ 之后、下一个 shard 执行之前检查 PAUSED。
///  这意味着：
///    - 当前 shard 一定会执行完（不能中断一个正在执行的 Slot）
///    - 如果被暂停，下次 Step() 从下一个 shard 继续
///    - 暂停时发射 "task.paused" 通知，外部可监听
///
///  【为什么用 EventBus 而不是直接调用模块？】
///   1. 解耦：Task 不知道有哪些模块，只知道信号名
///   2. 多播：一个信号可以有多个 Slot 响应（例如 logging、metrics）
///   3. 异常隔离：一个 Slot 抛异常不影响其他 Slot
///   4. 动态：模块可以在运行时注册/注销信号和 Slot
/// =================================================================

#include "Task.h"
#include "core/ITaskPersistence.h"  // TaskRecord
#include "event_bus/event_bus.h"
#include "modules/logging/LogModule.h"

bool Task::Step(EventBus& bus)
{
    // ---- ① 状态检查 ----
    State s = state_.load();
    if (s == State::COMPLETED || s == State::FAILED)
        return false;           // 已完成或失败，不再执行
    if (s == State::PAUSED)
        return false;           // 暂停中，等待 Resume()
    if (s == State::IDLE)
        state_.store(State::RUNNING);  // 首次执行：IDLE → RUNNING

    // ---- ②~③ 分片检查 + 发射（全程持锁保护 shards_）----
    // v2.4: shards_mutex_ prevents concurrent PushShard/Reset from
    // corrupting the vector. We hold the lock through Emit because
    // the raw pack pointer belongs to shards_ and must not be freed
    // by a concurrent Reset() during the Emit call.
    ParmarPack* pack = nullptr;
    std::string signal;
    {
        std::lock_guard lock(shards_mutex_);

        if (current_ >= shards_.size())
        {
            state_.store(State::COMPLETED);
            bus.Emit("task.completed", GetID());
            if (on_complete_) on_complete_(*this);
            return true;
        }

        pack = shards_[current_].get();
        signal = pack->mod_id + "." + pack->func_id;
    }

    LOG_DEBUG(std::string("Task emit: ") + signal);
    bool emitted = bus.Emit(signal, pack);

    // ---- ④ 处理信号未找到 ----
    if (!emitted)
    {
        pack->success = false;
        pack->error.code = ErrorCode::SIGNAL_NOT_FOUND;
        pack->error.message = "Signal not found: " + signal;

        LOG_ERROR(pack->error.message);

        state_.store(State::FAILED);

        bus.Emit("task.result", GetID(), pack->success,
                 pack->error.code, pack->error.message.c_str(),
                 pack->return_value.c_str());

        if (on_error_) on_error_(*this);
        return false;
    }

    // ---- ⑤ 分片完成通知 ----
    bus.Emit("task.shard_done", GetID(), static_cast<uint32_t>(current_));

    // ---- ⑥~⑧ 推进分片 + 完成检查（加锁保护）----
    {
        std::lock_guard lock(shards_mutex_);

        current_++;

        if (state_.load() == State::PAUSED)
        {
            bus.Emit("task.paused", GetID(), static_cast<uint32_t>(current_));
            return false;
        }

        if (current_ >= shards_.size())
        {
            state_.store(State::COMPLETED);

            auto* result_pack = (current_ > 0) ? shards_.back().get() : nullptr;
            if (result_pack)
            {
                bus.Emit("task.result", GetID(), result_pack->success,
                         result_pack->error.code, result_pack->error.message.c_str(),
                         result_pack->return_value.c_str());
            }

            bus.Emit("task.completed", GetID());
            if (on_complete_) on_complete_(*this);
        }
    }

    return true;
}

// ================================================================
//  v2.6: Restore — 从 TaskRecord 恢复分片状态
// ================================================================
bool Task::Restore(const TaskRecord& record)
{
    // 只能在 IDLE 状态恢复（PAUSED 任务被 Release 后回到 IDLE）
    State s = state_.load();
    if (s != State::IDLE) return false;

    std::lock_guard lock(shards_mutex_);

    // 从 shards_json 重建分片
    shards_.clear();
    current_ = record.current_shard;
    declared_total_.store(record.declared_total);
    id_.store(record.task_id);

    // 恢复至少一个分片（Step() 依赖 shards_ 非空）
    {
        auto pack = std::make_unique<ParmarPack>();
        if (!record.shards_json.empty()) {
            // TODO: 从 JSON 反序列化完整分片数据
            pack->mod_id  = "restored";
            pack->func_id = "restored";
        } else {
            // 空快照: 创建占位分片，保证 Step() 不崩溃
            pack->mod_id  = "restored";
            pack->func_id = "restored";
        }
        pack->success = false;
        pack->owner_task = this;
        shards_.push_back(std::move(pack));
    }

    // 恢复状态
    if (record.state == "PAUSED")
        state_.store(State::PAUSED);
    else if (record.state == "FAILED")
        state_.store(State::FAILED);
    else if (record.state == "RUNNING")
        state_.store(State::RUNNING);
    else
        state_.store(State::IDLE);

    return true;
}
