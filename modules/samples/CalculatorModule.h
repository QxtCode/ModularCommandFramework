/// ============================================================
///  CalculatorModule — external DLL module example
/// ============================================================
///
///  Demonstrates the new ParmarPack API (v2.3):
///    pack->GetAsOr<int>("key", default)
///    pack->Get("key")           → optional<string>
///    pack->Set("key", "val")
///
///  Build:
///    add_library(CalculatorModule SHARED CalculatorModule.cpp)
///    target_include_directories(CalculatorModule PRIVATE ${test_shell_ROOT})
///    target_link_libraries(CalculatorModule PRIVATE eventbus)

#pragma once
#include "core/ModuleBaseObject.h"
#include "event_bus/event_bus.h"
#include <iostream>

class CalculatorModule : public ModuleBaseObject
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
