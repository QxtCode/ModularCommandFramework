#pragma once
/*
==========模块 DLL 导出机制==========

  每个模块 DLL 只需在 .cpp 末尾写：
    EXPORT_MODULE(MyModule)

  主程序加载 DLL 时会调用 CreateModule() 拿到模块实例。

  原理：
    extern "C" 让函数名不被 C++ 名称修饰（mangling），
    这样 GetProcAddress 才能找到函数。
*/

#include "Moduel_Base_Object.h"
#include <memory>

// ---- DLL 导出的工厂函数类型 ----
using CreateModuleFunc  = ModuleBaseObject * (*)();
using DestroyModuleFunc = void (*)(ModuleBaseObject*);

// ---- 宏：在模块 .cpp 里写 EXPORT_MODULE(类名) 即可 ----
#define EXPORT_MODULE(ClassName)                                            \
    extern "C" __declspec(dllexport) ModelBaseObject* CreateModule() {      \
        auto* m = new ClassName();                                          \
        if (!m->OnInit()) {                                                 \
            delete m;                                                       \
            return nullptr;                                                 \
        }                                                                   \
        return m;                                                           \
    }                                                                       \
    extern "C" __declspec(dllexport) void DestroyModule(ModelBaseObject* m) {\
        if (m) {                                                            \
            m->OnShutdown();                                                \
            delete m;                                                       \
        }                                                                   \
    }

// ---- 便捷宏：获取模块名称（用于注册表） ----
#define MODULE_API extern "C" __declspec(dllexport)
