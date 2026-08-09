# DLL Modules — Hot-Load Plugins

## Create a DLL module

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
// CalcModule.cpp — one line
#include "CalcModule.h"
#include "core/ModuleLifeManager.h"
EXPORT_MODULE(CalcModule)   // ← generates CreateModule/DestroyModule exports
```

## Build

Compile to `.dll`, link against `eventbus.lib`. Drop the `.dll` in `plugins/`. Framework auto-scans on startup.

## Hot-unload

```
UnloadModule("Calculator")
  → OnShutdown()
  → RemoveSignal("Calculator.*")   // blocks until all running callbacks finish
  → delete module object
  → FreeLibrary(handle)            // DLL unmapped safely
```

## No `DestroyModule` needed

`EXPORT_MODULE` generates `CreateModule` and `DestroyModule`. The framework only calls `CreateModule`. `DestroyModule` is kept for backward compatibility with external users who manage module lifetime manually. The framework uses `shared_ptr`'s default deleter instead.
