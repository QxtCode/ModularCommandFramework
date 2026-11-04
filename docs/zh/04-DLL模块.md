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
// CalcModule.h — 和普通模块一样，只 include SDK 伞头
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
// CalcModule.cpp — 一行宏，全平台通用
#include "CalcModule.h"     // 内含 sdk/sdk.h（含 EXPORT_MODULE）
EXPORT_MODULE(CalcModule)   // ← 生成 CreateModule/DestroyModule 导出
```

`EXPORT_MODULE` 定义在 `sdk/Export.h`，自带导出宏（`TEST_SHELL_EXPORT`）——在 Windows 上展开为 `__declspec(dllexport)`，在 Linux/macOS 上展开为 `__attribute__((visibility("default")))`。零 core 依赖。

## 构建

编译为共享库，链接 `test_shell_sdk`（纯头文件，**不链 eventbus**）。将输出放入 `plugins/`。启动时框架自动扫描。

```bash
# 示例：编译计算器模块
cmake --build --preset debug --target CalculatorModule
```

## 底层原理

```cpp
// ModuleLifeManager::LoadDLLModule（简化）
auto lib = platform::SharedLibrary::Load(path);  // LoadLibrary / dlopen
auto create = lib->GetFunction<CreateFunc>("CreateModule");  // GetProcAddress / dlsym
ModuleBase* raw = create();
AddModule(unique_ptr<ModuleBase>(raw));          // 宿主通过 ForEachFunc 自动接线
dll_handles_[name] = std::move(lib);  // RAII: 析构时自动 FreeLibrary / dlclose
```

`platform::SharedLibrary` 是 RAII 封装——离开作用域时自动卸载库。无需手动调用 `FreeLibrary` 或 `dlclose`。

## 热卸载

```
UnloadModule("Calculator")
  → OnShutdown()
  → RemoveSignal("Calculator.*")   // v2.6 快照式 Emit：立即返回，不等慢回调
  → module_map_.erase()            // 释放 shared_ptr → 无在途回调时删除模块
  → dll_handles_.erase()           // SharedLibrary 析构 → FreeLibrary/dlclose（立即）
```

v2.6 起 `RemoveSignal` 不再等待在途回调（快照式 Emit 只持锁拷贝信号 `shared_ptr`，
锁外执行 Slot）。由此：

1. `Slot::Run()` 执行期间持有模块的 `shared_ptr` → **模块对象**不会在回调中途析构；
2. 但 `UnloadModule` 会立即释放 DLL 句柄，**不会等待在途回调离开动态库代码**。

所以热卸载只应在**模块空闲**时调用（没有命令在飞），或由模块在 `OnShutdown()` 里
自行等待在途回调归零（如原子计数）。不要一边有慢命令执行一边卸载插件。

## 不需要 DestroyModule

`EXPORT_MODULE` 生成 `CreateModule` 和 `DestroyModule`。框架只调用 `CreateModule`。保留 `DestroyModule` 是为了向后兼容手动管理模块生命周期的外部用户。框架使用 `shared_ptr` 的默认 deleter。
