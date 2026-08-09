/// =================================================================
///  test_shell v2.5 — 多线程命令框架
/// =================================================================
///
///  一条命令的完整旅程：
///
///    输入 "-m:Calc -f:add -v:a|1,b|2"
///      │
///      ├─ ① CommandParser 解析成 ParmarPack
///      ├─ ② TasksPool 拿一个空闲 Task
///      ├─ ③ ThreadPool.Enqueue → 工人线程干活
///      │      │
///      │      └─ while (task->Step(bus)) {}  ← 推完所有分片
///      │             │
///      │             └─ bus.Emit("Calc.add", pack)
///      │                    → EventBus → Module::Execute → lambda
///      │
///      ├─ ④ 工人干完 → PushResult → 归还 Task（槽位立即可复用）
///      ├─ ⑤ 主循环 DrainResultStore → Formatter → LOG_PLAIN
///      └─ ⑥ 主循环 cv.wait_for(ResultStore || input_queue)，零空转
///
///  main() 只负责组装。循环逻辑在 ShellEngine 里。
/// =================================================================

#define _CRTDBG_MAP_ALLOC
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#include <crtdbg.h>
#endif

#include "core/Config.h"
#include "core/ModuleLifeManager.h"
#include "core/ShellEngine.h"
#include "modules/PrintModule.h"
#include "modules/logging/LogModule.h"
#include "event_bus/event_bus.h"
#include "sdk/IModule.h"

using namespace std;

int main(int argc, char* argv[])
{
#ifdef _WIN32
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    // ================================================================
    //  Config — file + CLI / 配置加载
    // ================================================================
    ShellConfig cfg;
    cfg.LoadFromFile("test_shell.cfg");   // 可选，不存在则用默认值
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

#ifdef _WIN32
    mgr.ScanPluginDirectory(cfg.plugin_dir);
#endif

    // ================================================================
    //  Engine — event-driven main loop, configured from cfg
    // ================================================================
    ShellEngine engine(cfg.pool_size, cfg.workers);

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
