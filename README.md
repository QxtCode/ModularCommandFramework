# test_shell v2.7 — C++20 多线程命令框架

[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-blue)]()

一个跨平台、模块化、事件驱动的 C++20 控制台命令框架。支持热加载 DLL 插件、FTXUI 监控面板、分片任务流水线。

```
==========================================================
  M A I N    T H R E A D                 W O R K E R S
==========================================================

  ┌─────────────┐     ┌──────────┐     ┌──────────────┐
  │ > 输入线程   │────▶│LockQueue │     │  ThreadPool  │
  │  getline()  │     │ <string> │     │  ┌────┐┌────┐│
  └─────────────┘     └────┬─────┘     │  │ W1 ││ W2 ││
                           │           │  └──┬─┘└──┬─┘│
  ┌─────────────────────────────────┐   └──────┼─────┼─┘
  │        S h e l l E n g i n e    │          │     │
  │                                 │          ▼     ▼
  │  while(running) {              │   ┌─────────────────┐
  │    ① DrainResults()  ◀─────────│───│  ResultStore     │
  │    ② FlushMetrics()            │   │  (Producer/      │
  │    ③ WaitForWork()  ◀── cv     │   │   Consumer)      │
  │    ④ ProcessInput() ──────────▶│   └─────────────────┘
  │  }                             │
  └─────────────────────────────────┘   ┌─────────────────┐
           │                    │       │   EventBus      │
           ▼                    │       │  (DLL 单例)     │
  ┌─────────────────┐           │       │  Signal/Slot    │
  │ CommandParser   │           │       │  v2.6 快照 Emit │
  │ (TXT/JSON/自定义)│           └──────▶│  shared_mutex   │
  └────────┬────────┘                   └────────┬────────┘
           │                                     │
           ▼                                     ▼
  ┌─────────────────┐                   ┌─────────────────┐
  │   ParmarPack    │                   │ ModuleLifeMgr   │
  │  -m:Mod -f:Func │                   │ .AddModule()    │
  │  -v:key|val,... │                   │ .UnloadModule() │
  └────────┬────────┘                   │ .ScanPluginDir()│
           │                            └────────┬────────┘
           ▼                                     │
  ┌─────────────────┐                   ┌────────▼────────┐
  │   TasksPool     │                   │  PrintModule    │
  │  (预分配槽位)    │                   │  Calculator     │
  │  Acquire/Release│                   │  Logger         │
  └─────────────────┘                   │  MetricsCollect │
                                        │  (DLL 插件...)   │
                                        └─────────────────┘

  ┌─────────────────────────────────────────────────────────┐
  │  shell_monitor.exe (FTXUI) ◀── SharedMemory ── Metrics  │
  └─────────────────────────────────────────────────────────┘
```

## 一条命令的旅程

```
输入 "-m:Calculator -f:add -v:a|1,b|2"
  │
  ├─ ① CommandParser 解析 → ParmarPack { mod="Calculator", func="add", params={a:[1],b:[2]} }
  ├─ ② TasksPool::Acquire(pack) → 获取空闲槽位
  ├─ ③ ThreadPool::Enqueue → Worker 线程
  │      │
  │      └─ while (task->Step(bus)) {}  推完所有分片
  │             │
  │             └─ bus.Emit("Calculator.add", pack)
  │                    → EventBus → Signal → Slot → Module::Execute
  │
  ├─ ④ Worker 完成 → ResultStore::PushResult → Release(task)
  ├─ ⑤ 主循环 DrainResults → Formatter → "[OK] 3"
  └─ ⑥ 主循环 cv.wait_for(HasResults || input), 零空转
```

## 快速开始

### 构建

```bash
# 前置: CMake 3.20+, Ninja, C++20 编译器
# Windows: VS 2022 + vcvars64.bat
# Linux/macOS: g++-11+ / clang-14+

cmake --preset debug
cmake --build --preset debug
```

### 运行

```bash
cd out/build/debug
./test_shell                        # 交互模式
./test_shell --pool_size=16         # 自定义参数
```

### 命令格式

```
-m:模块名 -f:函数ID -v:key|val,...
-m:模块名 -f:help       → 列出模块函数
<enter>                 → 列出所有模块
/exit                   → 退出
```

