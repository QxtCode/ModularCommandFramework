/// =================================================================
///  test_shell v2.5 — 多线程命令框架
/// =================================================================
///
///  启动流程：
///
///    main()
///      ├─ ① InitConsole / InitLeakDetection — 平台初始化
///      ├─ ② Config — platform::Process::ExeDir() + .cfg → 命令行覆盖
///      ├─ ③ ModuleLifeManager — 注册内置模块 + ScanPluginDirectory()
///      ├─ ④ ShellEngine(pool_size, workers) — 引擎组装
///      └─ ⑤ engine.Run() — 进入事件驱动主循环（阻塞直到 /exit）
///
///  一条命令的完整旅程（引擎内部）：
///
///    输入 "-m:Calc -f:add -v:a|1,b|2"
///      │
///      ├─ ① CommandParser 解析成 ParmarPack
///      ├─ ② TasksPool::Acquire(pack) → 拿一个空闲 Task
///      ├─ ③ ThreadPool::Enqueue → Worker 线程
///      │      │
///      │      └─ while (task->Step(bus)) {}  ← 推完所有分片
///      │             │
///      │             └─ bus.Emit("Calc.add", pack)
///      │                    → EventBus → Module::Execute → lambda
///      │
///      ├─ ④ Worker 完成 → ResultStore::PushResult → Release(task)
///      ├─ ⑤ 主循环 DrainResults → Formatter → LOG_PLAIN
///      └─ ⑥ 主循环 cv.wait_for(HasResults || input), 零空转
///
///  main() 只负责组装。循环逻辑在 ShellEngine 里。
/// =================================================================

#include "core/platform/platform.h"
#include "core/platform/process.h"

#include <iostream>

#include "core/Config.h"
#include "core/ModuleLifeManager.h"
#include "core/ShellEngine.h"
#include "modules/PrintModule.h"
#include "modules/logging/LogModule.h"
#include "modules/TaskManagerModule.h"
#include "core/MemTaskStore.h"
#include "event_bus/event_bus.h"
#include "sdk/IModule.h"

using namespace std;

int main(int argc, char* argv[])
{
    InitConsole();
    InitLeakDetection();

    // ================================================================
    //  Config — file + CLI / 配置加载
    // ================================================================
    ShellConfig cfg;
    // 从 exe 同目录加载配置文件（而非当前工作目录）
    std::string cfg_path = platform::Process::ExeDir() + "test_shell.cfg";
    cfg.LoadFromFile(cfg_path);
    cfg.ApplyArgs(argc, argv);            // 命令行覆盖
    cfg.Print();

    cout << "===== test_shell v2.5 =====" << endl << endl;

    // ================================================================
    //  Modules — register before engine starts
    // ================================================================
    auto& mgr = ModuleLifeManager::GetInstance();
    auto& bus = EventBus::GetInstance();

    mgr.AddModule(make_unique<PrintModule>());
    mgr.AddModule(make_unique<LogModule>());
    mgr.AddModule(make_unique<MetricsCollector>());

    // 应用配置中的日志级别（覆盖 log.conf 设置）
    LogFac::Instance().GetLogger().SetLevel(StringToLogLevel(cfg.log_level));

    LOG_INFO("test_shell v2.5 started");

    bus.RegisterSignal<uint32_t, bool, int, const char*, const char*>("task.result");

    mgr.ScanPluginDirectory(cfg.plugin_dir);

    // ================================================================
    //  Engine — event-driven main loop, configured from cfg
    // ================================================================
    ShellEngine engine(cfg.pool_size, cfg.workers);

    // v2.6: TaskManager — pause/resume/list（进程内有效）
    auto taskStore = std::make_shared<MemPersistence>();
    mgr.AddModule(std::make_unique<TaskManagerModule>(
        engine.GetPool(), engine.GetWorkers(), taskStore.get()));

    cout << endl
         << "  -m:ModuleName -f:FuncID -v:key|val,..." << endl
         << "  -m:ModuleName -f:help  -> list functions" << endl
         << "  <enter>  -> list modules" << endl
         << "  /exit    -> quit" << endl << endl;

    engine.Run();
    engine.Shutdown();

    // ================================================================
    //  Done
    // ================================================================
    LOG_PLAIN("Goodbye.");
    return 0;
}
