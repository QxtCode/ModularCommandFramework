/// =================================================================
///  shared_library_win.cpp — Windows 动态库加载实现
/// =================================================================
///
///  CMake if(WIN32) 分支编译此文件，_posix.cpp 在 Windows 上被跳过。
///
///  Load:        LoadLibraryA → RAII SharedLibrary 对象
///  GetRawSymbol: GetProcAddress
///  ~SharedLibrary: FreeLibrary（RAII 自动释放）
/// =================================================================

#include "shared_library.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

namespace platform {

std::unique_ptr<SharedLibrary> SharedLibrary::Load(const std::string& path) {
    HMODULE handle = LoadLibraryA(path.c_str());
    if (!handle) return nullptr;

    auto lib = std::unique_ptr<SharedLibrary>(new SharedLibrary());
    lib->handle_ = static_cast<NativeHandle>(handle);
    return lib;
}

SharedLibrary::~SharedLibrary() {
    if (handle_) {
        FreeLibrary(static_cast<HMODULE>(handle_));
    }
}

void* SharedLibrary::GetRawSymbol(const std::string& name) {
    if (!handle_) return nullptr;
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle_), name.c_str()));
}

}  // namespace platform
