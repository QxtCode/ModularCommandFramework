# test_shell v2.6 — C++20 Multithreaded Command Framework

[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-blue)]()

A cross-platform, modular, event-driven C++20 console command framework. Features hot-reloadable DLL plugins, FTXUI monitoring dashboard, and shard-based task pipeline.

```
==========================================================
  M A I N    T H R E A D                 W O R K E R S
==========================================================

  ┌─────────────┐     ┌──────────┐     ┌──────────────┐
  │ > Input Thr  │────▶│LockQueue │     │  ThreadPool  │
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
           ▼                    │       │  (DLL Singleton)│
  ┌─────────────────┐           │       │  Signal/Slot    │
  │ CommandParser   │           │       │  v2.6 Snapshot  │
  │ (TXT/JSON/custom)│          └──────▶│  Emit            │
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
  │  (Pre-alloc)    │                   │  Calculator     │
  │  Acquire/Release│                   │  Logger         │
  └─────────────────┘                   │  MetricsCollect │
                                        │  (DLL plugins…)  │
                                        └─────────────────┘

  ┌─────────────────────────────────────────────────────────┐
  │  shell_monitor.exe (FTXUI) ◀── SharedMemory ── Metrics  │
  └─────────────────────────────────────────────────────────┘
```

## Journey of a Command

```
Input "-m:Calculator -f:add -v:a|1,b|2"
  │
  ├─ ① CommandParser → ParmarPack { mod="Calculator", func="add", params={a:[1],b:[2]} }
  ├─ ② TasksPool::Acquire(pack) → grab a free slot
  ├─ ③ ThreadPool::Enqueue → Worker thread
  │      │
  │      └─ while (task->Step(bus)) {}  // push all shards
  │             │
  │             └─ bus.Emit("Calculator.add", pack)
  │                    → EventBus → Signal → Slot → Module::Execute
  │
  ├─ ④ Worker done → ResultStore::PushResult → Release(task)
  ├─ ⑤ Main loop DrainResults → Formatter → "[OK] 3"
  └─ ⑥ Main loop cv.wait_for(HasResults || input), zero busy-wait
```

## Quick Start

### Build

```bash
# Prerequisites: CMake 3.20+, Ninja, C++20 compiler
# Windows: VS 2022 + vcvars64.bat
# Linux/macOS: g++-11+ / clang-14+

cmake --preset debug
cmake --build --preset debug
```

### Run

```bash
cd out/build/debug
./test_shell                        # interactive mode
./test_shell --pool_size=16         # custom pool size
```

### Command Format

```
-m:ModuleName -f:FuncID -v:key|val,...
-m:ModuleName -f:help    → list module functions
<enter>                  → list all modules
/exit                    → quit
```

### Demo

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
[OK] Dashboard launched   ← launches external FTXUI monitoring panel

> /exit
Goodbye.
```

## Architecture

### ShellEngine 4-Stage Pipeline

```
while (running) {
    ① DrainResults   → ResultStore batch consume → Formatter → output
    ② FlushMetrics   → write shared memory (for shell_monitor)
    ③ WaitForWork    → cv.wait_for(HasResults || input || !running) 100ms timeout
    ④ ProcessInput   → non-blocking drain input_queue → parse → SubmitTask
}
```

### Core Components

| Component | File | Role |
|-----------|------|------|
| **ShellEngine** | `core/ShellEngine.h` | Main loop engine, v2.6 shared_ptr shared state |
| **ResultStore** | `core/ResultStore.h` | Singleton result warehouse, O(1) swap batch consume |
| **EventBus** | `eventbus/include/event_bus/event_bus.h` | DLL singleton Signal/Slot, v2.6 snapshot Emit |
| **ModuleLifeManager** | `core/ModuleLifeManager.h` | Module lifecycle, DLL hot-load, shared lib management |
| **CommandParser** | `parser/CommandParser.h` | Multi-format command parsing (TXT/JSON/custom) |
| **TasksPool** | `core/TasksPool.h` | Pre-allocated Task object pool, O(1) Acquire/Release |
| **ThreadPool** | `core/ThreadPool.h` | Fixed-size worker thread pool, destructor join safety |
| **ShellConfig** | `core/Config.h` | INI + CLI config, three-tier priority |
| **Platform Layer** | `core/platform/` | SharedLibrary / SharedMemory / Process / FileSystem |

### Cross-Platform Abstraction

```
core/platform/
├── platform.h           # PLATFORM_WINDOWS / PLATFORM_EXPORT macros
├── shared_library.h     # LoadLibrary ←→ dlopen
├── shared_memory.h      # CreateFileMapping ←→ shm_open
├── process.h            # CreateProcess ←→ posix_spawn
├── file_system.h        # FindFirstFile ←→ opendir
└── console.cpp          # Console UTF-8 / leak detection init
```

- Compile-time dispatch: CMake `if(WIN32)` → `_win.cpp`, `else()` → `_posix.cpp`
- **Zero `#ifdef _WIN32` in framework code** — uses `PLATFORM_WINDOWS` / `PLATFORM_LINUX` / `PLATFORM_MACOS`

