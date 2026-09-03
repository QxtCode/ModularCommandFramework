# SDK 原理 — 写给刚接触的人

> 这篇不讲 API 细节，讲「为什么」。看懂为什么，比背 API 重要得多。
> 你学了一年 C++ 还看不懂很多代码，不是因为你笨——是因为大部分代码讲的是
> 「怎么实现」，而这篇讲的是「为什么这么设计」。两者合起来才是一个完整的图。

---

## 1. SDK 到底是个什么东西？

先用一个生活里的类比。

你想做一个游戏，想让大家给你的游戏写 mod（插件）。你会怎么做？

你会给 mod 作者发一个「**开发包**」，里面有两样东西：

1. **一份说明书（契约）**：告诉作者「你的 mod 必须长这样」——必须有个名字、有个 `启动()` 方法、能处理某个动作。
2. **一套工具**：让他不用从零开始，直接继承你给好的模板，填几个方法就行。

这个「开发包」就是 **SDK**（Software Development Kit，软件开发工具包）。

而你的「游戏引擎」本身（怎么跑、怎么调度、怎么多线程）是**另外一回事**，mod 作者根本不需要知道，也不应该需要知道。

**关键点：SDK 和「框架内核」是两样东西。**
- SDK = 给**别人**用的（mod 作者、第三方插件作者）。
- 内核 = 给你**自己**用的（引擎怎么运转）。

之前这个项目的问题，就是把这两样东西**搅在一起了**：想写个模块，还得去 include 内核的头文件、链接内核的动态库。这等于「做 mod 的人还得懂游戏引擎内部构造」。

---

## 2. 为什么「契约」和「实现」要分开？

这是整个 SDK 思想的核心。用两个词概括：

- **WHAT（做什么）** — 契约，写在 SDK 里。
- **HOW（怎么做）** — 实现，写在内核里。

举个例子。你要写一个「计算器」模块，能算加法。

**你（模块作者）只需要关心 WHAT：**
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

**你完全不需要关心 HOW：**
- 命令 `add` 是怎么被找到的？
- 怎么被一个线程池调度执行的？
- 结果怎么回传的？

这些「HOW」全由**内核**负责。模块作者写完上面的代码，剩下的事框架包了。

**为什么要分开？** 三个原因：
1. **门槛低**：mod 作者不用懂内核，一行 `#include "sdk/sdk.h"` 就能开工。
2. **不会崩**：mod 作者碰不到内核的锁、线程、内存管理，就碰不坏它们。
3. **内核可以随便改**：只要契约（`GetName`/`OnInit`/`Execute`）不变，内核内部怎么重构，模块都不受影响。

---

## 3. 这个项目的 SDK 里到底有什么？

打开 `sdk/` 目录，就 5 个文件，每个干一件事：

| 文件 | 是什么 | 类比 |
|------|--------|------|
| `ParmarPack.h` | 传数据的「信封」 | 快递信封，里面装 `mod_id`、`func_id`、参数 |
| `IModule.h` | 模块的「契约」（接口） | 说明书：你必须实现 `GetName`/`OnInit` |
| `ModuleBase.h` | 契约 + 收尾助手 | 说明书 + 帮你填好快递单的便利贴 |
| `Export.h` | DLL 的「出厂口」 | 让内核能 `new` 出你的模块 |
| `sdk.h` | 伞头（一个 include 全拉进来） | 目录页 |

### 3.1 ParmarPack —— 信封

模块之间传数据，不能靠全局变量（会打架），得靠一个「信封」。

```cpp
ParmarPack pack;
pack.mod_id  = "Calculator";   // 发给哪个模块
pack.func_id = "add";          // 调用哪个命令
pack.params["a"] = {"3"};      // 参数 a=3
pack.params["b"] = {"4"};      // 参数 b=4

// 模块执行完后，把结果写回信封：
pack.success = true;
pack.return_value = "7";
```

整个流程里，框架和模块之间只传这一个信封，谁也不用管对方内部长什么样。

### 3.2 IModule —— 契约（接口）

这就是「说明书」。它规定了「一个模块至少要有这些方法」：

