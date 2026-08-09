/// =================================================================
///  IModule — 模块开发者只需要 include 这一个头文件
/// =================================================================
///
///  写一个模块只需要知道 3 件事:
///    1. 继承 IModule，实现 GetName()
///    2. 在 OnInit() 里用 REGISTER_FUNC 注册函数
///    3. 用 ParmarPack 读写参数 (Get/Set/GetAsOr)
///
///  示例:
///    class MyModule : public IModule {
///        const char* GetName() const override { return "MyModule"; }
///        bool OnInit() override {
///            REGISTER_FUNC("hello", "Say hello", {
///                LOG_PLAIN("Hello!");
///                pack->success = true;
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
//  IModule — module base class (public SDK)
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
