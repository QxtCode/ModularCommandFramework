/// =================================================================
///  Export.h — DLL 导出接口（SDK 自包含，零 core 依赖）
/// =================================================================
///
///  把 EXPORT_MODULE(YourClass) 放在模块 .cpp 末尾，生成 CreateModule /
///  DestroyModule / GetSdkApiVersion 导出符号，供宿主 LoadDLLModule 加载。
///
///  本文件自带导出宏，不依赖 core/platform —— SDK 可独立分发，
///  模块作者不需要 include core、不需要链接 eventbus。
///
///  【SDK API 版本（v2.8 引入）】
///  TEST_SHELL_SDK_API_MAJOR/MINOR 是模块接口的 ABI 版本，独立于宿主
///  功能版本（CMake VERSION）。宿主 LoadDLLModule 加载 DLL 时校验
///  GetSdkApiVersion() 返回值与宿主编译时版本严格匹配，不匹配拒绝加载，
///  防止「旧 SDK 编译的 DLL 加载到新宿主」导致内存布局不匹配崩溃。
///  - MAJOR：ABI 不兼容改动（ParmarPack/ModuleBase 内存布局变化）+1
///  - MINOR：兼容的功能增加 +1（当前严格匹配，将来可放宽为只校验 MAJOR）
/// =================================================================

#pragma once

#include <cstdint>
#include "ModuleBase.h"

// 自包含导出宏（等价 core/platform/platform.h 的 PLATFORM_EXPORT）
#if defined(_WIN32)
    #define TEST_SHELL_EXPORT __declspec(dllexport)
#else
    #define TEST_SHELL_EXPORT __attribute__((visibility("default")))
#endif

// ============================================================
//  SDK API 版本号（模块接口 ABI 版本，首次引入 = 1.0）
// ============================================================
#define TEST_SHELL_SDK_API_MAJOR 1
#define TEST_SHELL_SDK_API_MINOR 0

/// 把主.次版本编码成单个 uint32，便于跨 DLL 传递（major<<16 | minor）。
/// 模块和宿主都调它，保证用同一套宏算出同一值。
inline constexpr uint32_t TestShellSdkApiVersion() {
    return (static_cast<uint32_t>(TEST_SHELL_SDK_API_MAJOR) << 16)
         | static_cast<uint32_t>(TEST_SHELL_SDK_API_MINOR);
}

// =================================================================
//  EXPORT_MODULE macro — DLL 导出接口
// =================================================================
//  CreateModule()    由 LoadDLLModule 调用实例化模块。
//  GetSdkApiVersion() 返回模块编译时的 SDK API 版本，宿主加载时校验。
//  DestroyModule() DEPRECATED：框架用 shared_ptr 管理生命周期，
//  手动调用会 double-delete；仅向后兼容手动管理生命周期者保留。
#define EXPORT_MODULE(ClassName)                                            \
    extern "C" TEST_SHELL_EXPORT ModuleBase* CreateModule() {               \
        return new ClassName();                                             \
    }                                                                       \
    extern "C" TEST_SHELL_EXPORT unsigned GetSdkApiVersion() {              \
        return TestShellSdkApiVersion();                                    \
    }                                                                       \
    extern "C" TEST_SHELL_EXPORT void DestroyModule(ModuleBase* m) {        \
        if (m) { m->OnShutdown(); delete m; } /* DEPRECATED in v2.4 */      \
    }
