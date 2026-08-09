#pragma once
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#include "Task.h"

// ============================================================
//  TasksPool — Task 对象池（预分配，避免频繁 new/delete）
// ============================================================

class EventBus;

class TasksPool
{
public:
    explicit TasksPool(size_t capacity)
    {
        if (capacity == 0)
            throw std::invalid_argument("TasksPool capacity must be > 0");
        CreateTasks(capacity);
    }

    ~TasksPool() = default;

    /// 从池中取出一个空闲 Task 并传入参数包。池子满时返回 nullptr。
    Task* Acquire(std::unique_ptr<ParmarPack> pack)
    {
        if (!pack) return nullptr;

        std::lock_guard lock(mutex_);
        if (free_indices_.empty()) return nullptr;

        size_t idx = free_indices_.front();
        free_indices_.pop_front();

        auto& task = tasks_[idx];
        task->Assign(std::move(pack));
        task->pool_index_ = idx;  // O(1) 归还时直接定位
        return task.get();
    }

    /// 归还 Task 到池子（O(1)）
    void Release(Task* task)
    {
        if (!task) return;
        task->Reset();

        std::lock_guard lock(mutex_);
        free_indices_.push_back(task->pool_index_);
    }

    /// 执行一步：遍历所有 RUNNING 的 task，各执行一个 Step
    void Tick(EventBus& bus)
    {
        std::lock_guard lock(mutex_);
        for (auto& t : tasks_)
        {
            if (t && t->GetState() == Task::State::RUNNING)
                t->Step(bus);
        }
    }

    size_t GetFreeCount() const { std::lock_guard lock(mutex_); return free_indices_.size(); }
    size_t GetTotalCount() const { return tasks_.size(); }

    TasksPool(const TasksPool&) = delete;
    TasksPool& operator=(const TasksPool&) = delete;

private:
    void CreateTasks(size_t capacity)
    {
        tasks_.reserve(capacity);
        for (size_t i = 0; i < capacity; ++i)
        {
            auto t = std::make_unique<Task>();
            t->pool_index_ = i;
            tasks_.emplace_back(std::move(t));
            free_indices_.push_back(i);
        }
    }

    std::vector<std::unique_ptr<Task>> tasks_;
    std::list<size_t>                  free_indices_;
    mutable std::mutex                 mutex_;
};
