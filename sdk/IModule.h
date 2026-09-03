/// =================================================================
///  IModule — 模块接口基类（SDK 自包含，不依赖 EventBus）
/// =================================================================
///
///  定义模块契约。写一个模块只需要知道 3 件事:
///    1. 继承 SDK 基类 ModuleBase（见 sdk/ModuleBase.h），实现 GetName()
///    2. 在 OnInit() 里用 REGISTER_FUNC 注册函数
///    3. 用 ParmarPack 读写参数 (Get/Set/GetAsOr)
///
///  继承关系:
///    IModule (本文件, 接口契约)  ←  ModuleBase (sdk/, + SetResult/SetError 收尾助手)
///                                        ↑
///                                你的模块 (继承 ModuleBase)
///
///  接线谁来做？——宿主。模块只声明「我有哪些命令」，宿主（core/ModuleLifeManager）
///  通过 ForEachFunc 枚举命令表，把每个命令接成 EventBus 信号。所以 SDK 层
///  对 EventBus / 线程池 / 任务分片一无所知。
///  直接继承 IModule 只适合「独立测试模块逻辑」（见 test/sdk/imodule_test.cpp）。
///
///  示例（注册进框架的正确写法，见 modules/samples/CalculatorModule.h）:
///    #include "sdk/sdk.h"                      // 一个 include 到位
///    class MyModule : public ModuleBase {
///        const char* GetName() const override { return "MyModule"; }
///        bool OnInit() override {
///            REGISTER_FUNC("hello", "Say hello", {
///                SetResult(pack, "Hi!");       // 成功收尾
///            });
///            return true;
///        }
///    };
///
///  EventBus? 线程池? 分片任务? 都不用管。框架帮你处理好。
/// =================================================================

#pragma once
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>

#include "ParmarPack.h"

// 线程安全的输出。替代 std::cout，多线程时不会交错乱码。
#define LOG_PLAIN(msg)                                              \
    do {                                                            \
        std::lock_guard<std::mutex> _lk(IModule::OutputMutex());  \
        std::cout << msg << std::endl;                              \
    } while(0)

// 注册一个命令函数。三个参数：函数ID、帮助文本、函数体。
// 函数体里可以用 "pack" 访问 ParmarPack。
// 示例：REGISTER_FUNC("add", "a+b", { pack->return_value = "3"; pack->success = true; });
#define REGISTER_FUNC(id, description, body) \
    RegisterFunc(id, description, [this](ParmarPack* pack) body)

// =================================================================
//  IModule — 模块接口基类 (public SDK interface)
//  具体模块请继承 sdk/ModuleBase（本类 + SetResult/SetError 收尾助手）
// =================================================================
class IModule
{
public:
    virtual ~IModule() = default;

    // ---- lifecycle ----
    virtual bool        OnInit()     { return true; }
    virtual void        OnShutdown() {}

    // ---- core interface ----
    virtual void        Execute(ParmarPack* pack);
    virtual void        Help(ParmarPack* pack);
    virtual void        ReturnValue(ParmarPack* pack);
    virtual const char* GetName()    const = 0;
    virtual int         GetVersion() const { return 1; }

    // ---- function table enumeration (host uses this to wire commands) ----
    // 遍历「命令 id → 描述」。宿主侧接线（AddModule）据此把每个命令注册成
    // EventBus 信号；本类对 EventBus 一无所知，只暴露自己的命令表。
    virtual void ForEachFunc(
        const std::function<void(const std::string& id, const std::string& desc)>& visitor) const
    {
        for (const auto& kv : funcs_)
        {
            auto it = explains_.find(kv.first);
            visitor(kv.first, (it != explains_.end()) ? it->second : std::string());
        }
    }

    // ---- global output mutex (single lock for all cout output) ----
    static std::mutex& OutputMutex() { static std::mutex m; return m; }

protected:
    // ---- RegisterFunc (called by REGISTER_FUNC macro) ----
    void RegisterFunc(const std::string& id, const std::string& desc,
                      std::function<void(ParmarPack*)> fn)
    {
        funcs_[id]    = std::move(fn);
        explains_[id] = desc;
    }

    // ---- function table ----
    using Func = std::function<void(ParmarPack*)>;
    std::unordered_map<std::string, Func>        funcs_;
    std::unordered_map<std::string, std::string> explains_;
};

// =================================================================
//  Default implementations
// =================================================================

inline void IModule::Execute(ParmarPack* pack)
{
    if (!pack) return;
    auto it = funcs_.find(pack->func_id);
    if (it == funcs_.end())
    {
        pack->success = false;
        pack->error.code = 404;
        pack->error.message = std::string(GetName())
            + ": function not found: " + pack->func_id;
        std::cerr << pack->error.message << "\n";
        return;
    }

    // print explanation header (thread-safe via LOG_PLAIN)
    if (pack->show_explanation)
    {
        auto exp_it = explains_.find(pack->func_id);
        std::string desc = (exp_it != explains_.end())
            ? exp_it->second
            : std::string("(no description)");
        LOG_PLAIN("--- [" << GetName() << "." << pack->func_id
                  << "] " << desc << " ---");
    }

    it->second(pack);  // invoke the registered function
}

inline void IModule::Help(ParmarPack*)
{
    LOG_PLAIN("=== " << GetName() << " Help ===");
    for (const auto& [id, desc] : explains_)
        LOG_PLAIN("  " << id << " : " << desc);
}

inline void IModule::ReturnValue(ParmarPack* pack)
{
    if (pack && pack->return_value.empty())
        pack->return_value = std::string(GetName()) + " done.";
}
