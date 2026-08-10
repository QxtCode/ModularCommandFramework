/// =================================================================
///  process_win.cpp — Windows 进程管理实现
/// =================================================================
///
///  编译时分发: CMakeLists.txt 的 if(WIN32) 分支选中此文件，
///  process_posix.cpp 在 Windows 上不会被编译。
///  两个 .cpp 提供相同签名的函数，调用方只 include process.h，
///  无需知道底层是哪个 OS。
///
///  ExeDir/ExePath:        GetModuleFileNameA
///  CpuTimeUs:             GetProcessTimes → FILETIME
///  Launch:                CreateProcessA
/// =================================================================

#include "process.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>
#include <string>

namespace platform {

bool Process::Launch(const std::string& exePath,
                     const std::string& args,
                     const std::string& workDir,
                     bool newConsole) {
    std::string cmd = exePath;
    if (!args.empty()) {
        cmd += " " + args;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);

    DWORD flags = 0;
    if (newConsole) {
        flags |= CREATE_NEW_CONSOLE;
    }

    const char* dir = workDir.empty() ? nullptr : workDir.c_str();

    PROCESS_INFORMATION pi{};
    // cmd.data() is non-const in MSVC (but CreateProcessA won't actually modify it)
    BOOL ok = CreateProcessA(
        nullptr,                      // lpApplicationName
        cmd.data(),                   // lpCommandLine (non-const in API)
        nullptr, nullptr,             // lpProcessAttributes, lpThreadAttributes
        FALSE,                        // bInheritHandles
        flags,                        // dwCreationFlags
        nullptr,                      // lpEnvironment
        dir,                          // lpCurrentDirectory
        &si, &pi);

    if (!ok) return false;

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

uint64_t Process::CpuTimeUs() {
    FILETIME creation, exit, kernel, user;
    BOOL ok = GetProcessTimes(GetCurrentProcess(),
                              &creation, &exit, &kernel, &user);
    if (!ok) {
        return 0;
    }

    // FILETIME is in 100-nanosecond intervals
    // C-style cast matches existing working code in MetricsCollector
    uint64_t k = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    uint64_t u = ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;

    // Convert to microseconds: divide by 10
    return (k + u) / 10;
}

std::string Process::ExeDir() {
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return ".";

    std::string path(buf, len);
    size_t pos = path.find_last_of("\\/");
    if (pos != std::string::npos) {
        return path.substr(0, pos + 1);
    }
    return ".";
}

std::string Process::ExePath() {
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return "";
    return std::string(buf, len);
}

}  // namespace platform
