/// =================================================================
///  Slot<Args...> — 类型安全回调包装
/// =================================================================
///
///  持有 callable + 指向所属模块的 weak_ptr。
///  模块被卸载后 weak_ptr 过期，Slot 自动跳过执行（IsAlive 返回 false）。
///
///  v2.5: Run() 用 try-catch(...) 包裹 func_() 调用作为最后防线 —
///  一个模块回调抛异常不会崩掉整个进程。异常后 slot 标记为 dead。
/// =================================================================

#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <typeindex>
#include "i_slot.h"
#include "weak_ref_holder.h"

template <class... Args>
class Slot : public ISlot
{
public:
    using Func = std::function<void(Args...)>;

    explicit Slot(Func func)
        : func_(std::move(func))
    {
        is_alive_.store(true);
    }

    template <class T>
    Slot(Func func, std::shared_ptr<T> bound_obj)
        : func_(std::move(func)),
          weak_holder_(std::make_unique<WeakRefHolder<T>>(std::move(bound_obj)))
    {
        is_alive_.store(true);
    }

    bool Run(Args... args)
    {
        // Lock weak_ptr → shared_ptr to keep the module alive during
        // func_() execution. If the module was deleted between the
        // IsAlive() check in TakeSnapshot and now, Lock() returns
        // nullptr and we skip safely.
        auto keep_alive = weak_holder_ ? weak_holder_->Lock() : nullptr;
        if (weak_holder_ && !keep_alive) return false;

        if (!UpdateState()) return false;

        // ============================================================
        //  v2.5: Last line of defense — catch(...) safety net
        // ============================================================
        //  Third-party module code may throw unhandled exceptions.
        //  We catch them here to prevent one misbehaving slot from
        //  crashing the entire process.  The slot is marked dead so
        //  it won't be invoked again.
        try {
            func_(args...);
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[Slot::Run] Exception: " << e.what()
                      << " (slot marked dead)" << std::endl;
            MarkDead();
            return false;
        } catch (...) {
            std::cerr << "[Slot::Run] Unknown exception (slot marked dead)"
                      << std::endl;
            MarkDead();
            return false;
        }
    }

    bool IsAlive() override
    {
        UpdateState();
        return is_alive_;
    }

    /// Mark this slot as dead (e.g., after it threw an exception).
    /// Dead slots are excluded from future snapshots and cleaned up.
    void MarkDead() { is_alive_.store(false); }

    std::type_index GetTypeIndex() override
    {
        return typeid(Slot<Args...>);
    }

    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;

private:
    bool UpdateState()
    {
        if (weak_holder_ && weak_holder_->IsExpired())
            is_alive_.store(false);
        return is_alive_;
    }

    Func func_;
    std::atomic_bool is_alive_;
    std::unique_ptr<IWeakRefHolder> weak_holder_;
};
