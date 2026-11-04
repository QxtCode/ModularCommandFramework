# Shared Library Modules — Cross-Platform Plugins

test_shell supports hot-loading modules from shared libraries on **Windows, Linux, and macOS**.

| Platform | Extension | Dynamic library API |
|----------|:---------:|---------------------|
| Windows  | `.dll` | `LoadLibrary` / `FreeLibrary` |
| Linux    | `.so`  | `dlopen` / `dlclose` |
| macOS    | `.dylib` | `dlopen` / `dlclose` |

All platform differences are abstracted by `core/platform/shared_library.h` — you don't need `#ifdef` in your code.

## Create a shared library module

```cpp
// CalcModule.h — same as any module, only include the SDK umbrella header
#include "sdk/sdk.h"
class CalcModule : public ModuleBase {
    const char* GetName() const override { return "Calculator"; }
    bool OnInit() override {
        REGISTER_FUNC("add", "a+b", { ... });
        return true;
    }
};
```

```cpp
// CalcModule.cpp — one macro, platform-agnostic
#include "CalcModule.h"     // includes sdk/sdk.h (and EXPORT_MODULE)
EXPORT_MODULE(CalcModule)   // ← generates CreateModule/DestroyModule exports
```

`EXPORT_MODULE` lives in `sdk/Export.h` and uses `TEST_SHELL_EXPORT` — it expands to `__declspec(dllexport)` on Windows and `__attribute__((visibility("default")))` on Linux/macOS. Zero core dependency.

## Build

Compile to a shared library, linking `test_shell_sdk` (headers only, **no eventbus dependency**). Drop the output in `plugins/`. Framework auto-scans on startup via `ScanPluginDirectory`.

```bash
# Example: build calculator module
cmake --build --preset debug --target CalculatorModule
```

## How it works under the hood

```cpp
// ModuleLifeManager::LoadDLLModule (simplified)
auto lib = platform::SharedLibrary::Load(path);  // LoadLibrary / dlopen
auto create = lib->GetFunction<CreateFunc>("CreateModule");  // GetProcAddress / dlsym
ModuleBase* raw = create();
AddModule(std::unique_ptr<ModuleBase>(raw));
dll_handles_[name] = std::move(lib);  // RAII: destructor calls FreeLibrary / dlclose
```

`platform::SharedLibrary` is an RAII wrapper — when it goes out of scope, the library is automatically unloaded. No manual `FreeLibrary` or `dlclose` needed.

## Hot-unload

```
UnloadModule("Calculator")
  → OnShutdown()
  → RemoveSignal("Calculator.*")   // v2.6 snapshot Emit: returns immediately, does NOT wait
  → module_map_.erase()            // drops shared_ptr → may delete module if no callback is running
  → dll_handles_.erase()           // SharedLibrary destructor → FreeLibrary/dlclose (immediately)
```

Since v2.6 (snapshot Emit), `RemoveSignal` no longer waits for in-flight callbacks:

1. `Slot::Run()` holds a `shared_ptr` to the module while executing, so the **module object**
   cannot be destroyed mid-callback.
2. But `UnloadModule` releases the DLL handle immediately — it does **not** wait for
   in-flight slot code to leave the library.

Therefore only hot-unload an **idle** module (no commands in flight), or have the module
wait for its own in-flight callbacks to drain in `OnShutdown()` (e.g. an atomic counter).
Do not rely on the pre-v2.6 behavior where unload blocked until all callbacks returned.

## No `DestroyModule` needed

`EXPORT_MODULE` generates `CreateModule` and `DestroyModule`. The framework only calls `CreateModule`. `DestroyModule` is kept for backward compatibility with external users who manage module lifetime manually. The framework uses `shared_ptr`'s default deleter instead.
