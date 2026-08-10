/// =================================================================
///  platform.h — 平台检测 + 导出宏 + 小工具函数
/// =================================================================
///
///  整个框架唯一的平台差异集中点。其他文件只 include 此头，
///  不写 #ifdef _WIN32 / #ifdef __linux__。
///
///  提供:
///    - 平台检测宏 (PLATFORM_WINDOWS / PLATFORM_LINUX / PLATFORM_MACOS)
///    - 符号导出宏 (PLATFORM_EXPORT / PLATFORM_IMPORT)
///    - 共享库扩展名常量
///    - 控制台初始化
///    - 内存泄漏检测初始化
/// =================================================================

#pragma once

// ================================================================
//  平台检测
// ================================================================
#if defined(_WIN32)
    #define PLATFORM_WINDOWS 1
#elif defined(__APPLE__)
    #define PLATFORM_MACOS 1
#elif defined(__linux__)
    #define PLATFORM_LINUX 1
#else
    #error "Unsupported platform"
#endif

// ================================================================
//  符号导出宏 — 替代 __declspec(dllexport/dllimport)
// ================================================================
// 用法: 在 EVENTBUS_DLL_EXPORTS 定义时展开为导出，否则为导入。
// Windows 需要显式导入声明；Linux/macOS 默认所有符号可见，
// 仅当 -fvisibility=hidden 时需要用 default 标记导出。
#if PLATFORM_WINDOWS
    #define PLATFORM_EXPORT __declspec(dllexport)
    #define PLATFORM_IMPORT __declspec(dllimport)
#else
    #define PLATFORM_EXPORT __attribute__((visibility("default")))
    #define PLATFORM_IMPORT
#endif

// ================================================================
//  共享库扩展名
// ================================================================
#if PLATFORM_WINDOWS
    inline constexpr const char* kSharedLibExt = ".dll";
#elif PLATFORM_MACOS
    inline constexpr const char* kSharedLibExt = ".dylib";
#else
    inline constexpr const char* kSharedLibExt = ".so";
#endif

// ================================================================
//  控制台初始化 — 设置 UTF-8 编码
// ================================================================
// Windows 控制台默认不是 UTF-8，需要显式设置。
// Linux/macOS 终端默认就是 UTF-8，无需操作。
inline void InitConsole() {
#if PLATFORM_WINDOWS
    // 这两个函数在 windows.h 中声明，调用方需先 include <windows.h>
    // 或依赖 CoreApplication 初始化。
    // 实际实现在 console.cpp 中（避免每个翻译单元都 include windows.h）。
    extern void PlatformInitConsole();
    PlatformInitConsole();
#else
    // POSIX 终端默认 UTF-8，无需操作
#endif
}

// ================================================================
//  内存泄漏检测 — 开发模式开启
// ================================================================
inline void InitLeakDetection() {
#if PLATFORM_WINDOWS && defined(_DEBUG)
    extern void PlatformInitLeakDetection();
    PlatformInitLeakDetection();
#else
    // Linux/macOS: 使用 ASan/LSan 编译参数，无需代码介入
#endif
}
