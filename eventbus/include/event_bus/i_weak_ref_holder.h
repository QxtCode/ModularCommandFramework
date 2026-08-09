#pragma once
#include <memory>

/// Tracks lifetime of a shared_ptr-managed object across DLL boundaries.
class IWeakRefHolder
{
public:
    virtual ~IWeakRefHolder() = default;
    virtual bool IsExpired() const = 0;

    /// Try to lock the weak_ptr → shared_ptr.
    /// Returns nullptr if the object has already been deleted.
    /// The returned shared_ptr keeps the object alive until released.
    virtual std::shared_ptr<void> Lock() = 0;
};
