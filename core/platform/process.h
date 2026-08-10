/// =================================================================
///  process.h — 跨平台进程管理
/// =================================================================
///
///  提供:
///    - Launch()    — 启动子进程
///    - CpuTimeUs() — 当前进程 CPU 时间（微秒）
///    - ExeDir()    — 当前可执行文件所在目录
///    - ExePath()   — 当前可执行文件完整路径
///
///  用法:
///    Process::Launch("shell_monitor", "", ".", true);
///    uint64_t cpu = Process::CpuTimeUs();
///    std::string dir = Process::ExeDir();  // "/usr/bin/"
/// =================================================================

#pragma once
#include <cstdint>
#include <string>

namespace platform {

class Process {
public:
    /// 启动子进程。
    /// @param exePath    可执行文件路径
    /// @param args       命令行参数（Windows 风格，空格分隔）
    /// @param workDir    工作目录（空 = 继承当前）
    /// @param newConsole 是否开新终端窗口（POSIX 忽略）
    /// @return 成功返回 true
    static bool Launch(const std::string& exePath,
                       const std::string& args = "",
                       const std::string& workDir = "",
                       bool newConsole = false);

    /// 获取当前进程的 CPU 时间（微秒）。
    /// 返回 kernel + user 时间之和。
    static uint64_t CpuTimeUs();

    /// 获取当前可执行文件所在目录（结尾带分隔符）。
    static std::string ExeDir();

    /// 获取当前可执行文件的完整路径。
    static std::string ExePath();
};

}  // namespace platform
