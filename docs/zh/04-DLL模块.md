# 共享库模块 — 跨平台插件

test_shell 支持在 **Windows、Linux、macOS** 上热加载共享库模块。

| 平台 | 扩展名 | 动态库 API |
|------|:------:|-----------|
| Windows  | `.dll` | `LoadLibrary` / `FreeLibrary` |
| Linux    | `.so`  | `dlopen` / `dlclose` |
| macOS    | `.dylib` | `dlopen` / `dlclose` |

所有平台差异由 `core/platform/shared_library.h` 抽象——你的代码不需要 `#ifdef`。

## 创建共享库模块

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
// CalcModule.cpp — 一行宏，全平台通用
#include "CalcModule.h"
#include "core/ModuleLifeManager.h"
EXPORT_MODULE(CalcModule)   // ← 生成 CreateModule/DestroyModule 导出
```

`EXPORT_MODULE` 使用平台层的 `PLATFORM_EXPORT`——在 Windows 上展开为 `__declspec(dllexport)`，在 Linux/macOS 上展开为 `__attribute__((visibility("default")))`。

## 构建

编译为共享库，链接 `eventbus`。将输出放入 `plugins/`。启动时框架自动扫描。

```bash
# 示例：编译计算器模块
cmake --build --preset debug --target CalculatorModule
```

## 底层原理

```cpp
// ModuleLifeManager::LoadDLLModule（简化）
auto lib = platform::SharedLibrary::Load(path);  // LoadLibrary / dlopen
auto create = lib->GetFunction<CreateFunc>("CreateModule");  // GetProcAddress / dlsym
ModuleBaseObject* raw = create();
AddModule(unique_ptr<ModuleBaseObject>(raw));
dll_handles_[name] = std::move(lib);  // RAII: 析构时自动 FreeLibrary / dlclose
```

`platform::SharedLibrary` 是 RAII 封装——离开作用域时自动卸载库。无需手动调用 `FreeLibrary` 或 `dlclose`。

## 热卸载

```
UnloadModule("Calculator")
  → OnShutdown()
  → RemoveSignal("Calculator.*")   // 阻塞直到所有正在执行的回调完成
  → module_map_.erase()            // 释放 shared_ptr → 可能删除模块
  → dll_handles_.erase()           // SharedLibrary 析构 → FreeLibrary/dlclose
```

框架保证即使回调正在执行也能安全卸载：
1. `RemoveSignal` 获取独占锁——阻塞直到所有 `Emit` 调用完成
2. `Slot::Run()` 在执行期间持有模块的 `shared_ptr`
3. 所有引用释放后，动态库才被卸载

## 不需要 DestroyModule

`EXPORT_MODULE` 生成 `CreateModule` 和 `DestroyModule`。框架只调用 `CreateModule`。保留 `DestroyModule` 是为了向后兼容手动管理模块生命周期的外部用户。框架使用 `shared_ptr` 的默认 deleter。
