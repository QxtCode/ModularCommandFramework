/// =================================================================
///  shared_memory_win.cpp — Windows 共享内存实现
/// =================================================================
///
///  CMake if(WIN32) 分支编译此文件，_posix.cpp 在 Windows 上被跳过。
///
///  Create:   CreateFileMappingA(INVALID_HANDLE_VALUE) → MapViewOfFile
///  Open:     OpenFileMappingA → MapViewOfFile
///  析构:     UnmapViewOfFile + CloseHandle（RAII 自动释放）
/// =================================================================

#include "shared_memory.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

namespace platform {

// 将 UTF-8 字符串转为宽字符（用于 Windows API）
static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], len);
    w.resize(len - 1);  // 去掉 null terminator
    return w;
}

std::unique_ptr<SharedMemory> SharedMemory::Create(const std::string& name,
                                                   size_t size) {
    if (size == 0) return nullptr;

    std::wstring wname = ToWide(name);

    HANDLE hMap = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, static_cast<DWORD>(size), wname.c_str());

    if (!hMap) {
        // 尝试打开已存在的
        hMap = OpenFileMappingW(FILE_MAP_WRITE, FALSE, wname.c_str());
        if (!hMap) return nullptr;
    }

    void* data = MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, size);
    if (!data) {
        CloseHandle(hMap);
        return nullptr;
    }

    auto shm = std::unique_ptr<SharedMemory>(new SharedMemory());
    shm->handle_   = hMap;
    shm->data_     = data;
    shm->size_     = size;
    shm->is_owner_ = true;
    return shm;
}

std::unique_ptr<SharedMemory> SharedMemory::Open(const std::string& name,
                                                 size_t size) {
    std::wstring wname = ToWide(name);

    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, wname.c_str());
    if (!hMap) return nullptr;

    void* data = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, size);
    if (!data) {
        CloseHandle(hMap);
        return nullptr;
    }

    auto shm = std::unique_ptr<SharedMemory>(new SharedMemory());
    shm->handle_   = hMap;
    shm->data_     = data;
    shm->size_     = size;
    shm->is_owner_ = false;
    return shm;
}

SharedMemory::~SharedMemory() {
    if (data_) {
        UnmapViewOfFile(data_);
        data_ = nullptr;
    }
    if (handle_) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

}  // namespace platform
