#pragma once
#include <memory>
#include "i_weak_ref_holder.h"

template <typename T>
class WeakRefHolder : public IWeakRefHolder
{
public:
    explicit WeakRefHolder(std::shared_ptr<T> obj)
        : weak_(std::move(obj)) {}

    bool IsExpired() const override
    {
        return weak_.expired();
    }

    std::shared_ptr<void> Lock() override
    {
        return weak_.lock();  // shared_ptr<T> implicitly converts to shared_ptr<void>
    }

private:
    std::weak_ptr<T> weak_;
};
