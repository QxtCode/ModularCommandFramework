/// =================================================================
///  shared_library_posix.cpp — POSIX 动态库加载实现 (Linux / macOS)
/// =================================================================
///
///  CMake else() 分支编译此文件，_win.cpp 在 POSIX 上被跳过。
///
///  Load:         dlopen → RAII SharedLibrary 对象
///  GetRawSymbol: dlsym
///  ~SharedLibrary: dlclose（RAII 自动释放）
/// =================================================================

#include "shared_library.h"

#include <dlfcn.h>

namespace platform {

std::unique_ptr<SharedLibrary> SharedLibrary::Load(const std::string& path) {
    // RTLD_NOW: 立即解析所有符号，失败时尽早暴露错误
    // RTLD_LOCAL 是默认值，但显式写出更清晰
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) return nullptr;

    auto lib = std::unique_ptr<SharedLibrary>(new SharedLibrary());
    lib->handle_ = handle;
    return lib;
}

SharedLibrary::~SharedLibrary() {
    if (handle_) {
        dlclose(handle_);
    }
}

void* SharedLibrary::GetRawSymbol(const std::string& name) {
    if (!handle_) return nullptr;
    return dlsym(handle_, name.c_str());
}

}  // namespace platform
