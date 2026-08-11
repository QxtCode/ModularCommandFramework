/// =================================================================
///  ResultStore — 结果仓库（Producer-Consumer）
/// =================================================================
///
///  并发模式: 多生产者(Worker线程) → 单消费者(主线程)
///  难度: ★☆☆☆☆（最容易的并发类，从这里开始学）
///
///  生活比喻: 餐厅出餐台
///    - PushResult = 厨师放一道菜上去，按铃
///    - Drain = 服务员一次性把所有菜端走
///    - HasResults = 服务员探头看"有没有菜？"
///
///  为什么用 swap 而不是逐条 pop？
///    锁只持有一瞬间——swap 两个 vector 的指针，O(1)。
///    如果逐条 pop + push，锁要持有 O(n) 的时间。
///
///  线程安全保证:
///    - PushResult: lock → push_back → notify → unlock（锁极短）
///    - Drain:      lock → swap → unlock（瞬间释放）
///    - HasResults: lock → !empty() → unlock（只读）
///
///  与 EventBus 的关系:
///    EventBus 是实时通知（"菜做好了！"），ResultStore 是实物交接。
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
