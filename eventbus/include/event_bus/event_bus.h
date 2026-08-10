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
//  EventBus — type-safe signal-slot dispatcher
// =================================================================
class EventBus
{
public:
    static EventBus& GetInstance()
    {
        return *static_cast<EventBus*>(GetEventBusPtr());
    }

    // ---- Register signal ----
    std::string RegisterSignal(const std::string& name, std::unique_ptr<ISignal> signal)
    {
        if (name.empty() || !signal) return {};
        std::unique_lock lock(bus_mutex_);
        signals_[name] = std::move(signal);
        return name;
    }

    template <class... Args>
    std::string RegisterSignal(const std::string& name)
    {
        if (name.empty()) return {};
        auto signal = std::make_unique<Signal<Args...>>();
        std::unique_lock lock(bus_mutex_);
        signals_[name] = std::move(signal);
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
    /// v2.4: Holds bus_mutex_ shared_lock during slot execution.
    /// This prevents RemoveSignal (needs unique_lock) from deleting
    /// the signal while slots are running — guaranteeing module
    /// lifetime safety during DLL unload.
    template <class... Args>
    bool Emit(const std::string& signal_name, Args&&... args)
    {
        std::shared_lock lock(bus_mutex_);
        auto it = signals_.find(signal_name);
        if (it == signals_.end()) return false;

        using SignalType = Signal<std::decay_t<Args>...>;
        auto* typed = dynamic_cast<SignalType*>(it->second.get());
        if (!typed) return false;
        if (!typed->IsEnabled()) return false;

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
    std::unordered_map<std::string, std::unique_ptr<ISignal>> signals_;
    mutable std::shared_mutex bus_mutex_;
};
