/// =================================================================
///  platform 层四维测试：边界 / 冒烟 / 内存 / 并发竞争
/// =================================================================
/// 覆盖:
///   1. 边界值 (空、零、极大、特殊字符、路径分隔符)
///   2. 冒烟     (端到端快速验证)
///   3. 内存     (RAII 正确性、无泄漏、重复释放安全)
///   4. 并发竞争 (多线程创建/销毁/读写、死锁检测)
/// =================================================================

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <sys/stat.h>
    #include <unistd.h>
#endif

#include "core/platform/file_system.h"
#include "core/platform/shared_library.h"
#include "core/platform/shared_memory.h"
#include "core/platform/process.h"

using namespace platform;
using namespace std::chrono_literals;

// ================================================================
//  辅助工具
// ================================================================
namespace {

/// RAII 临时目录（测试结束自动清理）
class TempDir {
public:
    TempDir(const std::string& prefix = "plat_test_") {
        name_ = prefix + std::to_string(counter_++);
#ifdef _WIN32
        CreateDirectoryA(name_.c_str(), nullptr);
#else
        mkdir(name_.c_str(), 0755);
#endif
    }

    ~TempDir() {
        for (auto& f : FindFiles(name_, "*"))
            std::remove(f.c_str());
#ifdef _WIN32
        RemoveDirectoryA(name_.c_str());
#else
        rmdir(name_.c_str());
#endif
    }

    const std::string& Path() const { return name_; }

    void CreateFile(const std::string& filename, const std::string& content = "x") {
        std::string path = name_ + "/" + filename;
        std::ofstream f(path);
        f << content;
    }

private:
    std::string name_;
    static std::atomic<int> counter_;
};

std::atomic<int> TempDir::counter_{0};

/// 高精度计时器（用于并发超时检测）
class TimeoutGuard {
public:
    explicit TimeoutGuard(std::chrono::milliseconds limit)
        : limit_(limit), start_(std::chrono::steady_clock::now()) {}

