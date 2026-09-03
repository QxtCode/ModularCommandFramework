/// =================================================================
///  ModuleBaseObject — 兼容基类（core 侧，额外带 ConnectToEventBus）
/// =================================================================
///
///  继承 SDK 的 ModuleBase，额外保留 ConnectToEventBus()，供「手动接线 / 测试后门」。
///
///  正常注册请直接用 SDK 基类 ModuleBase —— AddModule 已改为宿主侧通过
///  IModule::ForEachFunc 枚举命令表自行接线，不再需要 ConnectToEventBus。
///  本类仅为历史代码 / 需要手动管理生命周期的场景保留。
///
///  继承关系（v2.8）:
///    IModule (sdk, 契约)  ←  ModuleBase (sdk, +收尾助手)
///                                ↑
///                        ModuleBaseObject (core, +ConnectToEventBus 后门)
/// =================================================================

#pragma once
#include <memory>
#include "sdk/ModuleBase.h"
#include "event_bus/event_bus.h"

class ModuleBaseObject : public ModuleBase
{
public:
    // ============================================================
    //  ConnectToEventBus — 手动把已注册函数接到 EventBus 信号（测试/后门）
    // ============================================================
    //  v2.4: shared_ptr self → Slot 持 weak_ptr，模块卸载时安全跳过。
    //  v2.8: 正常注册路径（AddModule）已改用宿主侧 ForEachFunc 接线；
    //        本方法仅供需要手动管理生命周期 / 测试注入的场景使用。
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
