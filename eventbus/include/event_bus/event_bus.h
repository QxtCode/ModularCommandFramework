#pragma once
#include <iostream>
#include <memory>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include "i_signal.h"
#include "signal.h"

// =================================================================
//  跨平台符号导出 — 由 platform.h 统一
//  Windows: __declspec(dllexport/dllimport)
//  Linux/macOS: __attribute__((visibility("default")))
// =================================================================
#include "core/platform/platform.h"

#ifdef EVENTBUS_DLL_EXPORTS
#define EVENTBUS_API PLATFORM_EXPORT
#else
#define EVENTBUS_API PLATFORM_IMPORT
#endif

/// 获取 EventBus 单例指针（位于 eventbus 共享库中）
extern "C" EVENTBUS_API void* GetEventBusPtr();

// =================================================================
//  EventBus — type-safe signal-slot dispatcher (v2.6 snapshot Emit)
// =================================================================
///
///  v2.4: shared_lock during execution prevents RemoveSignal from
///        deleting the signal while slots are running.
///  v2.6: A1 snapshot Emit — lock held only for signal lookup +
///        shared_ptr snapshot; Slots execute outside the lock, so
///        RemoveSignal/UnloadModule are no longer blocked by slow Slots.
///        signals_ uses shared_ptr to keep the signal alive even after
///        RemoveSignal erases it from the map.
///
class EventBus
{
public:
    static EventBus& GetInstance()
    {
        return *static_cast<EventBus*>(GetEventBusPtr());
    }

    // ---- Register signal ----
    /// v2.6: signals_ stores shared_ptr (was unique_ptr) so Emit can
    /// hold a snapshot copy while executing Slots outside the lock.
    std::string RegisterSignal(const std::string& name, std::unique_ptr<ISignal> signal)
    {
        if (name.empty() || !signal) return {};
        std::unique_lock lock(bus_mutex_);
        signals_[name] = std::shared_ptr<ISignal>(std::move(signal));
        return name;
    }

    template <class... Args>
    std::string RegisterSignal(const std::string& name)
    {
        if (name.empty()) return {};
        auto signal = std::make_unique<Signal<Args...>>();
        std::unique_lock lock(bus_mutex_);
        signals_[name] = std::shared_ptr<ISignal>(std::move(signal));
        return name;
    }

    // ---- Signal control ----
    bool DisableSignal(const std::string& name)
    {
        std::shared_lock lock(bus_mutex_);
        auto it = signals_.find(name);
        if (it == signals_.end()) return false;
        it->second->SetEnabled(false);
        return true;
    }

    /// v2.6: 快照式 Emit 释放 shared_lock 后才执行 Slot，RemoveSignal
    /// 不再被慢 Slot 阻塞 — unique_lock 立即可获得。
    bool RemoveSignal(const std::string& name)
    {
        std::unique_lock lock(bus_mutex_);
        return signals_.erase(name) > 0;
    }

    void PrintAllSignals() const
    {
        std::shared_lock lock(bus_mutex_);
        for (const auto& [name, signal] : signals_)
        {
            if (signal)
                std::cout << name << " ";
        }
        std::cout << std::endl;
    }

    std::vector<std::string> GetSignalNames() const
    {
        std::shared_lock lock(bus_mutex_);
        std::vector<std::string> names;
        for (const auto& [name, signal] : signals_)
            names.push_back(name);
        return names;
    }

    size_t GetSlotCount(const std::string& name) const
    {
        std::shared_lock lock(bus_mutex_);
        auto it = signals_.find(name);
        if (it == signals_.end()) return 0;
        return it->second->GetSlotCount();
    }

    // ---- Connect slots ----
    template <class... Args>
    size_t LinkSlot(const std::string& signal_name, std::unique_ptr<Slot<Args...>> slot)
    {
        std::shared_lock lock(bus_mutex_);
        auto it = signals_.find(signal_name);
        if (it == signals_.end()) return 0;
        auto* raw = it->second.get();
        if (raw->GetTypeIndex() != typeid(Signal<Args...>)) return 0;
        auto* typed = static_cast<Signal<Args...>*>(raw);
        return typed->ConnectSlot(std::move(slot));
    }

    template <class... Args, class Callable>
    size_t LinkSlotFunc(const std::string& signal_name, Callable&& callable)
    {
        auto slot = std::make_unique<Slot<Args...>>(std::forward<Callable>(callable));
        return LinkSlot<Args...>(signal_name, std::move(slot));
    }

    /// v2.4: Bind slot to a shared_ptr-managed object. The slot holds
    /// a weak_ptr and checks liveness before each execution. Slot::Run()
    /// locks a shared_ptr during execution to prevent use-after-free.
    template <class... Args, class T, class Callable>
    size_t LinkSlotFunc(const std::string& signal_name, std::shared_ptr<T> obj, Callable&& callable)
    {
        auto slot = std::make_unique<Slot<Args...>>(
            std::forward<Callable>(callable), std::move(obj));
        return LinkSlot<Args...>(signal_name, std::move(slot));
    }

    // ---- Slot removal ----
    bool DeleteSlot(const std::string& signal_name, size_t slot_id)
    {
        std::shared_lock lock(bus_mutex_);
        auto it = signals_.find(signal_name);
        if (it == signals_.end()) return false;
        return it->second->DeleteSlot(slot_id);
    }

    // ---- Emit (the core method) ----
    /// v2.6 A1 快照式 Emit：锁内拍 shared_ptr<ISignal> 快照 → 释放锁
    /// → 锁外执行 TraverseSlots。RemoveSignal 不再被慢 Slot 阻塞。
    /// 信号由 shared_ptr 保护：即使 RemoveSignal 在 Emit 期间删除了
    /// map 中的条目，快照持有的 shared_ptr 仍保持信号对象存活。
    template <class... Args>
    bool Emit(const std::string& signal_name, Args&&... args)
    {
        using SignalType = Signal<std::decay_t<Args>...>;

        // ★ v2.6: 锁内只拍快照，不执行 Slot
        std::shared_ptr<ISignal> signal_snapshot;
        SignalType* typed = nullptr;
        {
            std::shared_lock lock(bus_mutex_);
            auto it = signals_.find(signal_name);
            if (it == signals_.end()) return false;

            typed = dynamic_cast<SignalType*>(it->second.get());
            if (!typed) return false;
            if (!typed->IsEnabled()) return false;

            signal_snapshot = it->second;  // shared_ptr copy → ref count++
        }  // ★ 释放 shared_lock — RemoveSignal 可立即获得 unique_lock

        // ★ 锁外执行 Slot — 不再阻塞 RemoveSignal/UnloadModule/Dispatch
        typed->TraverseSlots(std::forward<Args>(args)...);
        return true;
    }

    // ---- Singleton protection ----
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    EventBus(EventBus&&) = delete;
    EventBus& operator=(EventBus&&) = delete;

    EventBus() = default;

private:
    /// v2.6: shared_ptr (was unique_ptr) — 使 Emit 可拍下信号快照，
    /// 即使 RemoveSignal 并发地删除 map 条目，快照仍保持信号存活。
    std::unordered_map<std::string, std::shared_ptr<ISignal>> signals_;
    mutable std::shared_mutex bus_mutex_;
};
