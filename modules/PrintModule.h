#pragma once
#include <iostream>
#include "core/ModuleBaseObject.h"
#include "event_bus/event_bus.h"

// ============================================================
//  PrintModule — 演示：用 REGISTER_FUNC 注册 3 个函数
// ============================================================
//  给初学者看的范例模块

class PrintModule : public ModuleBaseObject
{
public:
    const char* GetName() const override { return "PrintModule"; }
    int GetVersion() const override { return 1; }

    bool OnInit() override
    {
        // 函数1: 打印版本
        REGISTER_FUNC("1", "Print version",
        {
            LOG_PLAIN(GetName() << " v" << GetVersion());
            pack->success = true;
            pack->return_value = "version " + std::to_string(GetVersion());
        });

        REGISTER_FUNC("2", "Print text (-v:Param|text)",
        {
            auto& vals = pack->GetAll("Param");
            if (!vals.empty())
                for (const auto& v : vals)
                    LOG_PLAIN(v);
            else
                LOG_PLAIN("[PrintModule] No Param provided.");
            pack->success = true;
        });

        REGISTER_FUNC("3", "Dump all parameters",
        {
            LOG_PLAIN("=== Params ===");
            for (const auto& [k, vals] : pack->params)
                for (const auto& v : vals)
                    LOG_PLAIN("  " << k << " = " << v);
            pack->success = true;
        });

        return true;
    }
};
