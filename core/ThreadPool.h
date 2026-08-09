/// =================================================================
///  ThreadPool — 固定数量工人线程
/// =================================================================
///
///  用餐厅比喻：
///    - 构造函数 = 请 N 个厨师上岗
///    - Enqueue    = 把菜单贴窗口（非阻塞）
///    - Submit     = 贴菜单 + 拿小票(future)，做完可取结果
///    - 析构函数   = 打烊，等厨师把手上的菜做完再下班
///
///  关键设计：
///    - 厨师没活就睡(cv.wait)，不空转烧CPU
///    - 锁只在接单那一刻持有，做菜时不锁 → 厨师之间不互相等
///    - 一个厨师炒糊了(exception)不会炸厨房 → try-catch
///
///  用法:
///    ThreadPool pool(4);
///    pool.Enqueue([]() { do_work(); });

#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool
{
public:
    explicit ThreadPool(size_t num_threads)
        : stop_(false)
    {
        for (size_t i = 0; i < num_threads; ++i)
            workers_.emplace_back(&ThreadPool::WorkerLoop, this);
    }

    ~ThreadPool()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (std::thread& w : workers_)
            if (w.joinable()) w.join();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /// 提交任务（不管结果，类似 fire-and-forget）
    void Enqueue(std::function<void()> job)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            queue_.push(std::move(job));
        }
        cv_.notify_one();
    }

    /// 提交任务 + 拿 future（可以 .get() 等结果）
    template<typename F, typename... Args>
    auto Submit(F&& f, Args&&... args)
        -> std::future<decltype(f(args...))>
    {
        using ReturnType = decltype(f(args...));
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<ReturnType> result = task->get_future();

        Enqueue([task]() { (*task)(); });
        return result;
    }

    size_t Pending() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    size_t WorkerCount() const { return workers_.size(); }

private:
    void WorkerLoop()
    {
        while (true)
        {
            std::function<void()> job;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() {
                    return stop_ || !queue_.empty();
                });

                if (stop_ && queue_.empty())
                    return;  // 打烊了，走人

                job = std::move(queue_.front());
                queue_.pop();
            }  // ★ 释放锁 → 别的厨师可以同时接单

            try { job(); }
            catch (...) {}  // 炒糊了不影响别的厨师
        }
    }

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> queue_;
    mutable std::mutex                mutex_;
    std::condition_variable           cv_;
    bool                              stop_ = false;
};
