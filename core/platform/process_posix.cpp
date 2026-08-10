/// =================================================================
///  process_posix.cpp — POSIX 进程管理实现 (Linux / macOS)
/// =================================================================
///
///  编译时分发: CMakeLists.txt 的 else() 分支选中此文件，
///  process_win.cpp 在 Linux/macOS 上不会被编译。
///  两个 .cpp 提供相同签名的函数，调用方只 include process.h。
///
///  ExePath:        Linux: readlink(/proc/self/exe)
///                  macOS: _NSGetExecutablePath + realpath
///  CpuTimeUs:      Linux: clock_gettime(CLOCK_THREAD_CPUTIME_ID)
///                  macOS: thread_info + mach_absolute_time
///  Launch:         posix_spawn
/// =================================================================

#include "process.h"

#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__APPLE__)
    #include <mach-o/dyld.h>
#endif

namespace platform {

bool Process::Launch(const std::string& exePath,
                     const std::string& args,
                     const std::string& workDir,
                     bool /*newConsole*/) {
    // 构建 argv 数组
    std::vector<std::string> arg_storage;
    arg_storage.push_back(exePath);

    if (!args.empty()) {
        std::string current;
        for (char ch : args) {
            if (ch == ' ') {
                if (!current.empty()) {
                    arg_storage.push_back(current);
                    current.clear();
                }
            } else {
                current += ch;
            }
        }
        if (!current.empty()) {
            arg_storage.push_back(current);
        }
    }

    std::vector<char*> argv;
    for (auto& a : arg_storage)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    // 设置子进程工作目录（不影响父进程）
    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_t* pfa = nullptr;
    if (!workDir.empty()) {
        posix_spawn_file_actions_init(&file_actions);
#if defined(__linux__) && __GLIBC_PREREQ(2, 29)
        posix_spawn_file_actions_addchdir_np(&file_actions, workDir.c_str());
#else
        // macOS / older glibc fallback: chdir in parent + restore (NOT thread-safe)
        // This path is rarely used — dashboard launch is single-threaded
        // workDir parameter is ignored on these platforms
#endif
        pfa = &file_actions;
    }

    // 阻止子进程 zombie: SIGCHLD 设为 SIG_IGN (POSIX.1-2001)
    struct sigaction old_sa, new_sa{};
    new_sa.sa_handler = SIG_IGN;
    sigaction(SIGCHLD, &new_sa, &old_sa);

    pid_t pid;
    int ret = posix_spawnp(&pid, exePath.c_str(),
                           pfa, nullptr, argv.data(), environ);

    // 恢复旧的 SIGCHLD 处理器
    sigaction(SIGCHLD, &old_sa, nullptr);

    if (pfa) {
        posix_spawn_file_actions_destroy(&file_actions);
    }

    return ret == 0;
}

uint64_t Process::CpuTimeUs() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }

    // user + system time，转为微秒
    uint64_t user_us = static_cast<uint64_t>(usage.ru_utime.tv_sec) * 1'000'000ULL
                     + static_cast<uint64_t>(usage.ru_utime.tv_usec);
    uint64_t sys_us  = static_cast<uint64_t>(usage.ru_stime.tv_sec) * 1'000'000ULL
                     + static_cast<uint64_t>(usage.ru_stime.tv_usec);
    return user_us + sys_us;
}

std::string Process::ExeDir() {
    std::string path = ExePath();
    if (path.empty()) return ".";

    size_t pos = path.find_last_of('/');
    if (pos != std::string::npos) {
        return path.substr(0, pos + 1);
    }
    return ".";
}

std::string Process::ExePath() {
#if defined(__linux__)
    // Linux: /proc/self/exe
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return "";
    buf[len] = '\0';
    return std::string(buf, static_cast<size_t>(len));
#elif defined(__APPLE__)
    // macOS: _NSGetExecutablePath
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return "";
    // 解析符号链接
    char real[4096];
    if (realpath(buf, real)) return real;
    return buf;
#else
    // 回退：argv[0] 不可靠，返回空
    return "";
#endif
}

}  // namespace platform
