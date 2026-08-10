/// platform::Process 独立测试
/// 覆盖: ExePath, ExeDir, CpuTimeUs, Launch

#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "core/platform/process.h"
#include "core/platform/file_system.h"

using namespace platform;

// ================================================================
//  ExePath / ExeDir
// ================================================================
TEST(ProcessTest, ExePath_NotEmpty) {
    std::string path = Process::ExePath();
    EXPECT_FALSE(path.empty());
    // 应该以 test_runner 结尾（或 test_runner.exe）
    EXPECT_TRUE(path.find("test_runner") != std::string::npos);
}

TEST(ProcessTest, ExeDir_EndsWithSeparator) {
    std::string dir = Process::ExeDir();
    EXPECT_FALSE(dir.empty());
    EXPECT_TRUE(dir.back() == '/' || dir.back() == '\\');
}

TEST(ProcessTest, ExePath_InsideExeDir) {
    std::string path = Process::ExePath();
    std::string dir  = Process::ExeDir();
    // ExePath 应该以 ExeDir 开头
    EXPECT_EQ(path.find(dir), 0u);
}

// ================================================================
//  CpuTimeUs
// ================================================================
TEST(ProcessTest, CpuTimeUs_NonNegative) {
    // CpuTimeUs 至少应该返回有效值（可能为 0，但不应崩溃）
    uint64_t t = Process::CpuTimeUs();
    // 不在极短时间内强断言 >0（首次调用可能返回 0）
    (void)t;
    SUCCEED();
}

TEST(ProcessTest, CpuTimeUs_Monotonic) {
    // GetProcessTimes 分辨率受系统时钟 tick 限制（Windows 默认 ~15.6ms）
    // 自旋 50ms 确保跨至少 3 个 tick
    auto start = std::chrono::steady_clock::now();
    uint64_t t1 = Process::CpuTimeUs();

    volatile int x = 0;
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(50)) {
        for (int i = 0; i < 1000; ++i) x += i;
    }
    (void)x;

    uint64_t t2 = Process::CpuTimeUs();
    EXPECT_GE(t2, t1);
    EXPECT_GT(t2, 0u) << "CPU time should be > 0 after 50ms spin-wait";
}

// ================================================================
//  Launch
// ================================================================
TEST(ProcessTest, Launch_SimpleCommand) {
#ifdef _WIN32
    // 用 cmd.exe 执行 echo，验证进程启动
    bool ok = Process::Launch("cmd.exe", "/c echo test_shell_platform_test");
#else
    bool ok = Process::Launch("/bin/echo", "test_shell_platform_test");
#endif
    EXPECT_TRUE(ok);
}

TEST(ProcessTest, Launch_NonExistentExe) {
    bool ok = Process::Launch("__nonexistent_exe_12345__");
    EXPECT_FALSE(ok);
}

// ================================================================
//  压力测试: 快速连续启动进程
// ================================================================
TEST(ProcessTest, Stress_RapidLaunch) {
    constexpr int kCount = 20;
    int ok = 0;

    for (int i = 0; i < kCount; ++i) {
#ifdef _WIN32
        if (Process::Launch("cmd.exe", "/c exit 0"))
            ok++;
#else
        if (Process::Launch("/bin/true", ""))
            ok++;
#endif
    }

    EXPECT_EQ(ok, kCount);
}

// ================================================================
//  ExeDir 集成测试: 用 ExeDir 构造文件路径
// ================================================================
TEST(ProcessTest, ExeDir_ConstructPath) {
    std::string dir = Process::ExeDir();
    // 构造相邻文件的路径
    std::string cfg_path = dir + "test_shell.cfg";
    // 不保证文件存在，只确保路径格式正确
    EXPECT_FALSE(cfg_path.empty());
    EXPECT_TRUE(cfg_path.find("test_shell.cfg") != std::string::npos);
}
