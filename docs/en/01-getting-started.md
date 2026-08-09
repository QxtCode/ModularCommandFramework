# Getting Started — Write Your First Module

A module is **just a C++ class** with a name and some functions.

## Minimal Module (3 things)

```cpp
// MyModule.h
#pragma once
#include "core/ModuleBaseObject.h"

class MyModule : public ModuleBaseObject {
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

### Built-in: add 2 lines to `main.cpp`

```cpp
#include "modules/MyModule.h"
// ...
mgr.AddModule(std::make_unique<MyModule>());
```

### DLL plugin: drop in `plugins/`

```cpp
// MyModule.cpp
#include "MyModule.h"
#include "core/ModuleLifeManager.h"
EXPORT_MODULE(MyModule)
```

Compile to `.dll`, put in `plugins/`. Framework auto-loads it on startup.

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
