#pragma once
#include <memory>
#include <typeindex>
#include "i_slot.h"

/// Base interface for all signal types — non-template, safe for DLL boundaries.
class ISignal
{
public:
    virtual ~ISignal() = default;
    virtual bool DeleteSlot(size_t slot_id) = 0;
    virtual size_t GetSlotCount() const = 0;
    virtual size_t ConnectSlot(std::unique_ptr<ISlot> slot) = 0;
    virtual std::type_index GetTypeIndex() const = 0;
    virtual bool SetEnabled(bool enabled) = 0;
    virtual bool IsEnabled() const = 0;
};
