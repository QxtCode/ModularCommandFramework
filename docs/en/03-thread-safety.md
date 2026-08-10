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

Unloading a shared library module while it's executing is safe:

1. `UnloadModule` calls `RemoveSignal` which blocks until all `Emit` calls finish
2. `Emit` holds a shared_lock during slot execution
3. `Slot::Run()` locks a shared_ptr to the module during callback — defense in depth
4. Only after all callbacks return does the library unload (via RAII `platform::SharedLibrary` destructor — `FreeLibrary` on Windows, `dlclose` on Linux/macOS)

**You don't need to worry about this.** The framework handles it.

## `LOG_PLAIN` — use it, don't use `std::cout`

```cpp
// ❌ Don't — output interleaves with other threads
std::cout << "hello" << std::endl;

// ✅ Do — each line is atomic
LOG_PLAIN("hello");
```
