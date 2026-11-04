# Thread Safety — What the Framework Guarantees

## Your module callbacks run on a worker thread

```cpp
REGISTER_FUNC("work", "...", {
    // This runs on a ThreadPool worker thread
    // NOT the main thread
});
```

## What's safe

- Reading/writing `pack` — each task has its own pack
- Calling `LOG_PLAIN(...)` — protected by global output mutex
- Using `std::atomic` members — standard C++ atomics work
- Calling `pack->owner_task->PushShard(...)` — protected by shards_mutex_

## What's NOT safe

- Sharing non-atomic data between REGISTER_FUNC callbacks
- Holding pointers to other modules' data across callbacks
- Blocking forever (blocks the worker thread)

## Plugin unload safety

`UnloadModule` runs `OnShutdown()` → `RemoveSignal("Module.*")` → drops the module
object → unloads the library (RAII `platform::SharedLibrary` destructor — `FreeLibrary`
on Windows, `dlclose` on Linux/macOS).

Since v2.6, `Emit` takes a snapshot (copies the signal `shared_ptr` under the lock, runs
slots outside it), so `RemoveSignal`/`UnloadModule` **do not wait for slow in-flight
callbacks**:

- The `shared_ptr` held by `Slot::Run()` keeps the **module object** alive mid-callback;
- But the DLL handle is released before `UnloadModule` returns. The framework does **not**
  keep library code mapped while an in-flight callback is still executing in it.

Rule: hot-unload only an **idle** module (no commands in flight), or make the module wait
for its own in-flight callbacks to drain in `OnShutdown()`. Do not rely on the pre-v2.6
guarantee that unload blocks until every callback returns.

## `LOG_PLAIN` — use it, don't use `std::cout`

```cpp
// ❌ Don't — output interleaves with other threads
std::cout << "hello" << std::endl;

// ✅ Do — each line is atomic
LOG_PLAIN("hello");
```