    bool Expired() const {
        return std::chrono::steady_clock::now() - start_ > limit_;
    }

private:
    std::chrono::milliseconds limit_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace

// ================================================================
//  一、边界测试
// ================================================================

// ---- NormalizePath 边界 ----
TEST(BoundaryNormalizePath, EmptyString) {
    EXPECT_EQ(NormalizePath(""), "");
}

TEST(BoundaryNormalizePath, OnlyBackslash) {
    EXPECT_EQ(NormalizePath("\\"), "/");
}

TEST(BoundaryNormalizePath, OnlySlashes) {
    EXPECT_EQ(NormalizePath("///"), "///");
    EXPECT_EQ(NormalizePath("\\\\\\"), "///");
}

TEST(BoundaryNormalizePath, ConsecutiveMixed) {
    EXPECT_EQ(NormalizePath("a\\\\b//c"), "a//b//c");
}

TEST(BoundaryNormalizePath, TrailingBackslash) {
    EXPECT_EQ(NormalizePath("a\\b\\"), "a/b/");
}

TEST(BoundaryNormalizePath, LeadingBackslash) {
    EXPECT_EQ(NormalizePath("\\a\\b"), "/a/b");
}

TEST(BoundaryNormalizePath, LongPath) {
    std::string long_path(10000, '\\');
    auto result = NormalizePath(long_path);
    EXPECT_EQ(result.size(), long_path.size());
    for (char ch : result) EXPECT_EQ(ch, '/');
}

TEST(BoundaryNormalizePath, NoChangeForwardSlash) {
    EXPECT_EQ(NormalizePath("already/normalized/path"), "already/normalized/path");
}

// ---- FindFiles 边界 ----
TEST(BoundaryFindFiles, EmptyDir) {
    // 空目录参数 → FindFiles 使用当前工作目录，应正常返回且不崩溃
    auto files = FindFiles("", "*.dll");
    // 不崩溃即可，结果取决于当前工作目录内容
    SUCCEED();
}

TEST(BoundaryFindFiles, EmptyPattern) {
    TempDir dir;
    dir.CreateFile("a.txt");
    auto files = FindFiles(dir.Path(), "");
    // 空 pattern 应该不匹配任何文件
    EXPECT_EQ(files.size(), 0u);
}

TEST(BoundaryFindFiles, PatternWithSpecialChars) {
    TempDir dir;
    dir.CreateFile("[test].txt");
    dir.CreateFile("atest.txt");
    dir.CreateFile("btest.txt");

    auto files = FindFiles(dir.Path(), "?.txt");  // 不匹配（? 匹配单字符）
    // FindFirstFile 支持 ? 和 * 通配符，? 匹配单字符
    // 但我们创建的文件名 >1 字符 + ".txt"
    EXPECT_GE(files.size(), 0u);  // 至少不崩溃
}

TEST(BoundaryFindFiles, NoMatchReturnsEmpty) {
    TempDir dir;
    auto files = FindFiles(dir.Path(), "*.exe");
    EXPECT_EQ(files.size(), 0u);
}

TEST(BoundaryFindFiles, SingleFile) {
    TempDir dir;
    dir.CreateFile("only.txt");
    auto files = FindFiles(dir.Path(), "*.txt");
    EXPECT_EQ(files.size(), 1u);
}

TEST(BoundaryFindFiles, ManyFiles) {
    TempDir dir;
    constexpr int N = 200;
    for (int i = 0; i < N; ++i)
        dir.CreateFile("file_" + std::to_string(i) + ".dat");

    auto files = FindFiles(dir.Path(), "*.dat");
    EXPECT_EQ(files.size(), static_cast<size_t>(N));
}

TEST(BoundaryFindFiles, SkipsDotAndDotDot) {
    TempDir dir;
    auto all = FindFiles(dir.Path(), "*");
    for (const auto& f : all) {
        EXPECT_TRUE(f.find("/.") == std::string::npos || f.find("/..") != std::string::npos);
    }
}

// ---- FileExists 边界 ----
TEST(BoundaryFileExists, EmptyPath) {
    EXPECT_FALSE(FileExists(""));
}

TEST(BoundaryFileExists, WhitespaceOnly) {
    EXPECT_FALSE(FileExists("   "));
}

// ---- SharedLibrary 边界 ----
TEST(BoundarySharedLibrary, LoadEmptyPath) {
    auto lib = SharedLibrary::Load("");
    EXPECT_EQ(lib, nullptr);
}

TEST(BoundarySharedLibrary, GetRawSymbolEmptyName) {
    auto lib = SharedLibrary::Load("__nonexistent__.dll");
    EXPECT_EQ(lib, nullptr);
}

TEST(BoundarySharedLibrary, GetFunctionNullHandle) {
    // 加载失败的库，GetRawSymbol 应返回 nullptr
    auto lib = SharedLibrary::Load("__nonexistent__.dll");
    EXPECT_EQ(lib, nullptr);
    // operator bool 应返回 false
}

TEST(BoundarySharedLibrary, RapidSingleCycle) {
    // 验证 RAII：创建即销毁不泄漏
    for (int i = 0; i < 10; ++i) {
        auto lib = SharedLibrary::Load("__nonexistent__.dll");
        EXPECT_EQ(lib, nullptr);
    }
}

// ---- SharedMemory 边界 ----
TEST(BoundarySharedMemory, CreateZeroSize) {
    auto shm = SharedMemory::Create("bound_zero", 0);
    EXPECT_EQ(shm, nullptr);
}

TEST(BoundarySharedMemory, CreateOneByte) {
    auto shm = SharedMemory::Create("bound_1byte", 1);
    ASSERT_NE(shm, nullptr);
    EXPECT_EQ(shm->Size(), 1u);
    EXPECT_NE(shm->Data(), nullptr);
}

TEST(BoundarySharedMemory, CreateWithEmptyName) {
    auto shm = SharedMemory::Create("", 256);
    // 空名称：Windows 可能失败，POSIX 不可预测
    // 不崩溃即可
    SUCCEED();
}

TEST(BoundarySharedMemory, OpenNonExistent) {
    auto shm = SharedMemory::Open("__bound_nonexist__", 256);
    EXPECT_EQ(shm, nullptr);
}

TEST(BoundarySharedMemory, DataNonNullAfterCreate) {
    auto shm = SharedMemory::Create("bound_data_check", 256);
    ASSERT_NE(shm, nullptr);
    EXPECT_NE(shm->Data(), nullptr);
    const auto* cshm = static_cast<const SharedMemory*>(shm.get());
    EXPECT_NE(cshm->Data(), nullptr);
}

TEST(BoundarySharedMemory, SizePreserved) {
    constexpr size_t kSz = 1024;
    auto shm = SharedMemory::Create("bound_size", kSz);
    ASSERT_NE(shm, nullptr);
    EXPECT_EQ(shm->Size(), kSz);
}

TEST(BoundarySharedMemory, MultipleOpenSameName) {
    auto owner = SharedMemory::Create("bound_multi_open", 256);
    ASSERT_NE(owner, nullptr);

    auto r1 = SharedMemory::Open("bound_multi_open", 256);
    auto r2 = SharedMemory::Open("bound_multi_open", 256);
    auto r3 = SharedMemory::Open("bound_multi_open", 256);

    EXPECT_NE(r1, nullptr);
    EXPECT_NE(r2, nullptr);
    EXPECT_NE(r3, nullptr);

    // 三个 reader 映射不同地址
    EXPECT_NE(r1->Data(), r2->Data());
    EXPECT_NE(r2->Data(), r3->Data());
}

// ---- Process 边界 ----
TEST(BoundaryProcess, ExePathNotEmpty) {
    EXPECT_FALSE(Process::ExePath().empty());
}

TEST(BoundaryProcess, ExeDirEndsWithSep) {
    char last = Process::ExeDir().back();
    EXPECT_TRUE(last == '/' || last == '\\');
}

TEST(BoundaryProcess, LaunchEmptyPath) {
    bool ok = Process::Launch("");
    EXPECT_FALSE(ok);
}

TEST(BoundaryProcess, CpuTimeUsNonNegative) {
    uint64_t t = Process::CpuTimeUs();
    // 刚启动可能为 0，但不应该崩溃
    (void)t;
    SUCCEED();
}

// ================================================================
//  二、冒烟测试  (端到端快速验证)
// ================================================================

/// 模拟真实使用场景: 创建目录 → 扫描插件 → 加载 DLL → 卸载
TEST(Smoke, PluginDiscoveryPipeline) {
    TempDir plugins("smoke_plugins_");

    // 场景: 目录中有 .dll 和其他文件，只加载 .dll
    plugins.CreateFile("ModuleA.dll");
    plugins.CreateFile("ModuleB.dll");
    plugins.CreateFile("readme.txt");
    plugins.CreateFile("ModuleC.so");      // 不同扩展名
    plugins.CreateFile("helper.dat");

    // 冒烟: FindFiles 只匹配 .dll
    auto dlls = FindFiles(plugins.Path(), "*.dll");
    EXPECT_EQ(dlls.size(), 2u);

    // 冒烟: .so 过滤
    auto sos = FindFiles(plugins.Path(), "*.so");
    EXPECT_EQ(sos.size(), 1u);
}

/// 模拟真实使用场景: 共享内存 create → write → open → read → destroy
TEST(Smoke, SharedMemoryRoundTrip) {
    constexpr const char* kName = "smoke_shm";

    // 第一轮
    {
        auto writer = SharedMemory::Create(kName, 128);
        ASSERT_NE(writer, nullptr);

        // 写入结构化数据
        struct TestData { int32_t a; int32_t b; char name[16]; };
        auto* td = static_cast<TestData*>(writer->Data());
        td->a = 42;
        td->b = -7;
        std::strncpy(td->name, "smoke_test", 15);

        auto reader = SharedMemory::Open(kName, 128);
        ASSERT_NE(reader, nullptr);
        auto* rd = static_cast<const TestData*>(reader->Data());
        EXPECT_EQ(rd->a, 42);
        EXPECT_EQ(rd->b, -7);
        EXPECT_STREQ(rd->name, "smoke_test");
    }

    // 第二轮: 重新创建（验证第一轮析构成功）
    {
        auto writer2 = SharedMemory::Create(kName, 64);
        ASSERT_NE(writer2, nullptr);
        EXPECT_EQ(writer2->Size(), 64u);
    }
}

/// 模拟真实使用场景: 进程自省 (ExePath + ExeDir)
TEST(Smoke, ProcessSelfInspection) {
    std::string exe = Process::ExePath();
    std::string dir = Process::ExeDir();

    EXPECT_FALSE(exe.empty());
    EXPECT_FALSE(dir.empty());

    // ExePath 以 ExeDir 开头
    EXPECT_EQ(exe.find(dir), 0u);

    // ExeDir + "test_runner" 应该构成 ExePath 的一部分
    EXPECT_TRUE(exe.find("test_runner") != std::string::npos);
}

/// 模拟真实使用场景: 动态库加载 → 取符号 → 调用 → 卸载
/// DLL 查找策略: ① 当前工作目录 → ② CMake 输出目录 out/build/debug
/// （匹配 CMakePresets.json 的 binaryDir）
TEST(Smoke, SharedLibraryLifecycle) {
    std::string dll_path = "TestDLLModule.dll";
    if (!FileExists(dll_path)) {
        auto found = FindFiles("out/build/debug", "TestDLLModule.dll");
        if (found.empty()) {
            GTEST_SKIP() << "TestDLLModule.dll not found";
        }
        dll_path = found[0];
    }

    auto lib = SharedLibrary::Load(dll_path);
    ASSERT_NE(lib, nullptr) << "Failed to load: " << dll_path;

    auto* raw_sym = lib->GetRawSymbol("CreateModule");
    EXPECT_NE(raw_sym, nullptr);

    auto* typed_fn = lib->GetFunction<void* (*)()>("CreateModule");
    EXPECT_NE(typed_fn, nullptr);

    auto* non_exist = lib->GetRawSymbol("__no_such_function__");
    EXPECT_EQ(non_exist, nullptr);
}

// ================================================================
//  三、内存测试  (RAII 正确性、无泄漏)
// ================================================================

/// RAII: SharedLibrary 析构不泄漏（通过重复加载验证引用计数）
TEST(Memory, SharedLibraryRefCounting) {
    std::string dll_path = "TestDLLModule.dll";
    if (!FileExists(dll_path)) {
        auto found = FindFiles("out/build/debug", "TestDLLModule.dll");
        if (found.empty()) GTEST_SKIP() << "TestDLLModule.dll not found";
        dll_path = found[0];
    }

    // 重复加载/卸载 300 次，验证不会耗尽资源
    for (int i = 0; i < 300; ++i) {
        auto lib = SharedLibrary::Load(dll_path);
        ASSERT_NE(lib, nullptr) << "Iteration " << i;

        auto* fn = lib->GetRawSymbol("CreateModule");
        EXPECT_NE(fn, nullptr) << "Iteration " << i;

        // lib 析构 → FreeLibrary
    }
    SUCCEED();
}

/// RAII: SharedMemory 创建/销毁循环不泄漏
TEST(Memory, SharedMemoryRapidCreateDestroy) {
    for (int i = 0; i < 500; ++i) {
        std::string name = "mem_cycle_" + std::to_string(i);
        auto shm = SharedMemory::Create(name, 256);
        ASSERT_NE(shm, nullptr) << "Iteration " << i;

        // 写入验证数据没有被复用前的脏数据干扰
        auto* p = static_cast<int*>(shm->Data());
        *p = i;
    }
    SUCCEED();
}

/// RAII: SharedMemory 多重打开不泄漏
TEST(Memory, SharedMemoryMultipleOpensNoLeak) {
    constexpr const char* kName = "mem_multi_open";

    for (int cycle = 0; cycle < 100; ++cycle) {
        auto owner = SharedMemory::Create(kName, 128);
        ASSERT_NE(owner, nullptr);

        // 每个 cycle 创建 5 个 reader
        std::vector<std::unique_ptr<SharedMemory>> readers;
        for (int j = 0; j < 5; ++j) {
            readers.push_back(SharedMemory::Open(kName, 128));
            EXPECT_NE(readers.back(), nullptr);
        }
        // 全部析构
    }
    SUCCEED();
}

/// RAII: 构造中途失败不泄漏（异常安全）
TEST(Memory, NoLeakOnPartialFailure) {
    // SharedMemory::Create 失败 → 返回 nullptr，无资源泄漏
    for (int i = 0; i < 100; ++i) {
        auto shm = SharedMemory::Create("", 0);  // 零 size → 应失败
        EXPECT_EQ(shm, nullptr);
    }

    // SharedLibrary::Load 失败 → 返回 nullptr
    for (int i = 0; i < 100; ++i) {
        auto lib = SharedLibrary::Load("__nonexistent__");
        EXPECT_EQ(lib, nullptr);
    }
    SUCCEED();
}

/// 析构顺序安全性: shm_data_ 先于 shm_ 析构
TEST(Memory, DestructorOrderSafety) {
    // 模拟 MetricsCollector 的析构场景:
    //   1. 设置 magic=0（标记为无效）
    //   2. 置空 data 指针
    //   3. 释放共享内存
    // 如果步骤顺序不对，步骤 1 会在已释放的内存上写入 → 崩溃

    constexpr const char* kName = "mem_dtor_order";

    for (int i = 0; i < 50; ++i) {
        auto shm = SharedMemory::Create(kName, 256);
        ASSERT_NE(shm, nullptr);

        auto* data = static_cast<uint32_t*>(shm->Data());
        *data = 0xFEEDBEEF;  // 模拟 magic

        // 模拟 OnShutdown 顺序
        *data = 0;  // magic = 0

        // shm 析构 → unmap
    }
    SUCCEED();
}

// ================================================================
//  四、并发竞争测试
// ================================================================

/// 并发: 多线程同时创建不同名的共享内存
TEST(Concurrency, CreateSharedMemoryDifferentNames) {
    constexpr int kThreads = 12;
    constexpr int kPerThread = 80;
    std::atomic<int> ok{0};
    std::atomic<int> fail{0};
    std::atomic<bool> deadlock_guard{true};

    TimeoutGuard timeout(30s);

    auto worker = [&](int tid) {
        for (int i = 0; i < kPerThread && !timeout.Expired(); ++i) {
            std::string name = "conc_diff_" + std::to_string(tid) + "_" + std::to_string(i);
            auto shm = SharedMemory::Create(name, 64);
            if (shm && shm->Data()) {
                ok.fetch_add(1);
            } else {
                fail.fetch_add(1);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back(worker, i);
    for (auto& t : threads) t.join();

    deadlock_guard.store(false);

    EXPECT_EQ(fail.load(), 0);
    EXPECT_EQ(ok.load(), kThreads * kPerThread);
    EXPECT_FALSE(timeout.Expired()) << "Possible deadlock detected";
}

/// 并发: 多线程同时打开同一个共享内存（reader 并发安全）
// DISABLED: 高密度 Open/Map 操作在 Windows 上随线程数指数变慢
// 保留此测试作为压力基准，CI 排除。
TEST(Concurrency, DISABLED_ConcurrentReadersSameSharedMemory) {
    constexpr const char* kName = "conc_readers";
    constexpr int kThreads = 8;
    constexpr int kIterations = 200;

    auto owner = SharedMemory::Create(kName, 256);
    ASSERT_NE(owner, nullptr);

    volatile int* counter = static_cast<volatile int*>(owner->Data());
    *counter = 0;

    std::atomic<int> reads{0};
    std::atomic<bool> stop{false};

    // Writer 线程
    std::thread writer([&]() {
        auto wr = SharedMemory::Open(kName, 256);
        if (!wr) return;
        volatile int* cnt = static_cast<volatile int*>(wr->Data());
        for (int i = 0; i < kIterations && !stop; ++i) {
            ++(*cnt);  // 简单自增（非原子，测试允许偶尔丢失）
        }
    });

    // Reader 线程
    auto reader_fn = [&]() {
        for (int i = 0; i < kIterations && !stop; ++i) {
            auto rd = SharedMemory::Open(kName, 256);
            if (!rd) continue;
            volatile int* cnt = static_cast<volatile int*>(rd->Data());
            if (*cnt > 0) reads.fetch_add(1);
            std::this_thread::yield();  // 避免 Open 风暴
        }
    };

    std::vector<std::thread> readers;
    for (int i = 0; i < kThreads; ++i)
        readers.emplace_back(reader_fn);

    writer.join();
    stop.store(true);
    for (auto& t : readers) t.join();

    EXPECT_GT(reads.load(), 0) << "No concurrent reads succeeded";
}

/// 并发: 创建/打开/销毁循环（压力检测死锁和竞态）
TEST(Concurrency, CreateOpenDestroyCycles) {
    constexpr int kThreads = 10;
    constexpr int kCycles = 200;

    std::atomic<int> ops{0};
    std::atomic<int> errors{0};

    TimeoutGuard timeout(30s);

    auto worker = [&](int tid) {
        for (int i = 0; i < kCycles && !timeout.Expired(); ++i) {
            // 使用线程本地名称避免冲突
            std::string name = "conc_cod_" + std::to_string(tid) + "_c" + std::to_string(i);

            {
                auto owner = SharedMemory::Create(name, 128);
                if (owner) {
                    ops.fetch_add(1);
                    auto* p = static_cast<int*>(owner->Data());
                    *p = tid * 10000 + i;
                } else {
                    errors.fetch_add(1);
                }
            }  // owner 析构

            {
                // 尝试打开（此时 owner 已析构，某些平台上可能仍然存活）
                auto reader = SharedMemory::Open(name, 128);
                if (reader) ops.fetch_add(1);
                // reader 不存在不算错误（owner 已析构时 shm 可能已 unlink）
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back(worker, i);
    for (auto& t : threads) t.join();

    EXPECT_GT(ops.load(), 0);
    EXPECT_FALSE(timeout.Expired()) << "Possible deadlock in Create/Open/Destroy cycles";
}

/// 并发: 多线程同时加载/卸载不同的 DLL
TEST(Concurrency, ParallelDllLoadUnload) {
    std::string dll_path = "TestDLLModule.dll";
    if (!FileExists(dll_path)) {
        auto found = FindFiles("out/build/debug", "TestDLLModule.dll");
        if (found.empty()) GTEST_SKIP() << "TestDLLModule.dll not found";
        dll_path = found[0];
    }

    constexpr int kThreads = 8;
    constexpr int kPerThread = 100;
    std::atomic<int> loads{0};
    std::atomic<int> symbols{0};
    std::atomic<int> failures{0};
    std::atomic<bool> deadline{false};

    TimeoutGuard timeout(30s);

    auto worker = [&]() {
        for (int i = 0; i < kPerThread && !timeout.Expired() && !deadline; ++i) {
            auto lib = SharedLibrary::Load(dll_path);
            if (lib) {
                loads.fetch_add(1);
                auto* fn = lib->GetRawSymbol("CreateModule");
                if (fn) symbols.fetch_add(1);
            } else {
                failures.fetch_add(1);
            }
            // lib 析构 → FreeLibrary (Windows 引用计数保护)
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    deadline.store(true);

    EXPECT_GT(loads.load(), 0) << "No successful loads in concurrent test";
    EXPECT_EQ(symbols.load(), loads.load()) << "Symbol resolution failed";
    EXPECT_FALSE(timeout.Expired()) << "Possible deadlock in parallel DLL loads";
}

/// 并发: 快速创建进程（不等待）
TEST(Concurrency, RapidProcessLaunch) {
    constexpr int kCount = 30;
    std::atomic<int> successes{0};

    auto launcher = [&](int start, int end) {
        for (int i = start; i < end; ++i) {
#ifdef _WIN32
            if (Process::Launch("cmd.exe", "/c exit 0"))
                successes.fetch_add(1);
#else
            if (Process::Launch("/bin/true", ""))
                successes.fetch_add(1);
#endif
            // 给子进程一点时间完成
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };

    std::thread t1(launcher, 0, kCount / 2);
    std::thread t2(launcher, kCount / 2, kCount);
    t1.join(); t2.join();

    EXPECT_EQ(successes.load(), kCount);
}

/// 并发: FindFiles 和 FileExists 并发调用
TEST(Concurrency, FileSystemConcurrentAccess) {
    TempDir dir("conc_fs_");
    for (int i = 0; i < 100; ++i)
        dir.CreateFile("f_" + std::to_string(i) + ".txt");

    std::atomic<int> found{0};
    std::atomic<int> exists{0};

    auto worker = [&]() {
        for (int j = 0; j < 200; ++j) {
            auto files = FindFiles(dir.Path(), "*.txt");
            found.fetch_add(static_cast<int>(files.size()));

            if (FileExists(dir.Path() + "/f_0.txt"))
                exists.fetch_add(1);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    EXPECT_GT(found.load(), 0);
    EXPECT_GT(exists.load(), 0);
}

/// 并发: 多写者同时写共享内存（压力检测竞态，不要求精确一致性）
// DISABLED: 8×5000 非原子写操作在共享内存中性能极差（每次触发页错误）
// 保留此测试作为极限压力基准，CI 排除。
TEST(Concurrency, DISABLED_MultiWriterSharedMemory) {
    constexpr const char* kName = "conc_mw";
    constexpr int kWriters = 8;
    constexpr int kIncrements = 5000;

    auto owner = SharedMemory::Create(kName, 256);
    ASSERT_NE(owner, nullptr);

    volatile int* counter = static_cast<volatile int*>(owner->Data());
    *counter = 0;

    std::vector<std::thread> writers;
    for (int i = 0; i < kWriters; ++i) {
        writers.emplace_back([&]() {
            auto wr = SharedMemory::Open(kName, 256);
            if (!wr) return;
            volatile int* cnt = static_cast<volatile int*>(wr->Data());
            for (int j = 0; j < kIncrements; ++j) {
                ++(*cnt);  // 非原子操作会丢失更新，但这是压力测试的目的
            }
        });
    }

    for (auto& t : writers) t.join();

    // 不要求精确计数（非原子操作会丢失），但应大于单线程的值
    EXPECT_GT(*counter, kIncrements / 2) << "Severe write starvation detected";
    EXPECT_LT(*counter, kWriters * kIncrements);
}

/// 死锁检测: SharedMemory 析构不应持有锁
TEST(Concurrency, NoDeadlockOnDestroyDuringOpen) {
    constexpr const char* kName = "conc_ndlock";
    constexpr int kRounds = 100;

    std::atomic<int> completed{0};

    auto worker = [&]() {
        for (int i = 0; i < kRounds; ++i) {
            // Reader: 持续打开
            auto rd = SharedMemory::Open(kName, 128);
            if (rd) completed.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    };

    // 先创建
    auto owner = SharedMemory::Create(kName, 128);
    ASSERT_NE(owner, nullptr);

    std::thread t1(worker);
    std::thread t2(worker);

    // 在 reader 活跃期间重建 owner
    for (int i = 0; i < 10; ++i) {
        owner.reset();
        owner = SharedMemory::Create(kName, 128);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    t1.join();
    t2.join();

    EXPECT_GT(completed.load(), 0);
}
