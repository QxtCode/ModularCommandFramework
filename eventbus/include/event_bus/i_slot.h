#pragma once
#include <typeindex>

/// Base interface for all slot types — non-template, safe for DLL boundaries.
class ISlot
{
public:
    virtual ~ISlot() = default;
    virtual bool IsAlive() = 0;
    virtual std::type_index GetTypeIndex() = 0;
};
