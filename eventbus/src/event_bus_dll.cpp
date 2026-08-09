/// DLL export: provides the singleton EventBus pointer.
/// All consumers (main exe, plugin DLLs) call GetEventBusPtr()
/// to get the ONE shared EventBus instance living inside this DLL.

#include "event_bus/event_bus.h"

extern "C" EVENTBUS_API void* GetEventBusPtr()
{
    static EventBus instance;
    return &instance;
}
