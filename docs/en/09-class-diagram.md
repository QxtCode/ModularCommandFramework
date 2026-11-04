# Class Diagram — test_shell v2.8

> Read from **top to bottom** — higher layers depend on lower ones.

```
                        ┌─────────────────────┐
                        │      main.cpp       │
                        │ (进程初始化 + 启动)   │
                        └──────────┬──────────┘
                                   │ 创建并使用
                        ┌──────────▼──────────┐
                        │      ShellApp       │  ★ UI integration
                        │  ─────────────────  │
                        │ + GetEngine()       │
                        │ + GetConfig()       │
                        │ + Run()             │
                        │ - SetupModules()    │
                        │ - store_, engine_   │
                        └──────────┬──────────┘
                                   │ 持有并组装
                        ┌──────────▼──────────┐
                        │    ShellEngine      │  ★ v2.5 新增
                        │  ─────────────────  │
                        │ + Run()             │
                        │ + RunWithoutInput() │  ★ UI integration
                        │ + SetResultSink()   │  ★ UI integration
                        │ + Shutdown()        │
                        │ + InjectCommand() ◄─│── test hook
                        │ + RequestStop()  ◄─│── test hook
                        │ - DrainResults()    │
                        │ - FlushMetrics()    │
                        │ - WaitForWork()     │
                        │ - ProcessInput()    │
                        └──┬──────┬──────┬───┘
                           │owns  │owns  │uses (singleton)
              ┌────────────┘      │      └──────────────┐
              ▼                   ▼                     ▼
   ┌─────────────────┐  ┌──────────────┐  ┌────────────────────┐
   │   ThreadPool    │  │  TasksPool   │  │   ResultStore      │ ★ v2.5
   │  ─────────────  │  │ ──────────── │  │   (Singleton)      │
   │ + Enqueue(job)  │  │ + Acquire()  │  │  ────────────────  │
   │ + Submit(fn)    │  │ + Release()  │  │  + PushResult()    │
   │ - workers_: [T] │  │ - tasks_: [T]│  │  + Drain()         │
   └────────┬────────┘  └──────┬───────┘  │  + HasResults()    │
            │                  │           │  - store_: [Item]  │
            │ 执行任务          │ 分配/归还  └─────────┬──────────┘
            ▼                  ▼                      │ 存储
   ┌─────────────────────────────────────┐            │
   │              Task                   │            │
   │  ───────────────────────────────    │            │
   │  + Step(EventBus&) : bool           │            │
   │  + PushShard(unique_ptr<Pack>)      │            │
   │  + Pause() / Resume() / Cancel()    │            │
   │  + CurrentPack() : ParmarPack*      │            │
   │  - shards_: [unique_ptr<Pack>]      │            │
   │  - state_: atomic<State>            │            │
   └──────────────┬──────────────────────┘            │
                  │ Step() 调用                       │
                  ▼                                   │
   ┌─────────────────────────────────────┐            │
   │           EventBus                  │            │
   │         (Singleton, DLL)            │            │
   │  ───────────────────────────────    │            │
   │  + RegisterSignal<Args>(name)       │            │
   │  + LinkSlotFunc<Args>(sig, fn)      │            │
   │  + Emit<Args>(sig, args...) : bool  │            │
   │  + RemoveSignal(name)               │            │
   │  - signals_: map<string, ISignal>   │            │
   └──────┬──────────────────┬───────────┘            │
          │ 包含              │ 包含                    │
          ▼                   ▼                        │
   ┌─────────────┐   ┌──────────────┐                 │
   │Signal<Args> │   │ Slot<Args>   │                 │
   │ : ISignal   │   │ : ISlot      │                 │
   │─────────────│   │──────────────│                 │
   │+Traverse()  │   │+ Run(args)   │                 │
   │+ConnectSlot │   │+ IsAlive()   │                 │
   │- slots_:map │   │- func_       │                 │
   └─────────────┘   │- weak_holder_│                 │
                     └──────┬───────┘                 │
                            │ 指向                     │
                            ▼                          │
   ┌─────────────────────────────────────┐            │
   │        ModuleLifeManager            │            │
   │          (Singleton)                │            │
   │  ───────────────────────────────    │            │
   │  + AddModule(unique_ptr<Module>)    │            │
   │  + UnloadModule(name)               │            │
   │  + LoadDLLModule(path)              │            │
   │  + ScanPluginDirectory(dir)         │            │
   │  + GetModule(name) : ModuleBase*    │            │
   │  - module_map_: map<string, shr_ptr>│            │
   │  - dll_handles_: map<string,        │            │
   │           unique_ptr<SharedLibrary>>│            │
   └──────────────┬──────────────────────┘            │
                  │ 管理                                │
                  ▼                                    │
   ┌─────────────────────────────────────┐            │
   │             ModuleBase              │            │
   │           : IModule                 │            │
   │  ───────────────────────────────    │            │
   │  + ForEachFunc(fn)                  │            │
   │    (host-side wiring, see SDK doc)  │            │
   └─────────────────────────────────────┘            │
                  △ 继承                               │
                  │                                    │
   ┌──────────────┴──────────────────────┐            │
   │             IModule                 │◄── SDK 公共 │
   │  ───────────────────────────────    │    接口     │
   │  + GetName() : const char*          │            │
   │  + OnInit() : bool                  │            │
   │  + Execute(pack)                    │            │
   │  + Help(pack)                       │            │
   │  # funcs_: map<string, Func>        │            │
   │  # RegisterFunc(id, desc, fn)       │            │
   └──────────────┬──────────────────────┘            │
                  │ 函数接收                             │
                  ▼                                    │
   ┌─────────────────────────────────────┐◄────────────┘
   │           ParmarPack                │  ResultItem
   │  ───────────────────────────────    │  包含
   │  + Get(key) : optional<string>      │
   │  + GetOr(key, def) : string         │
   │  + GetAsOr<T>(key, def) : T         │
   │  + Set(key, val)                    │
   │  + Has(key) : bool                  │
   │  + mod_id, func_id                  │
   │  + params: map<string, vec<string>> │
   │  + success, return_value, error     │
   │  + owner_task : Task*               │
   └─────────────────────────────────────┘
```

