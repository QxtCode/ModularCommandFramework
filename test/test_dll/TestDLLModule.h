/// Test DLL module — 用来测试 DLL 加载/卸载/执行/生命周期
#pragma once
#include "core/ModuleBaseObject.h"
#include "event_bus/event_bus.h"
#include <atomic>
#include <cstdlib>
#include <iostream>

// 全局计数器（DLL 内定义，测试通过 GetProcAddress 读取）
extern std::atomic<int> g_test_dll_init_count;
extern std::atomic<int> g_test_dll_shutdown_count;
extern std::atomic<int> g_test_dll_exec_count;
extern bool g_test_dll_oninit_result;  // OnInit 返回此值（可通过 export 函数或环境变量控制）

class TestDLLModule : public ModuleBaseObject
{
public:
    const char* GetName() const override { return "TestDLL"; }

    bool OnInit() override
    {
        g_test_dll_init_count.fetch_add(1);

        // 环境变量可覆盖（测试用）
        const char* env = std::getenv("TESTDLL_ONINIT_FAIL");
        if (env && env[0] == '1')
            g_test_dll_oninit_result = false;

        if (!g_test_dll_oninit_result)
            return false;

        REGISTER_FUNC("ping", "Echo back a message (-v:msg|hello)", {
            auto it = pack->params.find("msg");
            std::string msg = "pong";
            if (it != pack->params.end() && !it->second.empty())
                msg = it->second[0];
            std::cout << "[TestDLL] ping: " << msg << std::endl;
            pack->return_value = msg;
            pack->success = true;
        });

        return true;
    }

    void OnShutdown() override
    {
        g_test_dll_shutdown_count.fetch_add(1);
    }

    void Execute(ParmarPack* pack) override
    {
        g_test_dll_exec_count.fetch_add(1);
        ModuleBaseObject::Execute(pack);
    }
};
