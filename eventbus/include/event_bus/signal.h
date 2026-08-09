#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include "i_signal.h"
#include "slot.h"

constexpr size_t kSlotStartId = 0;

template <class... Args>
class Signal : public ISignal
{
public:
    Signal() : next_slot_id_(kSlotStartId)
    {
        is_enabled_.store(true);
    }

    ~Signal() = default;

    bool DeleteSlot(size_t slot_id) override
    {
        std::unique_lock lock(slot_mutex_);
        return slots_.erase(slot_id) > 0;
    }

    size_t GetSlotCount() const override
    {
        std::shared_lock lock(slot_mutex_);
        size_t alive = 0;
        for (const auto& [id, slot] : slots_)
            if (slot && slot->IsAlive()) ++alive;
        return alive;
    }

    size_t ConnectSlot(std::unique_ptr<ISlot> slot) override
    {
        if (!slot) return 0;
        if (slot->GetTypeIndex() != typeid(Slot<Args...>))
            return 0;

        std::shared_ptr<Slot<Args...>> shared_slot(
            static_cast<Slot<Args...>*>(slot.release()));

        std::unique_lock lock(slot_mutex_);
        size_t id = ++next_slot_id_;
        slots_[id] = std::move(shared_slot);
        return id;
    }

    std::type_index GetTypeIndex() const override
    {
        return typeid(Signal<Args...>);
    }

    bool SetEnabled(bool enabled) override
    {
        is_enabled_.store(enabled);
        return is_enabled_;
    }

    bool IsEnabled() const override
    {
        return is_enabled_;
    }

    void TraverseSlots(Args... args)
    {
        std::vector<size_t> dead_ids;
        std::vector<std::pair<size_t, std::shared_ptr<Slot<Args...>>>> snapshot;

        {
            std::shared_lock lock(slot_mutex_);
            for (auto& [id, slot_ptr] : slots_)
            {
                if (slot_ptr && slot_ptr->IsAlive())
                    snapshot.emplace_back(id, slot_ptr);
                else
                    dead_ids.emplace_back(id);
            }
        }

        for (auto& [id, slot_ptr] : snapshot)
        {
            try
            {
                slot_ptr->Run(args...);
            }
            catch (...)
            {
                dead_ids.emplace_back(id);
            }
        }

        RemoveDeadSlots(dead_ids);
    }

    /// v2.4: 拍下所有活着 Slot 的快照（shared_ptr 保证 Slot 对象不会被释放）。
    /// 调用方在 bus_mutex_ 外执行 Slot，不再阻塞 RemoveSignal。
    std::vector<std::shared_ptr<Slot<Args...>>> TakeSnapshot()
    {
        std::vector<std::shared_ptr<Slot<Args...>>> snapshot;
        std::shared_lock lock(slot_mutex_);
        for (auto& [id, slot_ptr] : slots_)
        {
            if (slot_ptr && slot_ptr->IsAlive())
                snapshot.push_back(slot_ptr);
        }
        return snapshot;
    }

    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

private:
    void RemoveDeadSlots(const std::vector<size_t>& ids)
    {
        if (ids.empty()) return;
        std::unique_lock lock(slot_mutex_);
        for (auto id : ids)
            slots_.erase(id);
    }

    size_t next_slot_id_;
    std::unordered_map<size_t, std::shared_ptr<Slot<Args...>>> slots_;
    mutable std::shared_mutex slot_mutex_;
    std::atomic_bool is_enabled_;
};
