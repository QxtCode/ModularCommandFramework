/// =================================================================
///  shared_memory.h — RAII 跨平台共享内存
/// =================================================================
///
///  替代 CreateFileMapping/MapViewOfFile (Windows) 和
///  shm_open/mmap (POSIX)。
///
///  用法:
///    // 创建（owner）
///    auto shm = SharedMemory::Create("metrics", 4096);
///    auto* data = static_cast<MetricsData*>(shm->Data());
///
///    // 打开已有（reader）
///    auto shm = SharedMemory::Open("metrics", 4096);
///    const auto* data = static_cast<const MetricsData*>(shm->Data());
///
///  设计要点:
///    - RAII: 析构时自动 unmap + close + unlink（owner）
///    - 名称统一: 所有平台使用普通字符串，Windows 内部转宽字符
///    - 无虚函数: 值语义，unique_ptr 持有
/// =================================================================

#pragma once
#include <cstddef>
#include <memory>
#include <string>

namespace platform {

class SharedMemory {
public:
    /// 创建新的共享内存区域（owner）。
    /// @param name 名称，POSIX 上自动加 "/" 前缀
    /// @param size 字节数
    /// @return 失败返回 nullptr
    static std::unique_ptr<SharedMemory> Create(const std::string& name,
                                                size_t size);

    /// 打开已存在的共享内存区域（reader / writer）。
    /// @param name 名称
    /// @param size 期望的字节数
    /// @return 失败返回 nullptr
    static std::unique_ptr<SharedMemory> Open(const std::string& name,
                                              size_t size);

    /// 析构自动 unmap + close + shm_unlink（仅 owner）
    ~SharedMemory();

    /// 获取映射区域的起始地址。
    void*       Data()       { return data_; }
    const void* Data() const { return data_; }

    /// 获取映射大小。
    size_t Size() const { return size_; }

    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

private:
    SharedMemory() = default;

    void*  data_ = nullptr;
    size_t size_ = 0;

#ifdef _WIN32
    void*   handle_ = nullptr;  // 文件映射句柄
    bool    is_owner_ = false;
#else
    int         fd_       = -1;
    std::string name_;         // 仅 owner 需 unlink
    bool        is_owner_ = false;
#endif
};

}  // namespace platform
