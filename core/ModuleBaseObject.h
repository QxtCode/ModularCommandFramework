/// =================================================================
///  ModuleBaseObject — 框架内核用的基类（开发者用 IModule 就够了）
/// =================================================================
///
///  继承 IModule，只多一件事：ConnectToEventBus()
///  把 REGISTER_FUNC 登记的函数接到 EventBus 信号上。
///
///  正常流程：AddModule → OnInit() → ConnectToEventBus() → 注册完成
///  开发者不用手动调 ConnectToEventBus，AddModule 会自动调。
/// =================================================================

#pragma once
#include <memory>
#include "sdk/IModule.h"
#include "event_bus/event_bus.h"

class ModuleBaseObject : public IModule
{
public:
    // ============================================================
    //  ConnectToEventBus — wire all registered functions to signals
    // ============================================================
    //  v2.4: Accepts shared_ptr to self. Slots hold weak_ptr via
    //  WeakRefHolder — if the module is unloaded while a slot is
    //  in-flight, the slot detects IsExpired() and skips safely.
    //  Called by ModuleLifeManager::AddModule() after OnInit().
    void ConnectToEventBus(std::shared_ptr<ModuleBaseObject> self)
    {
        auto& bus = EventBus::GetInstance();
        for (const auto& [id, fn] : funcs_)
        {
            std::string sig = std::string(GetName()) + "." + id;
            bus.RegisterSignal<ParmarPack*>(sig);
            bus.LinkSlotFunc<ParmarPack*>(sig, self,
                [this](ParmarPack* p) { Execute(p); });
        }
    }
};
