# v2.5 Architecture — ShellEngine + ResultStore

## What changed from v2.4

v2.4 used a `std::queue<Task*>` (done_queue) to ferry results from workers to the main thread. Every result waited for the main thread to drain the queue before the task slot was freed.

v2.5 introduces two new components that decouple production from consumption:

| Component | Role |
|-----------|------|
| **ResultStore** | Singleton data warehouse. Workers push results; consumers drain at their own pace. |
| **ShellEngine** | Encapsulates the event loop: `DrainResults → FlushMetrics → WaitForWork → ProcessInput`. |

## Data flow

```
Input "-m:Calc -f:add -v:a|1,b|2"
  │
  ├─ InputThread (getline) ──→ LockQueue<string> ──→ ShellEngine::ProcessInput
  │                                                       │
  │                                              CommandParser::SendCommand
  │                                                       │
  │                                              TasksPool::Acquire →
  │                                              ThreadPool::Enqueue
  │                                                       │
  │                                              Worker thread:
  │                                                while(task->Step(bus)) {}
  │                                                │
  │                                   ┌────────────┴────────────┐
  │                                   │                         │
  │                            bus.Emit("task.result")   ResultStore::PushResult
  │                            (real-time subscribers)   (batch-consumable data)
  │                                                             │
  │                                              ShellEngine::DrainResults
  │                                                → Formatter → LOG_PLAIN
  │
  └─ ShellEngine::WaitForWork
       cv.wait_for(HasResults || input || !running)
```

## Three independent pathways

```
Worker completes
  ├─ bus.Emit("task.result"...)  → EventBus subscribers (real-time, zero latency)
  ├─ PushResult                  → ResultStore (warehouse, batch-consumable)
  └─ tasks.Release(task)         → Pool slot immediately reusable
```

These three operate at different layers and don't conflict:
- Subscribe to EventBus for push notifications (metrics, alerts, chaining)
- Drain ResultStore for structured batch processing (CLI output, UI binding, test assertions)
- No more main-thread bottleneck — task slots are freed the moment a worker finishes

## ShellEngine — the four-step pipeline

```cpp
void ShellEngine::MainLoop() {
    while (running_) {
        DrainResults();   // ① ResultStore batch → format → print
        FlushMetrics();   // ② MetricsCollector → shared memory
        WaitForWork();    // ③ cv.wait_for(HasResults || input || !running)
        ProcessInput();   // ④ input → parse → submit task
    }
}
```

Each step has one clear responsibility. No more reasoning about locks, queues, and cv predicates in the same function.

## ResultStore — the data warehouse

```cpp
// Producer (worker thread)
ResultStore::Get().PushResult(task_id, pack_copy);

// Consumer (main / UI thread)
auto batch = ResultStore::Get().Drain();   // O(1) swap
for (auto& item : batch) { ... }

// Predicate-safe query (no side effects)
bool has = ResultStore::Get().HasResults();
```

Key design: `HasResults()` is a read-only query. It can be called inside `cv.wait_for` predicates without the side-effect bug that would occur if `Drain()` were used instead.

## Configuration

`ShellConfig` loads from an optional INI file and CLI overrides before `ShellEngine` starts:

```cpp
ShellConfig cfg;
cfg.LoadFromFile("test_shell.cfg");   // optional — defaults if missing
cfg.ApplyArgs(argc, argv);            // --pool_size=16 --workers=8

ShellEngine engine(cfg.pool_size, cfg.workers);
```

Priority: `CLI args > config file > built-in defaults`.

The `log_level` field is applied to `LogModule` after module registration, overriding `log.conf`.

See [10-performance](10-performance.md) for the full parameter reference.

## Safety — Slot exception catch-all (v2.5)

`Slot::Run()` wraps the module callback in `try-catch(...)` as the last line of defense:

```cpp
try {
    func_(args...);           // module callback
    return true;
} catch (const std::exception& e) {
    std::cerr << "[Slot::Run] Exception: " << e.what() << std::endl;
    MarkDead();               // won't be called again
    return false;
} catch (...) {
    std::cerr << "[Slot::Run] Unknown exception" << std::endl;
    MarkDead();
    return false;
}
```

A misbehaving third-party module callback won't crash the entire process.
The slot is marked dead after an exception and excluded from future snapshots.

## main.cpp — assembly only

```cpp
int main() {
    平台初始化
    注册模块 (PrintModule, LogModule, MetricsCollector)
    注册框架信号
    扫描插件
    ShellEngine engine(8, 4);
    engine.Run();
    engine.Shutdown();
}
```

main.cpp went from 262 lines to 74. The loop logic, done_queue, input thread, and cv sync live in ShellEngine.