### v2.6 Key Fixes

| Fix | Problem | Solution |
|-----|---------|----------|
| **A1 Snapshot Emit** | Emit held shared_lock while executing Slots; slow Slots blocked RemoveSignal → cascading block | Take `shared_ptr` snapshot inside lock → release → execute outside |
| **ShellSharedState** | `/exit` detached input thread; thread accessed destroyed ShellEngine members on process exit | Elevate `running_` etc. to heap via `shared_ptr` |
| **prompt_ready Gate** | Input thread printed `>` before main loop finished processing; output interleaved | `atomic<bool>` gate; main loop signals when done |

## Module Development

### Built-in Module

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

### DLL Plugin

```cpp
// my_module.cpp
class MyModule : public ModuleBaseObject { ... };
EXPORT_MODULE(MyModule)  // auto-generates CreateModule / DestroyModule
```

```bash
# Build as .dll / .so, drop into plugins/ directory
# test_shell auto-scans and loads on startup
```

## Testing

```bash
# Full suite (excluding slow tests)
./test_runner --gtest_filter=-StressGraduated.*:Stress60s.*:PeakStressTest.*

# Concurrency stress
./test_runner --gtest_filter=ConcurrencyStress.*

# Performance
./test_runner --gtest_filter=PeakStressTest.*

# Flaky detection
./test_runner --gtest_filter=ShellEngineTest.* --gtest_repeat=5 --gtest_shuffle
```

### Test Scale

```
385 tests / 48 suites
├── ShellEngine (20)       Start/stop, concurrency, shutdown races
├── ConcurrencyStress (8)  Emit blocking, TOCTOU, ChaosMonkey
├── InputThread (13)       Producer/consumer, backpressure, data integrity
├── DLLLifecycle (10)      Load/unload, concurrent execution, OnShutdown
├── PeakStressTest (6)     Throughput, burst, sustained, recovery
├── Platform (83)          5 cross-platform test groups
├── EventBus (26)          Signal/Slot, type safety, snapshot Emit
├── Task/ThreadPool (30)   Sharding, pause/resume, pooling
└── ...
```

### Performance Baseline (i7-13700H, Debug)

| Metric | Value |
|--------|-------|
| Peak throughput | **~22,400 cmd/s** |
| Burst 500 cmds | **~2,230 cmd/s**, 99.6% completion |
| Sustained 10s | memory delta **+200 KB** |
| 30 lifecycles | memory delta **+16 KB** |
| Overload recovery | **~108 ms** |

## Directory Structure

```
test_shell/
├── main.cpp                 # Entry point: assemble → Run()
├── core/                    # Kernel
│   ├── ShellEngine.h        #   Main loop engine
│   ├── ThreadPool.h         #   Thread pool
│   ├── TasksPool.h          #   Object pool
│   ├── ResultStore.h        #   Result warehouse
│   ├── ModuleLifeManager.h  #   Module lifecycle
│   ├── Config.h             #   Configuration system
│   └── platform/            #   Cross-platform abstraction
├── eventbus/                # EventBus shared library (DLL)
│   ├── include/event_bus/
│   │   ├── event_bus.h      #   Signal/Slot dispatcher
│   │   ├── signal.h         #   Type-safe signal
│   │   └── slot.h           #   weak_ptr-protected slot
│   └── src/event_bus_dll.cpp
├── parser/                  # Command parser
├── sdk/                     # Public API
│   ├── IModule.h            #   Module base + LOG_PLAIN + REGISTER_FUNC
│   └── ParmarPack.h         #   Parameter pack
├── modules/                 # Built-in modules
├── monitor/                 # shell_monitor (FTXUI)
├── test/                    # Tests (385 cases)
└── docs/                    # Documentation (11 bilingual articles)
```

## Documentation Index

| Document | Content |
|----------|---------|
| [01-Getting Started](docs/en/01-getting-started.md) | Write your first module in 5 minutes |
| [02-ParmarPack API](docs/en/02-parmarpack-api.md) | Complete ParmarPack API reference |
| [03-Thread Safety](docs/en/03-thread-safety.md) | Lock strategy & concurrency guarantees |
| [04-DLL Modules](docs/en/04-dll-modules.md) | Plugin development & hot-loading |
| [05-Task System](docs/en/05-task-system.md) | Shard-based task pipeline |
| [06-Monitoring](docs/en/06-monitoring.md) | MetricsCollector + FTXUI |
| [07-Architecture](docs/en/07-architecture.md) | Full architecture overview |
| [08-Testing](docs/en/08-testing.md) | Test strategy & patterns |
| [10-Performance](docs/en/10-performance.md) | Performance baseline & tuning |
| [12-Concurrency Audit](docs/zh/12-并发审计.md) | Concurrency issue tracker |

## License

MIT
