/// DLL module comprehensive tests
/// Load / Unload / Execute / Edge cases / Dangling pointer safety

#include <gtest/gtest.h>
#include <string>
#include "core/ModuleLifeManager.h"
#include "core/Task.h"
#include "core/ParmarPack.h"
#include "event_bus/event_bus.h"
#include "mocks/mock_module.h"

#ifdef _WIN32
#include <windows.h>
#endif

// ============================================================
//  DLL 计数器访问辅助
// ============================================================

using GetIntFn  = int  (*)();
using GetVoidFn = void (*)();
using SetBoolFn = void (*)(bool);

static HMODULE GetTestDLLHandle()
{
#ifdef _WIN32
    HMODULE h = GetModuleHandleA("TestDLLModule.dll");
    if (!h) h = LoadLibraryA("TestDLLModule.dll");
    return h;
#else
    return nullptr;
#endif
}

static int ReadCounter(const char* name)
{
#ifdef _WIN32
    HMODULE h = GetTestDLLHandle();
    auto fn = h ? (GetIntFn)GetProcAddress(h, name) : nullptr;
    return fn ? fn() : -1;
#else
    return -1;
#endif
}

static void CallVoid(const char* name)
{
#ifdef _WIN32
    HMODULE h = GetTestDLLHandle();
    auto fn = h ? (GetVoidFn)GetProcAddress(h, name) : nullptr;
    if (fn) fn();
#endif
}

static void CallSetBool(const char* name, bool val)
{
#ifdef _WIN32
    HMODULE h = GetTestDLLHandle();
    auto fn = h ? (SetBoolFn)GetProcAddress(h, name) : nullptr;
    if (fn) fn(val);
#endif
}

// ============================================================
//  Test fixture
// ============================================================

class DLLModuleTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        mgr = &ModuleLifeManager::GetInstance();
        bus = &EventBus::GetInstance();
        CallVoid("ResetTestDLLCounters");
    }

    void TearDown() override
    {
        mgr->UnloadModule("TestDLL");
    }

    // 加载 DLL 模块
    bool LoadDLL()
    {
        return mgr->LoadDLLModule("TestDLLModule.dll");
    }

    ModuleLifeManager* mgr = nullptr;
    EventBus* bus = nullptr;
};

// ============================================================
//  1. 基本加载 / 卸载
// ============================================================

TEST_F(DLLModuleTest, LoadDLLSucceeds)
{
    ASSERT_TRUE(LoadDLL());
    EXPECT_NE(mgr->GetModule("TestDLL"), nullptr);
    EXPECT_GE(ReadCounter("GetTestDLLInitCount"), 1);
}

TEST_F(DLLModuleTest, UnloadCallsShutdownAndRemoves)
{
    ASSERT_TRUE(LoadDLL());

    size_t before = mgr->GetModuleCount();
    EXPECT_EQ(ReadCounter("GetTestDLLShutdownCount"), 0);

    EXPECT_TRUE(mgr->UnloadModule("TestDLL"));
    EXPECT_GE(ReadCounter("GetTestDLLShutdownCount"), 1);
    EXPECT_EQ(mgr->GetModuleCount(), before - 1);
    EXPECT_EQ(mgr->GetModule("TestDLL"), nullptr);
}

// ============================================================
//  2. 执行验证
// ============================================================

TEST_F(DLLModuleTest, DispatchToDLLModule)
{
    ASSERT_TRUE(LoadDLL());

    ParmarPack pack;
    pack.mod_id  = "TestDLL";
    pack.func_id = "ping";
    pack.params["msg"].push_back("hello_dll");

    EXPECT_TRUE(mgr->Dispatch(&pack));
    EXPECT_TRUE(pack.success);
    EXPECT_EQ(pack.return_value, "hello_dll");
    EXPECT_GE(ReadCounter("GetTestDLLExecCount"), 1);
}

TEST_F(DLLModuleTest, DispatchViaTaskAndBus)
{
    ASSERT_TRUE(LoadDLL());

    auto pack = std::make_unique<ParmarPack>();
    ParmarPack* raw = pack.get();
    raw->mod_id  = "TestDLL";
    raw->func_id = "ping";
    raw->params["msg"].push_back("via_bus");

    Task task;
    task.Assign(std::move(pack));
    task.Step(*bus);

    EXPECT_TRUE(raw->success);
    EXPECT_EQ(raw->return_value, "via_bus");
}

TEST_F(DLLModuleTest, HelpOnDLLModule)
{
    ASSERT_TRUE(LoadDLL());

    ParmarPack pack;
    pack.mod_id  = "TestDLL";
    pack.func_id = "help";
    EXPECT_NO_THROW(mgr->Dispatch(&pack));
}

// ============================================================
//  3. 边界情况
// ============================================================

TEST_F(DLLModuleTest, LoadNonexistentDLL)
{
    EXPECT_FALSE(mgr->LoadDLLModule("ghost_xyz.dll"));
}

TEST_F(DLLModuleTest, DuplicateModuleRejected)
{
    ASSERT_TRUE(LoadDLL());
    EXPECT_FALSE(mgr->LoadDLLModule("TestDLLModule.dll"));
}

TEST_F(DLLModuleTest, OnInitFailureNotRegistered)
{
    _putenv("TESTDLL_ONINIT_FAIL=1");
    EXPECT_FALSE(LoadDLL());
    EXPECT_EQ(mgr->GetModule("TestDLL"), nullptr);
    _putenv("TESTDLL_ONINIT_FAIL=0");
}

// ============================================================
//  4. 卸载安全
// ============================================================

TEST_F(DLLModuleTest, DispatchAfterUnloadReturns404)
{
    ASSERT_TRUE(LoadDLL());
    mgr->UnloadModule("TestDLL");

    ParmarPack pack;
    pack.mod_id = "TestDLL";
    EXPECT_FALSE(mgr->Dispatch(&pack));
    EXPECT_EQ(pack.error.code, 404);
}

TEST_F(DLLModuleTest, UnloadNonexistent)
{
    EXPECT_FALSE(mgr->UnloadModule("never_loaded"));
}

TEST_F(DLLModuleTest, DoubleUnloadSafe)
{
    ASSERT_TRUE(LoadDLL());
    EXPECT_TRUE(mgr->UnloadModule("TestDLL"));
    EXPECT_FALSE(mgr->UnloadModule("TestDLL"));
}

TEST_F(DLLModuleTest, ModulePointerAfterUnload)
{
    ASSERT_TRUE(LoadDLL());
    mgr->UnloadModule("TestDLL");
    EXPECT_EQ(mgr->GetModule("TestDLL"), nullptr);
}

// ============================================================
//  5. 内置模块共存
// ============================================================

TEST_F(DLLModuleTest, CoexistsWithBuiltin)
{
    mgr->AddModule(std::make_unique<MockModule>("builtin_test"));
    size_t before = mgr->GetModuleCount();

    ASSERT_TRUE(LoadDLL());
    EXPECT_EQ(mgr->GetModuleCount(), before + 1);
    EXPECT_NE(mgr->GetModule("builtin_test"), nullptr);
    EXPECT_NE(mgr->GetModule("TestDLL"), nullptr);

    mgr->UnloadModule("TestDLL");
    EXPECT_EQ(mgr->GetModuleCount(), before);
}
