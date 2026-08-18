/// =================================================================
///  ShellApp — 应用组装器（把 main 里的组装逻辑收进来）
/// =================================================================
///
///  为什么有它：
///    main.cpp 原来有一大段「组装」代码 —— 加载配置、注册模块、创建
///    引擎、注入持久化后端、挂 TaskManager。这段代码 CLI 版要用、
///    QML 版也要用，不抽出来就得写两遍，以后加一个模块要改两处。
///
///  ShellApp 只做「组装」，不做「运行」。因为 CLI 和 UI 的运行方式
///  根本不同：
///    - CLI：主线程阻塞 engine.Run()，靠 stdin 输入线程喂命令
///    - UI ：后台线程跑 engine.RunWithoutInput()，靠 InjectCommand 喂
///    所以组装完就把引擎交出去，调用方自己决定怎么跑。
///
///  用法（CLI）：
///      int main(int argc, char* argv[]) {
///          ShellApp app(argc, argv);
///          app.Run();
///      }
///
///  用法（UI / QML）：
///      ShellApp app;                    // 默认配置，不读命令行参数
///      app.GetEngine().SetResultSink(...);   // 结果出口
///      std::thread t([]{ app.GetEngine().RunWithoutInput(); });
///
///  重要限制：
///    框架里的 ModuleLifeManager / EventBus / ResultStore 都是进程级
///    单例，所以一个进程只能组装一次。ShellApp 是「一次性」的 ——
///    别指望「重启内核」按钮能 stop 后再 start，单例不会自动重置。
/// =================================================================

#pragma once
#include <iostream>
#include <memory>
#include <string>

#include "core/Config.h"
#include "core/MemTaskStore.h"
#include "core/ModuleLifeManager.h"
#include "core/ShellEngine.h"
#include "core/platform/process.h"
#include "modules/PrintModule.h"
#include "modules/TaskManagerModule.h"
#include "modules/logging/LogModule.h"

class ShellApp {
public:
    /// 组装开关
    struct Options {
        bool quiet = false;   // true = 不打印横幅/帮助/配置（UI 用）
    };

    /// @param argc/argv  命令行参数（--key=value 覆盖配置）。UI 场景传 0/nullptr。
    /// @param opt        组装开关
    explicit ShellApp(int argc = 0, char* argv[] = nullptr, Options opt = {});

    /// 组装完成后交出引擎，调用方决定怎么跑（Run / RunWithoutInput）。
    ShellEngine& GetEngine() { return *engine_; }

    /// 配置对象（加载后），可读取/修改。
    ShellConfig& GetConfig() { return cfg_; }

    /// CLI 便捷入口：打印帮助 → 阻塞 engine.Run() → Shutdown。
    /// UI 不用这个方法，自己开线程跑 RunWithoutInput()。
    void Run();

private:
    void LoadConfig(int argc, char* argv[]);
    void SetupModules();

    ShellConfig cfg_;
    Options     opt_;

    // ★ 成员声明顺序 = 析构顺序（反过来）。
    //   store_ 先声明 → 后析构 → 比 engine 活得久。
    //   ShellEngine 析构时 Shutdown 会 join Worker，Worker 收尾可能调用
    //   store_->Save()，所以 store 必须最后才销毁，否则悬空指针。
    std::shared_ptr<MemPersistence> store_;
    std::unique_ptr<ShellEngine>    engine_;
};

// ================================================================
//  实现
// ================================================================

inline ShellApp::ShellApp(int argc, char* argv[], Options opt)
    : opt_(opt)
{
    LoadConfig(argc, argv);

    if (!opt_.quiet) {
        std::cout << "===== test_shell v2.7 =====" << std::endl << std::endl;
    }

    // 引擎组装：store 先建，engine 后建（析构顺序保证 store 活得久）。
    store_  = std::make_shared<MemPersistence>();
    engine_ = std::make_unique<ShellEngine>(cfg_.pool_size, cfg_.workers);
    engine_->SetTaskPersistence(store_.get());

    SetupModules();
}

inline void ShellApp::LoadConfig(int argc, char* argv[]) {
    // 从 exe 同目录加载配置文件（而非当前工作目录）
    std::string cfg_path = platform::Process::ExeDir() + "test_shell.cfg";
    cfg_.LoadFromFile(cfg_path);
    if (argc > 0 && argv) cfg_.ApplyArgs(argc, argv);
    if (!opt_.quiet) cfg_.Print();
}

inline void ShellApp::SetupModules() {
    auto& mgr = ModuleLifeManager::GetInstance();
    auto& bus = EventBus::GetInstance();

    mgr.AddModule(std::make_unique<PrintModule>());
    mgr.AddModule(std::make_unique<LogModule>());
    mgr.AddModule(std::make_unique<MetricsCollector>());

    // 应用配置中的日志级别（覆盖 log.conf 设置）
    LogFac::Instance().GetLogger().SetLevel(StringToLogLevel(cfg_.log_level));

    LOG_INFO("test_shell v2.7 started");

    bus.RegisterSignal<uint32_t, bool, int, const char*, const char*>("task.result");

    mgr.ScanPluginDirectory(cfg_.plugin_dir);

    // TaskManager 依赖引擎的 Pool/Workers + 持久化后端，所以放在引擎之后
    mgr.AddModule(std::make_unique<TaskManagerModule>(
        engine_->GetPool(), engine_->GetWorkers(), store_.get()));
}

inline void ShellApp::Run() {
    if (!opt_.quiet) {
        std::cout << std::endl
                  << "  -m:ModuleName -f:FuncID -v:key|val,..." << std::endl
                  << "  -m:ModuleName -f:help  -> list functions" << std::endl
                  << "  <enter>  -> list modules" << std::endl
                  << "  /exit    -> quit" << std::endl << std::endl;
    }

    engine_->Run();
    engine_->Shutdown();   // 幂等；析构时还会再走一次，安全
}