### 演示

```
> -m:PrintModule -f:2 -v:Param|hello
--- [PrintModule.2] Print text ---
hello
[OK]

> -m:MetricsCollector -f:show
=== test_shell v2.6 Dashboard ===
  CPU:     0.01%
  Memory:  8520 KB
  Threads: 4 workers / 16 total
  Tasks:   0 active / 8 total (0 completed)
  Modules: 3 loaded
  Signals: 9 registered
  Uptime:  42s

> -m:MetricsCollector -f:dashboard
[OK] Dashboard launched   ← 启动外部 FTXUI 监控面板

> /exit
Goodbye.
```

## 架构

### ShellEngine 四步流水线

```
while (running) {
    ① DrainResults   → ResultStore 批量取 → Formatter → 输出
    ② FlushMetrics   → 写共享内存（供 shell_monitor 读取）
    ③ WaitForWork    → cv.wait_for(HasResults || input || !running) 100ms超时
    ④ ProcessInput   → input_queue 非阻塞取 → 解析 → SubmitTask
}
```

### 核心组件

| 组件 | 文件 | 职责 |
|------|------|------|
| **ShellApp** | `core/ShellApp.h` | 应用组装器, CLI/UI 共用组装逻辑 |
| **ShellEngine** | `core/ShellEngine.h` | 主循环引擎, v2.6 shared_ptr 共享状态 |
| **ResultStore** | `core/ResultStore.h` | 单例结果仓库, O(1) swap 批量消费 |
| **EventBus** | `eventbus/include/event_bus/event_bus.h` | DLL 单例 Signal/Slot, v2.6 快照式 Emit |
| **ModuleLifeManager** | `core/ModuleLifeManager.h` | 模块生命周期, DLL 热加载, 共享库管理 |
| **CommandParser** | `parser/CommandParser.h` | 多格式命令解析（TXT/JSON/自定义）|
| **TasksPool** | `core/TasksPool.h` | 预分配 Task 对象池, O(1) Acquire/Release |
| **ThreadPool** | `core/ThreadPool.h` | 固定大小工人线程池, 析构 join 安全 |
| **ShellConfig** | `core/Config.h` | INI + 命令行配置, 三级优先级 |
| **Platform 层** | `core/platform/` | SharedLibrary / SharedMemory / Process / FileSystem |

### 跨平台抽象

```
core/platform/
├── platform.h           # PLATFORM_WINDOWS / PLATFORM_EXPORT 宏
├── shared_library.h     # LoadLibrary ←→ dlopen
├── shared_memory.h      # CreateFileMapping ←→ shm_open
├── process.h            # CreateProcess ←→ posix_spawn
├── file_system.h        # FindFirstFile ←→ opendir
└── console.cpp          # 控制台 UTF-8 / 内存泄漏检测
```

- 编译时分发: CMake `if(WIN32)` 选 `_win.cpp`, `else()` 选 `_posix.cpp`
- **框架主体零 `#ifdef _WIN32`** — 只用 `PLATFORM_WINDOWS` / `PLATFORM_LINUX` / `PLATFORM_MACOS`

### v2.6 关键修复

| 修复 | 问题 | 方案 |
|------|------|------|
| **A1 快照式 Emit** | Emit 持锁执行 Slot, 慢 Slot 阻塞 RemoveSignal → 连锁阻塞 | 锁内拍 `shared_ptr` 快照 → 释放锁 → 锁外执行 |
| **ShellSharedState** | /exit 时 detach 输入线程, 进程退出时访问析构成员 | `running_` 等提升为堆上 `shared_ptr` 管理 |
| **prompt_ready 门控** | 输入线程不等主循环就打印 `>`, 输出交错 | `atomic<bool>` 门控, 主循环处理完才放行 |

## 模块开发

### 内置模块示例

```cpp
class PrintModule : public ModuleBaseObject {
public:
    const char* GetName() const override { return "PrintModule"; }

    bool OnInit() override {
        REGISTER_FUNC("hello", "Print greeting", {
            LOG_PLAIN("Hello World!");
            pack->success = true;
        });
        return true;
    }
};
```

### DLL 插件

