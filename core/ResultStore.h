/// =================================================================
///  ResultStore — 结果仓库（Producer-Consumer 解耦层）
/// =================================================================
///
///  Worker 线程干完活，把结果 Push 进来，敲一下铃。
///  消费者（CLI / UI / 测试）按自己的节奏 Drain。
///
///  设计要点：
///    - Singleton — 全局唯一的结果集散地
///    - HasResults() — 只读查询，供 cv predicate 使用（无副作用）
///    - Drain()      — 批量取走所有结果，用 swap 做 O(1) 交换
///    - PushResult() — 工人线程调用，锁区间极短
///
///  与 EventBus 的关系：
///    - EventBus:     实时信号流（订阅者在线程内零延迟响应）
///    - ResultStore:  结构化数据仓库（消费者按自己节奏批量处理）
///    两者互补，不互相替代。
/// =================================================================

#pragma once
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include "sdk/ParmarPack.h"

// ================================================================
//  ResultItem — 单条结果数据
// ================================================================
struct ResultItem {
    uint64_t    task_id = 0;
    std::unique_ptr<ParmarPack> pack;               // 结果数据
    std::chrono::steady_clock::time_point done_time; // 完成时间戳

    ResultItem() : done_time(std::chrono::steady_clock::now()) {}
};

// ================================================================
//  ResultStore — 线程安全的结果仓库（单例）
// ================================================================
class ResultStore {
public:
    static ResultStore& Get() {
        static ResultStore instance;
        return instance;
    }

    // ================================================================
    //  Producer API（Worker 线程调用）
    // ================================================================

    /// 存入结果，锁区间极短（push_back + notify），不阻塞工人。
    void PushResult(uint64_t task_id, std::unique_ptr<ParmarPack> pack) {
        auto item = std::make_unique<ResultItem>();
        item->task_id = task_id;
        item->pack    = std::move(pack);
        {
            std::lock_guard lock(mutex_);
            store_.push_back(std::move(item));
        }
        cv_.notify_one();
    }

    // ================================================================
    //  Consumer API（主线程 / UI 线程调用）
    // ================================================================

    /// 批量取出所有待处理结果。O(1) swap，持锁极短。
    std::vector<std::unique_ptr<ResultItem>> Drain() {
        std::vector<std::unique_ptr<ResultItem>> batch;
        {
            std::lock_guard lock(mutex_);
            batch.swap(store_);
        }
        return batch;
    }

    /// 只读查询：仓库里有没有待处理结果？
    /// 供 cv predicate 使用，无副作用。
    bool HasResults() const {
        std::lock_guard lock(mutex_);
        return !store_.empty();
    }

    /// 暴露 cv 引用，消费者可用自己的 mutex + HasResults() 组成 predicate。
    std::condition_variable& GetCV() { return cv_; }

    // ================================================================
    //  查询
    // ================================================================

    /// 结果数（瞬时快照，可能已过时）
    size_t Size() const {
        std::lock_guard lock(mutex_);
        return store_.size();
    }

    /// 清空（测试 reset 用）
    void Clear() {
        std::lock_guard lock(mutex_);
        store_.clear();
    }

    ResultStore(const ResultStore&) = delete;
    ResultStore& operator=(const ResultStore&) = delete;

private:
    ResultStore() = default;

    std::vector<std::unique_ptr<ResultItem>> store_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};