> `+` public &nbsp; `-` private &nbsp; `◄── test hook` for test injection only (not used in production)
>
> `ModuleBaseObject` (`core/ModuleBaseObject.h`) is a compatibility subclass of `ModuleBase`
> that adds the `ConnectToEventBus` test backdoor. It is kept for legacy code and tests —
> it is **not** the base class module authors should inherit.

## Key relationships

```
  实线 →  = 拥有/组合 (unique_ptr, 成员变量)
  虚线 →  = 使用/依赖 (单例, 引用)
  △       = 继承
```

| From | To | Kind |
|------|----|------|
| `ShellApp` | `ShellEngine` | owns (unique_ptr) |
| `ShellApp` | `TaskFilePersistence` | owns (shared_ptr, default backend) |
| `ShellEngine` | `ThreadPool` | owns |
| `ShellEngine` | `TasksPool` | owns |
| `ShellEngine` | `ResultStore` | uses (singleton) |
| `TasksPool` | `Task` | owns (pool of) |
| `Task` | `ParmarPack` | owns (shards) |
| `Task::Step()` | `EventBus` | uses (singleton) |
| `EventBus` | `Signal<Args>` | owns |
| `Signal<Args>` | `Slot<Args>` | owns |
| `Slot<Args>` | `WeakRefHolder` | owns |
| `Slot<Args>` | `IModule` | points to (via weak_ptr) |
| `ModuleLifeManager` | `ModuleBase` | owns (shared_ptr map) |
| `ModuleLifeManager` | `EventBus` | uses (singleton) |
| `ModuleLifeManager` | `ModuleBase::ForEachFunc` | uses (host-side wiring) |
| `ModuleBase` | `IModule` | inherits (SDK layer) |
| `ModuleBaseObject` | `ModuleBase` | inherits (core compat layer) |
| `ModuleBaseObject` | `EventBus` | uses (ConnectToEventBus, test backdoor) |
| `MetricsCollector` | `ModuleBase` | inherits |
| `IModule::Execute()` | `ParmarPack` | receives pointer |
| `ShellEngine::ProcessInput()` | `CommandParser` | uses (singleton) |
| `Worker lambda` | `ResultStore` | uses (singleton, PushResult) |

## What happens to one command

```
Input: "-m:Calc -f:add -v:a|1,b|2"
  │
  │  ShellEngine::ProcessInput()
  ├─→ CommandParser.SendCommand("TXT", line)
  │     → Parses into ParmarPack {mod_id="Calc", func_id="add", params={a:[1],b:[2]}}
  │
  ├─→ TasksPool.Acquire(pack)
  │     → Task with shards_[0] = pack
  │
  ├─→ ThreadPool.Enqueue(lambda)
  │     │
  │     │  [Worker thread]
  │     ├─→ Task::Step(EventBus)
  │     │     → bus.Emit("Calc.add", pack)
  │     │       → Signal::TraverseSlots
  │     │         → Slot::Run
  │     │           → WeakRefHolder::Lock() ← v2.4 安全检查
  │     │           → ModuleBase::Execute(pack)
  │     │             → funcs_["add"](pack)  ← 模块的回调
  │     │               → pack->return_value = "3"
  │     │               → pack->success = true
  │     │
  │     ├─→ bus.Emit("task.result", id, ok, code, msg, retval)  ← 实时订阅者
  │     │
  │     ├─→ ResultStore::PushResult(id, pack_copy)   ← 落入仓库
  │     │
  │     └─→ tasks.Release(task)  ← 槽位立即归还池子
  │
  │  [Main loop, next iteration]
  ├─→ DrainResults()
  │     → ResultStore::Drain() → batch of ResultItem
  │     → ConsoleFormatter::Format(pack) → "[OK] 3"
  │     → LOG_PLAIN("[OK] 3")
  │
  └─→ WaitForWork()
        → cv.wait_for(HasResults || input || !running)
```
