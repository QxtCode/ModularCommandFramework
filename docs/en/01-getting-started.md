# Getting Started — Write Your First Module

A module is **just a C++ class** with a name and some functions.

## Cross-Platform

test_shell runs on **Windows, Linux, and macOS**. The same module code compiles everywhere:

| Platform | Plugin extension | Build command |
|----------|:-----------------:|---------------|
| Windows  | `.dll` | `cmake --preset debug` |
| Linux    | `.so`  | `cmake --preset debug` |
| macOS    | `.dylib` | `cmake --preset debug` |

All platform differences are handled by `core/platform/` — you write module logic once.

## Minimal Module (3 things)

```cpp
// MyModule.h
#pragma once
#include "sdk/sdk.h"          // one include is enough; no eventbus dependency

class MyModule : public ModuleBase {
public:
    // 1. Name — what you type after -m:
    const char* GetName() const override { return "MyModule"; }

    // 2. Register functions
    bool OnInit() override {
        REGISTER_FUNC("hello", "Say hello", {
            pack->return_value = "Hello, world!";
            pack->success = true;
        });
        return true;
    }
    // 3. Done. That's it.
};
```

### Built-in: register in `ShellApp::SetupModules()`

Built-in modules are registered in `core/ShellApp.h` (`SetupModules()`), not in `main.cpp`:

```cpp
#include "modules/MyModule.h"
// ...
mgr.AddModule(std::make_unique<MyModule>());
```

### Plugin: one macro, drop in `plugins/`

```cpp
// MyModule.cpp
#include "MyModule.h"
#include "core/ModuleLifeManager.h"
EXPORT_MODULE(MyModule)
```

Compile to shared library (`.dll` / `.so` / `.dylib`), put in `plugins/`. Framework auto-loads it on startup via `ScanPluginDirectory`. The `EXPORT_MODULE` macro lives in `sdk/Export.h` and uses `TEST_SHELL_EXPORT` — it works on all platforms with zero core dependency.

## Build

```bash
# Configure
cmake --preset debug

# Build
cmake --build --preset debug

# Run
./out/build/debug/test_shell     # Linux/macOS
out\build\debug\test_shell.exe    # Windows
```

> **Windows users**: you can also just double-click `rebuild_all.bat`.

## Run it

```
> -m:MyModule -f:hello
[OK] Hello, world!
```

## The interface is always the same

Every module, from the simplest to MetricsCollector, implements the same 3-method interface:

```cpp
class IModule {
    virtual const char* GetName() const = 0;   // required
    virtual bool        OnInit()         = 0;   // required: put REGISTER_FUNC here
    virtual void        OnShutdown()     {}     // optional: cleanup
    virtual int         GetVersion() const { return 1; }  // optional
};
```

That's the contract. The framework does everything else.
