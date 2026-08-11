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

## ITaskStore — Task State Persistence (v2.6 design, pending integration)

Currently all Task state lives in memory. Process exit → PAUSED tasks and queued commands are lost.
ITaskStore is a pluggable persistence layer:

```
ITaskPersistence          ITaskScheduler
  Save(task_id, snapshot)  Schedule(task_id, time)
  Load(task_id) → snapshot PollDue(now) → due list
  Delete(task_id)          NextWakeup(now) → cv timeout
  LoadAll() → restore list

NullPersistence           NullScheduler         ← default, zero overhead
FilePersistence           TimerWheel             ← lightweight (planned)
SqlitePersistence         SqliteScheduler        ← advanced (planned)
```

### Usage (planned)

```cpp
// main.cpp — restore pending tasks on startup
engine.SetTaskPersistence(new FilePersistence("tasks/"));
auto pending = taskStore->LoadAll();  // PAUSED/FAILED tasks from last exit
for (auto& rec : pending)
    engine.RestoreTask(rec);          // re-insert into pool

// Delayed tasks
engine.SetTaskScheduler(new TimerWheelScheduler());
engine.ScheduleTask(task_id, now + 3600s);  // execute in 1 hour
```

### Current Status

Interfaces and Null implementations are ready (`core/ITaskPersistence.h` / `core/NullTaskStore.h`).
Not yet integrated into ShellEngine — all behavior matches previous versions: tasks stay in memory, lost on exit.
FileStore implementation and ShellEngine integration will follow in a separate PR.

## State machine

```
IDLE → RUNNING → COMPLETED
              → PAUSED (→ Resume → RUNNING)
              → FAILED (cancel)
```