```cpp
class IModule {
    virtual const char* GetName() const = 0;  // 你叫什么名字（必须实现）
    virtual bool OnInit() { return true; }    // 启动时初始化（可选）
    virtual void Execute(ParmarPack* pack);   // 执行一个命令（有默认实现）
    // ...
};
```

`= 0` 是「纯虚函数」，意思是「谁继承我，谁必须实现这个」。这就是 C++ 里「定契约」的方式。

### 3.3 注册命令 —— REGISTER_FUNC

模块作者不需要手写「查表、分发」，只要在 `OnInit` 里「登记」自己的命令：

```cpp
REGISTER_FUNC("add", "a+b", { ... });
//           ↑命令名  ↑说明   ↑命令体
```

这个宏会把你写的命令体存进一张表（`funcs_`）。框架执行时，按命令名查表、调用。**你只需要声明「我有什么命令」，不用管「怎么分发」。**

### 3.4 谁把命令「接上线」？—— 宿主（内核）

这是整个设计最巧妙的一点，也是这次重构的核心。

想象一下：模块声明了「我有 add、mul 两个命令」。但这些命令怎么被外界调用？总得有人把它们「接到」框架的命令系统上。

**两种接法：**

- **旧接法（模块自己接）**：模块作者得调用 `ConnectToEventBus()`，把自己接上。这逼着模块作者**必须懂 EventBus**（内核的东西）。

- **新接法（宿主帮模块接）**：模块只声明「我有这些命令」（通过 `ForEachFunc` 暴露），内核的 `ModuleLifeManager` 拿到模块后，**自动**帮它把每个命令接成信号。

```cpp
// 内核里（ModuleLifeManager::AddModule 内部，模块作者看不到）：
shared_mod->ForEachFunc([&](const std::string& id, const std::string&) {
    std::string sig = name + "." + id;         // 拼成 "Calculator.add"
    bus.RegisterSignal<ParmarPack*>(sig);       // 注册一个信号
    bus.LinkSlotFunc<ParmarPack*>(sig, shared_mod,
        [raw = shared_mod.get()](ParmarPack* p) { raw->Execute(p); });
    // 信号触发时 → 调用模块的 Execute
});
```

**这就是「反转」**：接线这件事，从「模块的责任」变成了「内核的责任」。模块作者从此对 EventBus 一无所知。

> 这就像：你去餐厅，以前得自己进后厨找厨师（得懂厨房），现在你只要在前台说「我要点 add 这道菜」，服务员（内核）帮你下单、端菜。

### 3.5 EXPORT_MODULE —— DLL 的出厂口

一个模块如果编译成 DLL（动态库），内核要能「找到并创建」它，就需要一个固定名字的出口函数：

```cpp
EXPORT_MODULE(CalculatorModule)
// 展开成：
//   extern "C" ModuleBase* CreateModule() { return new CalculatorModule(); }
```

内核加载 DLL 后，调用 `CreateModule()`，就能拿到一个模块对象。`extern "C"` 保证函数名不被 C++ 改名，内核能按名字找到它。

---

## 4. 一句话总结

> **SDK = 契约 + 便利工具，只管「模块是什么、能干什么」。**
> **内核 = 实现，管「命令怎么调度、怎么执行、怎么保活」。**
> **模块作者只写「WHAT」，内核包办「HOW」。**

你写一个模块，脑子里只需要过三句话：
1. 我叫什么名字 → `GetName()`
2. 我有什么命令 → `REGISTER_FUNC` 登记
3. 每个命令拿参数、写结果 → `pack->GetAsOr` / `SetResult`

其余的一律不管。

---

## 5. 如果还想深挖

- 想知道命令到底怎么被调度执行的，看 `docs/zh/07-架构概览.md` 的「一条命令的完整旅程」。
- 想理解「接口」「虚函数」这些 C++ 概念，它们是这一切的地基。
- 想自己试写一个模块，照着 `modules/samples/CalculatorModule.h` 抄一遍，改个名字和命令，编译跑起来，比读十遍文档都管用。
