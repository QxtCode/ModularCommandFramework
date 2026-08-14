/// =================================================================
///  TaskManagerModule — 任务暂停/恢复/列表 (v2.7)
/// =================================================================
///
///  命令:
///    -m:TaskManager -f:pause  -v:id|N   暂停任务 N
///    -m:TaskManager -f:resume -v:id|N   恢复任务 N
///    -m:TaskManager -f:list             列出所有暂停中的任务
///
///  Pause:
///    1. TasksPool::FindTask(id) 找到 Task* → Pause() 设 PAUSED
///    2. Worker 在当前分片完成后检测 PAUSED → Snapshot → Save → Release
///
///  Resume:
///    1. ITaskPersistence::Load(id) 读快照
///    2. TasksPool::Acquire → Task::Restore(snapshot) → Resume → Enqueue
/// =================================================================

#pragma once
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "core/ITaskPersistence.h"
#include "core/ModuleBaseObject.h"
#include "core/Task.h"
#include "core/TasksPool.h"
#include "core/ThreadPool.h"
#include "core/ResultStore.h"
#include "event_bus/event_bus.h"
#include "sdk/IModule.h"
#include "sdk/ParmarPack.h"

class TaskManagerModule : public ModuleBaseObject {
public:
    /// @param pool       TasksPool 引用
    /// @param workers    ThreadPool 引用
    /// @param store      持久化后端（可选，nullptr = 纯内存模式）
    TaskManagerModule(TasksPool& pool, ThreadPool& workers,
                      ITaskPersistence* store = nullptr)
        : pool_(pool), workers_(workers), store_(store) {}

    const char* GetName() const override { return "TaskManager"; }

    bool OnInit() override {
        REGISTER_FUNC("pause", "Pause a task (-v:id|N)", {
            // 先按 int 读入，用 -1 作为"未传参数"的哨兵；
            // 确认非负后再转 uint32_t，避免无符号/有符号来回 cast 的绕圈。
            int id = pack->GetAsOr<int>("id", -1);
            if (id < 0) {
                pack->success = false;
                pack->error.message = "Usage: -m:TaskManager -f:pause -v:id|N";
                return;
            }
            uint32_t tid = static_cast<uint32_t>(id);

            Task* task = pool_.FindTask(tid);
            if (!task) {
                pack->success = false;
                pack->error.message = "Task " + std::to_string(tid) + " not found";
                return;
            }
            if (task->GetState() == Task::State::PAUSED) {
                pack->success = true;
                pack->return_value = "Task " + std::to_string(tid) + " already paused";
                return;
            }

            task->Pause();  // 标记 PAUSED，Worker 在分片边界处理
            pack->success = true;
            pack->return_value = "Task " + std::to_string(tid) + " pausing...";
        });

        REGISTER_FUNC("resume", "Resume a paused task (-v:id|N)", {
            // 同 pause：先按 int 读，-1 为哨兵，确认非负再转 uint32_t。
            int id = pack->GetAsOr<int>("id", -1);
            if (id < 0) {
                pack->success = false;
                pack->error.message = "Usage: -m:TaskManager -f:resume -v:id|N";
                return;
            }
            uint32_t tid = static_cast<uint32_t>(id);

            // 1. 从持久化后端加载快照（如果有）
            if (store_) {
                auto opt = store_->Load(tid);
                if (!opt.has_value()) {
                    pack->success = false;
                    pack->error.message = "No saved state for task " + std::to_string(tid);
                    return;
                }

                // 2. Acquire 新槽位 + 恢复
                auto dummy = std::make_unique<ParmarPack>();
                Task* task = pool_.Acquire(std::move(dummy));
                if (!task) {
                    pack->success = false;
                    pack->error.message = "No free task slot";
                    return;
                }

                task->Restore(*opt);
                task->Resume();  // PAUSED → RUNNING

                workers_.Enqueue([this, task]() {
                    try { while (task->Step(EventBus::GetInstance())) {} }
                    catch (...) {
                        auto* cp = task->CurrentPack();
                        if (cp) {
                            cp->success = false;
                            cp->error.code = ErrorCode::INTERNAL_ERROR;
                            cp->error.message = "Unhandled exception in task";
                        }
                    }

                    // 恢复的任务若再次暂停，同样保存快照（否则无法再次 resume）
                    if (task->GetState() == Task::State::PAUSED && store_) {
                        store_->Save(task->ExportRecord());
                    }

                    auto* cp = task->CurrentPack();
                    if (cp) {
                        auto result = std::make_unique<ParmarPack>(*cp);
                        ResultStore::Get().PushResult(task->GetID(), std::move(result));
                    }
                    pool_.Release(task);
                });

                store_->Delete(tid);
                pack->success = true;
                pack->return_value = "Task " + std::to_string(tid) + " resumed";
            } else {
                // 纯内存模式：直接从 TasksPool 找 PAUSED 任务恢复
                Task* task = pool_.FindTask(tid);
                if (!task) {
                    pack->success = false;
                    pack->error.message = "Task " + std::to_string(tid) + " not found";
                    return;
                }
                if (task->GetState() != Task::State::PAUSED) {
                    pack->success = false;
                    pack->error.message = "Task " + std::to_string(tid) + " is not paused";
                    return;
                }
                task->Resume();
                pack->success = true;
                pack->return_value = "Task " + std::to_string(tid) + " resumed";
            }
        });

        REGISTER_FUNC("list", "List paused tasks", {
            std::ostringstream oss;
            oss << "=== Paused Tasks ===";

            if (store_) {
                auto all = store_->LoadAll();
                if (all.empty()) {
                    oss << "\n  (none)";
                } else {
                    for (auto& r : all) {
                        oss << "\n  Task " << r.task_id
                            << " [" << r.state << "]"
                            << " shard " << r.current_shard;
                    }
                }
            } else {
                // 纯内存：遍历 pool 找 PAUSED
                bool found = false;
                for (size_t i = 0; i < pool_.GetTotalCount(); ++i) {
                    // TasksPool 没有公开迭代器，通过 FindTask 不可行
                    // 这里作为接口预留，实际需要遍历支持
                }
                oss << "\n  (store backend required for listing)";
            }

            pack->return_value = oss.str();
            pack->success = true;
        });

        return true;
    }

private:
    TasksPool& pool_;
    ThreadPool& workers_;
    ITaskPersistence* store_;
};