```cpp
// my_module.cpp
class MyModule : public ModuleBaseObject { ... };
EXPORT_MODULE(MyModule)  // 自动生成 CreateModule / DestroyModule
```

```bash
# 编译为 .dll / .so, 放入 plugins/ 目录
# test_shell 启动时自动扫描加载
```

## 测试

```bash
# 全量（排除慢速）
./test_runner --gtest_filter=-StressGraduated.*:Stress60s.*:PeakStressTest.*

# 并发
./test_runner --gtest_filter=ConcurrencyStress.*

# 性能
./test_runner --gtest_filter=PeakStressTest.*

# 不稳定检测
./test_runner --gtest_filter=ShellEngineTest.* --gtest_repeat=5 --gtest_shuffle
```

### 测试规模

```
488 tests / 68 suites
├── ShellEngine (20)     引擎启停/并发/关闭竞态
├── ConcurrencyStress (8) Emit阻塞/TOCTOU/ChaosMonkey
├── InputThread (13)     生产者消费者/背压/数据完整性
├── DLLLifecycle (10)    加载卸载/并发执行/OnShutdown
├── PeakStressTest (6)   吞吐/爆发/持续/恢复
├── Platform (83)        跨平台 5 组专项
├── EventBus (26)        Signal/Slot/类型安全
├── Task/ThreadPool (30) 分片/暂停/池化
└── ...
```

### 性能基线 (i7-13700H, Debug)

| 指标 | 值 |
|------|-----|
| 峰值吞吐 | **~21,000 cmd/s**（真实完成数统计）|
| Burst 500 条 | **~2,230 cmd/s**, 99.6% 完成率 |
| 持续 10s | 内存增量 **+200KB** |
| 30 轮生命周期 | 内存增量 **+16KB** |
| 过载恢复 | **~108ms** |

## 目录结构

```
test_shell/
├── main.cpp                 # 入口: ShellApp 组装 → Run()
├── core/                    # 内核
│   ├── ShellApp.h           #   应用组装器（CLI/UI 共用）
│   ├── ShellEngine.h        #   主循环引擎
│   ├── ThreadPool.h         #   线程池
│   ├── TasksPool.h          #   对象池
│   ├── ResultStore.h        #   结果仓库
│   ├── ModuleLifeManager.h  #   模块生命周期
│   ├── Config.h             #   配置系统
│   └── platform/            #   跨平台抽象层
├── eventbus/                # EventBus 共享库 (DLL)
│   ├── include/event_bus/
│   │   ├── event_bus.h      #   Signal/Slot 调度器
│   │   ├── signal.h         #   类型安全信号
│   │   └── slot.h           #   weak_ptr 保护的槽
│   └── src/event_bus_dll.cpp
├── parser/                  # 命令解析器
├── sdk/                     # 公开 API
│   ├── IModule.h            #   模块基类 + LOG_PLAIN + REGISTER_FUNC
│   └── ParmarPack.h         #   参数包
├── modules/                 # 内置模块
├── monitor/                 # shell_monitor (FTXUI)
├── test/                    # 测试 (385 cases)
└── docs/                    # 文档 (中英双语)
```

## 文档索引

| 文档 | 内容 |
|------|------|
| [01-快速上手](docs/zh/01-快速上手.md) | 5 分钟写出第一个模块 |
| [02-参数包API](docs/zh/02-参数包API.md) | ParmarPack 完整 API |
| [03-线程安全](docs/zh/03-线程安全.md) | 锁策略与并发保证 |
| [04-DLL模块](docs/zh/04-DLL模块.md) | 插件开发与热加载 |
| [05-任务系统](docs/zh/05-任务系统.md) | 分片任务流程 |
| [06-监控系统](docs/zh/06-监控系统.md) | MetricsCollector + FTXUI |
| [07-架构概览](docs/zh/07-架构概览.md) | 完整架构说明 |
| [08-测试指南](docs/zh/08-测试指南.md) | 测试策略与模式 |
| [10-性能测试](docs/zh/10-性能测试.md) | 性能基线与调优 |
| [12-并发审计](docs/zh/12-并发审计.md) | 并发问题追踪 |
| [13-UI对接](docs/zh/13-UI对接.md) | QML 前端接入指南 |

## License

MIT
