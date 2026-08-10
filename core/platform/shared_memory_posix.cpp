/// =================================================================
///  shared_memory_posix.cpp — POSIX 共享内存实现 (Linux / macOS)
/// =================================================================
///
///  CMake else() 分支编译此文件，_win.cpp 在 POSIX 上被跳过。
///
///  Create:   shm_open + ftruncate → mmap
///  Open:     shm_open → mmap
///  析构:     munmap + close（RAII 自动释放）
///  命名:     Linux 需要 shm_unlink 清理，macOS 行为略有差异
/// =================================================================

#include "shared_memory.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

namespace platform {

// POSIX shm_open 要求名称以 "/" 开头
static std::string ShmName(const std::string& name) {
    if (name.empty()) return "/";
    if (name[0] == '/') return name;
    return "/" + name;
}

std::unique_ptr<SharedMemory> SharedMemory::Create(const std::string& name,
                                                   size_t size) {
    if (size == 0) return nullptr;

    std::string sname = ShmName(name);

    int fd = shm_open(sname.c_str(), O_CREAT | O_RDWR | O_EXCL, 0666);
    bool is_new = (fd >= 0);
    if (!is_new) {
        // 已存在，打开写入
        fd = shm_open(sname.c_str(), O_RDWR, 0666);
        if (fd < 0) return nullptr;
    }

    if (is_new) {
        if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
            close(fd);
            shm_unlink(sname.c_str());
            return nullptr;
        }
    }

    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        if (is_new) shm_unlink(sname.c_str());
        return nullptr;
    }

    auto shm = std::unique_ptr<SharedMemory>(new SharedMemory());
    shm->fd_       = fd;
    shm->data_     = data;
    shm->size_     = size;
    shm->name_     = sname;
    shm->is_owner_ = is_new;  // 只有新建的才是 owner
    return shm;
}

std::unique_ptr<SharedMemory> SharedMemory::Open(const std::string& name,
                                                 size_t size) {
    std::string sname = ShmName(name);

    int fd = shm_open(sname.c_str(), O_RDONLY, 0666);
    if (fd < 0) return nullptr;

    void* data = mmap(nullptr, size, PROT_READ,
                      MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return nullptr;
    }

    auto shm = std::unique_ptr<SharedMemory>(new SharedMemory());
    shm->fd_       = fd;
    shm->data_     = data;
    shm->size_     = size;
    shm->name_     = sname;
    shm->is_owner_ = false;
    return shm;
}

SharedMemory::~SharedMemory() {
    if (data_ && data_ != MAP_FAILED) {
        munmap(data_, size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    if (is_owner_ && !name_.empty()) {
        shm_unlink(name_.c_str());
    }
}

}  // namespace platform
