#pragma once
#include <atomic>
#include <string>
#include "core/ModuleBaseObject.h"
#include "core/ParmarPack.h"

/// 测试用 Mock 模块 — 记录所有调用
class MockModule : public ModuleBaseObject
{
public:
    explicit MockModule(std::string name = "mock_module") : name_(std::move(name)) {}

    const char* GetName() const override { return name_.c_str(); }

    bool OnInit() override
    {
        init_called = true;
        init_count.fetch_add(1);
        return init_result;
    }

    void OnShutdown() override { shutdown_called = true; }

    void Execute(ParmarPack* pack) override
    {
        exec_called = true;
        exec_count.fetch_add(1);
        if (pack)
        {
            last_mod_id  = pack->mod_id;
            last_func_id = pack->func_id;
            pack->success = true;
            pack->return_value = "MockModule executed: " + name_;
        }
    }

    void Help(ParmarPack* pack) override
    {
        help_called = true;
        if (pack) pack->success = true;
    }

    void ReturnValue(ParmarPack* pack) override
    {
        if (pack) pack->return_value = "MockModule result";
    }

    // ---- 测试标志 ----
    std::string name_;
    bool init_called     = false;
    bool shutdown_called = false;
    bool exec_called     = false;
    bool help_called     = false;
    bool init_result     = true;   // 设为 false 模拟初始化失败

    std::atomic<int> init_count{0};
    std::atomic<int> exec_count{0};

    std::string last_mod_id;
    std::string last_func_id;
};

/// 初始化失败的模块
class FailingModule : public ModuleBaseObject
{
public:
    bool OnInit() override { return false; }
    const char* GetName() const override { return "FailingModule"; }
    void Execute(ParmarPack*) override {}
    void Help(ParmarPack*) override {}
    void ReturnValue(ParmarPack*) override {}
};
