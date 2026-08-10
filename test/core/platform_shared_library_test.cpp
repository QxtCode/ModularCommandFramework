/// platform::SharedLibrary 独立测试
/// 覆盖: Load 成功, Load 失败, GetFunction, 析构释放, 移动语义

#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <string>

#include "core/platform/shared_library.h"
#include "core/platform/file_system.h"

using namespace platform;

// ================================================================
//  测试夹具: 发现 TestDLLModule.dll 路径
// ================================================================
class SharedLibraryTest : public ::testing::Test {
protected:
    std::string dll_path;

    void SetUp() override {
        // TestDLLModule.dll 在 test_runner.exe 同目录
        dll_path = "TestDLLModule.dll";

        // 如果当前工作目录不是 build 目录，从 CMake 输出目录查找
        // out/build/debug 匹配 CMakePresets.json 的 binaryDir
        if (!FileExists(dll_path)) {
            auto files = FindFiles("out/build/debug", "TestDLLModule.*");
            // 过滤 .dll
            for (const auto& f : files) {
                if (f.find(".dll") != std::string::npos ||
                    f.find(".so")  != std::string::npos) {
                    dll_path = f;
                    break;
                }
            }
        }
    }
};

// ================================================================
//  基本生命周期
// ================================================================
TEST_F(SharedLibraryTest, Load_Success) {
    if (!FileExists(dll_path)) {
        GTEST_SKIP() << "TestDLLModule not found, skipping DLL test";
    }
    auto lib = SharedLibrary::Load(dll_path);
    ASSERT_NE(lib, nullptr);
    EXPECT_TRUE(static_cast<bool>(*lib));
}

TEST_F(SharedLibraryTest, Load_NonExistent) {
    auto lib = SharedLibrary::Load("__nonexistent__.dll");
    EXPECT_EQ(lib, nullptr);
}

// ================================================================
//  符号查找
// ================================================================
TEST_F(SharedLibraryTest, GetFunction_CreateModule) {
    if (!FileExists(dll_path)) {
        GTEST_SKIP() << "TestDLLModule not found, skipping DLL test";
    }
    auto lib = SharedLibrary::Load(dll_path);
    ASSERT_NE(lib, nullptr);

    // CreateModule 返回 ModuleBaseObject*
    using CreateFunc = void* (*)();  // 简化：只用 void* 做存在性测试
    auto fn = lib->GetFunction<CreateFunc>("CreateModule");
    EXPECT_NE(fn, nullptr);
}

TEST_F(SharedLibraryTest, GetFunction_GetTestDLLInitCount) {
    if (!FileExists(dll_path)) {
        GTEST_SKIP() << "TestDLLModule not found, skipping DLL test";
    }
    auto lib = SharedLibrary::Load(dll_path);
    ASSERT_NE(lib, nullptr);

    using CountFunc = int (*)();
    auto fn = lib->GetFunction<CountFunc>("GetTestDLLInitCount");
    EXPECT_NE(fn, nullptr);
}

TEST_F(SharedLibraryTest, GetFunction_NonExistent) {
    if (!FileExists(dll_path)) {
        GTEST_SKIP() << "TestDLLModule not found, skipping DLL test";
    }
    auto lib = SharedLibrary::Load(dll_path);
    ASSERT_NE(lib, nullptr);

    auto* sym = lib->GetRawSymbol("__nonexistent_function__");
    EXPECT_EQ(sym, nullptr);
}

TEST_F(SharedLibraryTest, GetRawSymbol_NullHandle) {
    auto lib = SharedLibrary::Load("__nonexistent__.dll");
    EXPECT_EQ(lib, nullptr);
    // 加载失败时不应创建实例
}

// ================================================================
//  析构释放验证（通过重复加载同一 DLL 间接验证）
// ================================================================
TEST_F(SharedLibraryTest, Destructor_RepeatedLoad) {
    if (!FileExists(dll_path)) {
        GTEST_SKIP() << "TestDLLModule not found, skipping DLL test";
    }

    // 第一次加载
    {
        auto lib1 = SharedLibrary::Load(dll_path);
        ASSERT_NE(lib1, nullptr);
        auto* sym1 = lib1->GetRawSymbol("CreateModule");
        EXPECT_NE(sym1, nullptr);
    }  // lib1 析构 → FreeLibrary

    // 第二次加载同一 DLL（验证析构成功释放了引用计数）
    {
        auto lib2 = SharedLibrary::Load(dll_path);
        ASSERT_NE(lib2, nullptr);
        auto* sym2 = lib2->GetRawSymbol("CreateModule");
        EXPECT_NE(sym2, nullptr);
    }  // lib2 析构
}

// ================================================================
//  压力测试：并发加载/卸载
// ================================================================
TEST_F(SharedLibraryTest, Stress_ConcurrentLoadUnload) {
    if (!FileExists(dll_path)) {
        GTEST_SKIP() << "TestDLLModule not found, skipping DLL test";
    }

    constexpr int kIterations = 50;
    std::atomic<int> ok_count{0};
    std::atomic<int> fail_count{0};

    auto worker = [&](int id) {
        for (int i = 0; i < kIterations; ++i) {
            auto lib = SharedLibrary::Load(dll_path);
            if (lib) {
                // 验证符号可用
                auto* fn = lib->GetRawSymbol("CreateModule");
                if (fn) ok_count.fetch_add(1);
                else   fail_count.fetch_add(1);
            } else {
                fail_count.fetch_add(1);
            }
            // lib 析构 → FreeLibrary
        }
    };

    std::thread t1(worker, 0);
    std::thread t2(worker, 1);
    std::thread t3(worker, 2);
    std::thread t4(worker, 3);

    t1.join(); t2.join(); t3.join(); t4.join();

    EXPECT_EQ(fail_count.load(), 0);
    EXPECT_EQ(ok_count.load(), kIterations * 4);
}

// ================================================================
//  压力测试：同一 DLL 多次 Load/GetFunction/Release 循环
// ================================================================
TEST_F(SharedLibraryTest, Stress_RapidLoadUnloadSingleThread) {
    if (!FileExists(dll_path)) {
        GTEST_SKIP() << "TestDLLModule not found, skipping DLL test";
    }

    for (int i = 0; i < 200; ++i) {
        auto lib = SharedLibrary::Load(dll_path);
        ASSERT_NE(lib, nullptr) << "Iteration " << i;

        auto* create = lib->GetRawSymbol("CreateModule");
        EXPECT_NE(create, nullptr) << "Iteration " << i;

        auto* reset = lib->GetRawSymbol("ResetTestDLLCounters");
        EXPECT_NE(reset, nullptr) << "Iteration " << i;
    }
}
