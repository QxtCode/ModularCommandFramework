#pragma once
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>

// ============================================================
//  LockQueue<T> — 线程安全队列
// ============================================================

template <typename T>
class LockQueue
{
public:
    LockQueue() = default;

    void Push(std::unique_ptr<T> ptr)
    {
        if (!ptr) return;
        std::lock_guard lock(mutex_);
        queue_.push(std::move(ptr));
        cv_.notify_one();
    }

    std::unique_ptr<T> Pop()
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty(); });
        auto ptr = std::move(queue_.front());
        queue_.pop();
        return ptr;
    }

    bool TryPop(std::unique_ptr<T>& out)
    {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    size_t Size() const
    {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    bool Empty() const
    {
        std::lock_guard lock(mutex_);
        return queue_.empty();
    }

    LockQueue(const LockQueue&) = delete;
    LockQueue& operator=(const LockQueue&) = delete;

private:
    std::queue<std::unique_ptr<T>> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};
