/// ============================================================
///  CalculatorModule — 纯 SDK 模块（演示第三方开发者写法）
/// ============================================================
///
///  只 include "sdk/sdk.h"，继承 ModuleBase。不 include core、不链 eventbus。
///  命令接线由宿主（ModuleLifeManager）通过 ForEachFunc 自动完成。
///
///  Build:
///    add_library(CalculatorModule SHARED CalculatorModule.cpp)
///    target_link_libraries(CalculatorModule PRIVATE test_shell_sdk)

#pragma once
#include "sdk/sdk.h"

class CalculatorModule : public ModuleBase
{
public:
    const char* GetName() const override { return "Calculator"; }

    bool OnInit() override
    {
        REGISTER_FUNC("add", "a + b (-v:a|1,b|2)", {
            int a = pack->GetAsOr<int>("a", 0);
            int b = pack->GetAsOr<int>("b", 0);
            pack->return_value = std::to_string(a + b);
            pack->success = true;
        });

        REGISTER_FUNC("mul", "a * b (-v:a|3,b|4)", {
            int a = pack->GetAsOr<int>("a", 0);
            int b = pack->GetAsOr<int>("b", 0);
            pack->return_value = std::to_string(a * b);
            pack->success = true;
        });

        return true;
    }
};
