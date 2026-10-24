# SDK 原理 — 写给刚接触的人

> 这篇不讲 API 细节，讲「为什么这么设计」。看懂为什么，比背 API 重要得多。
> 前提提醒：你学了一年 C++ 还看不懂很多代码，多半是因为那些代码讲的是「怎么实现」，而这篇讲「为什么」。

---

## 1. SDK 到底是个什么东西？

类比：你做游戏想让别人写 mod，你会给 mod 作者发一个「开发包」——
①一份**说明书（契约）**：「你的 mod 必须长这样」；②一套**工具**：继承模板填几个方法就行。
这个开发包就是 **SDK**。而游戏引擎本身怎么跑、怎么调度，mod 作者不需要也不应该知道。

**关键点：SDK 和「框架内核」是两样东西**——SDK 给**别人**用（插件作者），内核给你**自己**用。
这个项目之前的问题就是把两者搅在一起：想写个模块，还得 include 内核头文件、链内核动态库，等于「做 mod 的人必须懂引擎内部构造」。

---

## 2. 为什么「契约」和「实现」要分开？（核心思想）

- **WHAT（做什么）** — 契约，写在 SDK 里。
- **HOW（怎么做）** — 实现，写在内核里。

写一个「计算器」模块，你（模块作者）**只关心 WHAT**：

```cpp
class CalculatorModule : public ModuleBase {
    const char* GetName() const override { return "Calculator"; }  // 我叫啥

    bool OnInit() override {
        REGISTER_FUNC("add", "a+b", {              // 我有个命令叫 add
            int a = pack->GetAsOr<int>("a", 0);     // 拿参数 a
            int b = pack->GetAsOr<int>("b", 0);     // 拿参数 b
            SetResult(pack, std::to_string(a + b)); // 返回 a+b
        });
        return true;
    }
};
```

命令怎么被找到、怎么被线程池调度、结果怎么回传——这些 HOW 全由内核负责。

**为什么分开？** ① **门槛低**：一行 `#include "sdk/sdk.h"` 开工；② **不会崩**：碰不到内核的锁、线程、内存管理就碰不坏；③ **内核随便改**：只要契约不变，内核重构不影响模块。

---

## 3. SDK 里到底有什么？（`sdk/` 目录 5 个文件）

| 文件 | 是什么 | 类比 |
|------|--------|------|
| `ParmarPack.h` | 传数据的「信封」 | 快递信封，装 `mod_id`、`func_id`、参数 |
| `IModule.h` | 模块的「契约」（接口） | 说明书：必须实现 `GetName`/`OnInit` |
| `ModuleBase.h` | 契约 + 收尾助手 | 说明书 + 帮你填快递单的便利贴 |
| `Export.h` | DLL 的「出厂口」 | 让内核能 `new` 出你的模块 |
| `sdk.h` | 伞头 | 目录页，一个 include 全拉进来 |

### 3.1 ParmarPack —— 信封

模块间传数据不靠全局变量（会打架），靠一个「信封」：

```cpp
ParmarPack pack;
pack.mod_id  = "Calculator";   // 发给哪个模块
pack.func_id = "add";          // 调用哪个命令
pack.params["a"] = {"3"};
pack.params["b"] = {"4"};
// 模块执行完把结果写回信封：
pack.success = true;
pack.return_value = "7";
```

框架和模块之间只传这一个信封，谁也不用管对方内部长什么样。

### 3.2 IModule —— 契约

```cpp
class IModule {
    virtual const char* GetName() const = 0;  // 纯虚：谁继承谁必须实现
    virtual bool OnInit() { return true; }    // 可选
    virtual void Execute(ParmarPack* pack);   // 有默认实现
};
```

`= 0` 纯虚函数就是 C++ 里「定契约」的方式。

### 3.3 REGISTER_FUNC —— 登记命令

不用手写「查表、分发」，在 `OnInit` 里登记即可：

```cpp
REGISTER_FUNC("add", "a+b", { ... });
//           ↑命令名  ↑说明   ↑命令体
```

宏把命令体存进表（`funcs_`），框架按命令名查表调用。**你只声明「我有什么命令」，不管「怎么分发」。**

### 3.4 谁把命令「接上线」？—— 宿主（接线反转，本次重构核心）

模块声明了命令，还得有人把它们「接到」框架的命令系统上。两种接法：

- **旧接法（模块自己接）**：模块作者调 `ConnectToEventBus()`——逼着作者懂 EventBus（内核的东西）。
- **新接法（宿主帮模块接）**：模块通过 `ForEachFunc` 暴露命令清单，内核的 `ModuleLifeManager` **自动**把每个命令接成信号：

```cpp
// 内核里（ModuleLifeManager::AddModule 内部，模块作者看不到）：
shared_mod->ForEachFunc([&](const std::string& id, const std::string&) {
    std::string sig = name + "." + id;         // 拼成 "Calculator.add"
    bus.RegisterSignal<ParmarPack*>(sig);
    bus.LinkSlotFunc<ParmarPack*>(sig, shared_mod,
        [raw = shared_mod.get()](ParmarPack* p) { raw->Execute(p); });
});
```

**这就是「反转」**：接线从「模块的责任」变成「内核的责任」，模块作者从此对 EventBus 一无所知。

### 3.5 EXPORT_MODULE —— DLL 出厂口

```cpp
EXPORT_MODULE(CalculatorModule)
// 展开成：extern "C" ModuleBase* CreateModule() { return new CalculatorModule(); }
```

内核加载 DLL 后调 `CreateModule()` 拿到模块对象。`extern "C"` 保证函数名不被 C++ 改名，内核能按名字找到。

---

## 4. 一句话总结

> **SDK = 契约 + 便利工具，只管「模块是什么、能干什么」。**
> **内核 = 实现，管「命令怎么调度、执行、保活」。**
> **模块作者只写 WHAT，内核包办 HOW。**

写一个模块，脑子里只过三句话：① 我叫什么 → `GetName()`；② 我有什么命令 → `REGISTER_FUNC`；③ 拿参数写结果 → `pack->GetAsOr` / `SetResult`。其余一律不管。

---

## 5. 如果还想深挖

- 命令怎么被调度执行 → [07-架构概览](07-架构概览.md) 的「一条命令的完整旅程」。
- 「接口」「虚函数」这些 C++ 概念是一切的地基。
- 想试写模块 → 照 `modules/samples/CalculatorModule.h` 抄一遍改个名字，编译跑起来，比读十遍文档都管用。
