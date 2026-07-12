#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include<memory>
#include<iostream>
template<typename T>
class LockQueue {
public:
    LockQueue() = default;
    ~LockQueue() = default;
    // 入队：接收一个 unique_ptr
    void push(std::unique_ptr<T> ptr) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::cout << "入队成功" << std::endl;
            q_.push(std::move(ptr));
        }
        cv_.notify_one(); // 解锁后再通知，减少锁竞争
    }

    // 出队：阻塞等待直到有元素，返回 unique_ptr
    std::unique_ptr<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !q_.empty(); });
        auto ptr = std::move(q_.front());
        q_.pop();
        return ptr;
    }

    // 非阻塞尝试出队（可选）
    bool try_pop(std::unique_ptr<T>& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }

    const size_t Size()
    {
        return q_.size();
    }

    LockQueue(const LockQueue&) = delete;
    LockQueue& operator=(const LockQueue&) = delete;
private:
    std::queue<std::unique_ptr<T>> q_;
    std::mutex mutex_;
    std::condition_variable cv_;
};