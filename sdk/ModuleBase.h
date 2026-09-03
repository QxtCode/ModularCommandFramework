/// =================================================================
///  ModuleBase — SDK 模块基类（开发者实际继承这个）
/// =================================================================
///
///  继承 IModule，只多两个「收尾助手」，把最常用的样板收敛掉：
///     SetResult(pack, "42")            → success=true + return_value
///     SetError(pack, code, "boom")     → success=false + error
///
///  注意：本类对 EventBus / 线程池 / 任务分片一无所知——这是「纯 SDK」。
///  真正把命令接到 EventBus 信号上的接线动作在宿主侧（core/ModuleLifeManager），
///  宿主通过 IModule::ForEachFunc 枚举命令表自行接线。
///
///  用法（一个 include 到位）：
///     #include "sdk/sdk.h"
///     class MyModule : public ModuleBase {
///         const char* GetName() const override { return "MyModule"; }
///         bool OnInit() override {
///             REGISTER_FUNC("add", "a+b", {
///                 int a = pack->GetAsOr<int>("a", 0);
///                 int b = pack->GetAsOr<int>("b", 0);
///                 SetResult(pack, std::to_string(a + b));
///             });
///             return true;
///         }
///     };
/// =================================================================

#pragma once
#include <string>
#include <utility>

#include "IModule.h"

class ModuleBase : public IModule
{
public:
    /// 成功收尾：置 success=true 并写返回值。
    static void SetResult(ParmarPack* pack, std::string value = {})
    {
        if (!pack) return;
        pack->success = true;
        pack->return_value = std::move(value);
    }

    /// 失败收尾：置 success=false 并写错误码 + 消息。
    static void SetError(ParmarPack* pack, int code, std::string msg)
    {
        if (!pack) return;
        pack->success = false;
        pack->error.code = code;
        pack->error.message = std::move(msg);
    }
};
