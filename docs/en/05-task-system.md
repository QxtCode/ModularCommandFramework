# Task System — Shards, Pause, Cancel

A Task is a sequence of "shards." Each shard = one EventBus emit. Between shards you can pause, resume, or cancel.

## Data flow (v2.5)

```
Worker: while(task->Step(bus)) {}
  → bus.Emit("Calc.add", pack)     // module callback runs
  → ResultStore::PushResult(...)   // result lands in warehouse
  → tasks.Release(task)            // slot immediately reusable
  → Main loop: DrainResults()      // batch-print at consumer's pace
```

No more `done_queue`. Results go through `ResultStore` — workers push, consumers drain.

## Normal command (1 shard)

```
-m:Calc -f:add -v:a|1,b|2
→ Task shards_: [ {mod_id="Calc", func_id="add", params={a:1,b:2}} ]
→ Step() emits "Calc.add" → module callback runs → done
```

## Multi-shard workflow (dynamic append)

```cpp
REGISTER_FUNC("render", "frame by frame", {
    RenderFrame(frame_);
    frame_++;

    if (frame_ < total_) {
        auto next = std::make_unique<ParmarPack>();
        next->mod_id  = GetName();
        next->func_id = "render";
        pack->owner_task->PushShard(std::move(next));  // ← append next frame
    }

    pack->success = true;
});
```

Each `Step()` executes one shard. The callback can push more shards — they'll execute in sequence.

## Pause / Resume

```
task->Pause();        // Won't execute next shard
task->Resume();       // Continue from where it stopped
task->GetProgress();  // 0.0 ~ 1.0
```

Pause only takes effect **between shards**. A running shard always completes.

## Cancel

```
task->Cancel();       // State → FAILED, stops further shards
```

Same granularity: current shard completes, next shard skipped.

## Dead task? Can't kill mid-shard

C++ has no safe thread termination. If a shard enters an infinite loop, `Cancel()` won't stop it — it only prevents the NEXT shard. Solution: keep shards small (< 100ms).

## ITaskPersistence — Task State Persistence (v2.7, pause-save integrated)

By default all Task state lives in memory. Process exit → PAUSED tasks are lost.
`ITaskPersistence` is a pluggable layer that snapshots paused tasks so `resume` can
reconstruct them from disk.

### The pause → save → resume loop

```
pause command → task->Pause()                    // set flag
  → Worker finishes current shard → Step() returns false
  → Worker detects PAUSED → store->Save(task->ExportRecord())
  → Release(task)                                // slot returns to pool

resume command → store->Load(id) → pool.Acquire → task->Restore(rec)
  → Resume → Enqueue → continue from checkpoint
```

`Task::ExportRecord()` serializes the current shards into `TaskRecord.shards_json`
(a JSON array); `Task::Restore()` deserializes them back into real shards
(mod_id / func_id / params are fully preserved).

### Wiring it in

```cpp
// main.cpp — declare the store BEFORE the engine (destruction order)
auto taskStore = std::make_shared<MemPersistence>();
ShellEngine engine(cfg.pool_size, cfg.workers);
engine.SetTaskPersistence(taskStore.get());       // engine saves on pause
mgr.AddModule(std::make_unique<TaskManagerModule>(
    engine.GetPool(), engine.GetWorkers(), taskStore.get()));
```

`SetTaskPersistence(nullptr)` (default) = pure in-memory, paused tasks are dropped —
identical to v2.5 behavior.

### How to implement an ITaskPersistence backend

Developers can plug in their own backend (FilePersistence, SqlitePersistence, …) by
implementing 5 virtual methods:

| Method | Responsibility |
|--------|----------------|
| `Save(const TaskRecord&)` | Save/update a snapshot. **Idempotent** — same task_id may be overwritten. |
| `Load(id) → optional<TaskRecord>` | Read a snapshot; return nullopt if absent. |
| `Delete(id)` | Remove a snapshot (called by the resume flow after a task finishes). |
| `LoadAll() → vector<TaskRecord>` | Return all **non-terminal** records (startup restore). |
| `GC()` | Drop terminal (COMPLETED/FAILED) records. |
| `IsAvailable()` | Whether the backend works; if false the framework degrades to in-memory. |

Three hard requirements:

1. **Thread-safe** — Workers and the main thread may call concurrently; lock internally
   (see `MemPersistence`'s mutex).
2. **Exception-safe** — Save/Load/Delete must fail silently, never throw (a throw in the
   worker's finish path would crash the shell).
3. **Lifetime** — the backend must outlive `ShellEngine` (declare it before the engine).

Reference implementations:
- `NullPersistence` — default, zero-overhead no-op.
- `MemPersistence` — in-process map, lost on restart.
- `FilePersistence` / `SqlitePersistence` — planned; implement to the rules above.

### Delayed scheduling (ITaskScheduler, planned)

`ITaskScheduler` (`core/ITaskScheduler.h`) is the companion interface for time-based
tasks — `Schedule / Cancel / PollDue / NextWakeup`. It is **not yet integrated**;
`NullScheduler` keeps current behavior unchanged.

## State machine

```
IDLE → RUNNING → COMPLETED
              → PAUSED (→ Resume → RUNNING)
              → FAILED (cancel)
```
