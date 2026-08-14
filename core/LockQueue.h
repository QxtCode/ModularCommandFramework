#pragma once
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>

/// =================================================================
///  LockQueue<T> — 线程安全队列
/// =================================================================
///
///  并发模式: 多生产者 / 单消费者
///  难度: ★★☆☆☆（和 ResultStore 类似，多了一个 Pop 会睡觉等）
///
///  三个方法的使用场景:
///    Push  → 生产者放东西（主线程注入命令、输入线程读 stdin）
///    Pop   → 消费者等东西（阻塞，没东西就睡觉，不烧 CPU）
///    TryPop → 消费者看有没有东西（非阻塞，没东西立刻返回 false）
///
///  cv.wait 的行为:
///    ① 检查 predicate（queue 非空？）
///    ② 如果空：原子地 unlock mutex + 睡觉
///    ③ 被 notify_one 叫醒 → 重新 lock mutex → 再检查 predicate
///    → 醒来时 queue 一定非空，锁已持好
/// =================================================================

template <typename T>
class LockQueue
{
public:
    LockQueue() = default;

    void Push(std::unique_ptr<T> ptr)
    {
        if (!ptr) return;
        {
            std::lock_guard lock(mutex_);
            queue_.push(std::move(ptr));
        }  // ★ 先解锁，再通知 — 避免"通知了但别人还被锁挡在门外"
        cv_.notify_one();
    }

    /// 阻塞取 — 队列空就睡觉等，被 Push 叫醒再拿
    std::unique_ptr<T> Pop()
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty(); });
        auto ptr = std::move(queue_.front());
        queue_.pop();
        return ptr;
    }

    /// 非阻塞取 — 队列空直接返回 false，不等
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
