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
// CalcModule.h — same as any module
class CalcModule : public ModuleBaseObject {
    const char* GetName() const override { return "Calculator"; }
    bool OnInit() override {
        REGISTER_FUNC("add", "a+b", { ... });
        return true;
    }
};
```

```cpp
// CalcModule.cpp — one macro, platform-agnostic
#include "CalcModule.h"
#include "core/ModuleLifeManager.h"
EXPORT_MODULE(CalcModule)   // ← generates CreateModule/DestroyModule exports
```

`EXPORT_MODULE` uses `PLATFORM_EXPORT` from the platform layer — it expands to `__declspec(dllexport)` on Windows and `__attribute__((visibility("default")))` on Linux/macOS.

## Build

Compile to shared library, link against `eventbus`. Drop the output in `plugins/`. Framework auto-scans on startup via `ScanPluginDirectory`.

```bash
# Example: build calculator module
cmake --build --preset debug --target CalculatorModule
```

## How it works under the hood

```cpp
// ModuleLifeManager::LoadDLLModule (simplified)
auto lib = platform::SharedLibrary::Load(path);  // LoadLibrary / dlopen
auto create = lib->GetFunction<CreateFunc>("CreateModule");  // GetProcAddress / dlsym
ModuleBaseObject* raw = create();
AddModule(unique_ptr<ModuleBaseObject>(raw));
dll_handles_[name] = std::move(lib);  // RAII: destructor calls FreeLibrary / dlclose
```

`platform::SharedLibrary` is an RAII wrapper — when it goes out of scope, the library is automatically unloaded. No manual `FreeLibrary` or `dlclose` needed.

## Hot-unload

```
UnloadModule("Calculator")
  → OnShutdown()
  → RemoveSignal("Calculator.*")   // blocks until all running callbacks finish
  → module_map_.erase()            // drops shared_ptr → may delete module
  → dll_handles_.erase()           // SharedLibrary destructor → FreeLibrary/dlclose
```

The framework guarantees safe unload even while callbacks are running:
1. `RemoveSignal` acquires a unique lock — blocks until all `Emit` calls finish
2. `Slot::Run()` holds a `shared_ptr` to the module during execution
3. Only after all references are released does the DLL unload

## No `DestroyModule` needed

`EXPORT_MODULE` generates `CreateModule` and `DestroyModule`. The framework only calls `CreateModule`. `DestroyModule` is kept for backward compatibility with external users who manage module lifetime manually. The framework uses `shared_ptr`'s default deleter instead.
