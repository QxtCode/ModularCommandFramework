# DLL 模块 — 热加载插件

## 创建 DLL 模块

```cpp
// CalcModule.h — 和普通模块一样
class CalcModule : public ModuleBaseObject {
    const char* GetName() const override { return "Calculator"; }
    bool OnInit() override {
        REGISTER_FUNC("add", "a+b", { ... });
        return true;
    }
};
```

```cpp
// CalcModule.cpp — 一行
#include "CalcModule.h"
#include "core/ModuleLifeManager.h"
EXPORT_MODULE(CalcModule)
```

编译成 `.dll`，链接 `eventbus.lib`，扔进 `plugins/`。框架启动时自动扫描加载。

## 热卸载

```
UnloadModule("Calculator")
  → OnShutdown()
  → RemoveSignal("Calculator.*")   // 等所有正在跑的 Slot 结束
  → delete 模块对象
  → FreeLibrary(handle)            // 安全卸载 DLL
```

## DestroyModule 不需要

`EXPORT_MODULE` 生成了 `CreateModule` 和 `DestroyModule`。框架只用 `CreateModule`，模块由 `shared_ptr` 管理生命周期。`DestroyModule` 保留供手动管理模块的外部用户使用。
