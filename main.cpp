/// =================================================================
///  test_shell v2.7 — 多线程命令框架
/// =================================================================
///
///  启动流程（v2.7 起，组装逻辑收进 ShellApp）：
///
///    main()
///      ├─ ① InitConsole / InitLeakDetection — 进程级平台初始化
///      ├─ ② ShellApp app(argc, argv)      — 组装（见 core/ShellApp.h）
///      │      ├─ 加载配置（exe 目录 .cfg → 命令行覆盖）
///      │      ├─ 注册模块（Print/Log/Metrics/TaskManager + 插件扫描）
///      │      ├─ 创建 ShellEngine + 注入持久化后端
///      │      └─ 注册 EventBus 信号
///      └─ ③ app.Run()                     — 进入事件驱动主循环（阻塞直到 /exit）
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
///  main() 只负责进程初始化 + 组装 + 启动。组装细节在 ShellApp，
///  循环逻辑在 ShellEngine。
///
///  要接 UI？看 docs/zh/13-UI对接.md —— ShellApp 已经把「组装」抽好了，
///  UI 只需换「运行方式」：后台线程 RunWithoutInput() + SetResultSink()。
/// =================================================================

#include "core/platform/platform.h"   // InitConsole / InitLeakDetection
#include "core/ShellApp.h"

int main(int argc, char* argv[])
{
    InitConsole();
    InitLeakDetection();

    // 组装 + 启动。CLI 场景 Run() 阻塞直到 /exit。
    ShellApp app(argc, argv);
    app.Run();

    LOG_PLAIN("Goodbye.");
    return 0;
}
