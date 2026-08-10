/// =================================================================
///  console.cpp — 控制台初始化和内存泄漏检测
/// =================================================================
///
///  这是 platform.h 中 inline 函数 InitConsole / InitLeakDetection 的
///  实际实现。拆成独立 .cpp 是为了避免 platform.h 直接 include <windows.h>，
///  污染所有翻译单元的命名空间。platform.h 只通过 extern 声明引用这两个函数。
///
///  注意：这是整个 Platform 层唯一直接使用 #if PLATFORM_WINDOWS 的文件，
///  其他所有平台差异通过 if(WIN32) CMake 编译时分发处理。
///  调用方（main.cpp）只需 include "core/platform/platform.h"。
/// =================================================================

#include "platform.h"

#if PLATFORM_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

    #ifdef _DEBUG
        #include <crtdbg.h>
    #endif
#endif

void PlatformInitConsole() {
#if PLATFORM_WINDOWS
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

void PlatformInitLeakDetection() {
#if PLATFORM_WINDOWS && defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
}
