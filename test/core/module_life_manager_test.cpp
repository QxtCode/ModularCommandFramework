/// ModuleLifeManager unit tests — registration, dispatch, DLL injection

#include <gtest/gtest.h>
#include <memory>
#include "core/ModuleLifeManager.h"
#include "core/ParmarPack.h"
#include "mocks/mock_module.h"

class MgrTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        mgr = &ModuleLifeManager::GetInstance();
    }

    ModuleLifeManager* mgr = nullptr;
};

// ---- 注册 ----

TEST_F(MgrTest, CanAddModule)
{
    auto m = std::make_unique<MockModule>("test_add");
    bool ok = mgr->AddModule(std::move(m));
    EXPECT_TRUE(ok);
}

TEST_F(MgrTest, CannotAddNull)
{
    EXPECT_FALSE(mgr->AddModule(nullptr));
}

TEST_F(MgrTest, CannotAddDuplicateName)
{
    mgr->AddModule(std::make_unique<MockModule>("dup"));
    EXPECT_FALSE(mgr->AddModule(std::make_unique<MockModule>("dup")));
}

TEST_F(MgrTest, CannotAddFailingInit)
{
    auto m = std::make_unique<MockModule>("fail");
    m->init_result = false;
    EXPECT_FALSE(mgr->AddModule(std::move(m)));
}

TEST_F(MgrTest, OnInitIsCalled)
{
    auto m = std::make_unique<MockModule>("init_test");
    MockModule* raw = m.get();
    mgr->AddModule(std::move(m));
    EXPECT_TRUE(raw->init_called);
}

// ---- Dispatch ----

TEST_F(MgrTest, DispatchToExistingModule)
{
    auto m = std::make_unique<MockModule>("dispatch_test");
    MockModule* raw = m.get();
    mgr->AddModule(std::move(m));

    ParmarPack pack;
    pack.mod_id  = "dispatch_test";
    pack.func_id = "1";

    bool ok = mgr->Dispatch(&pack);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(raw->exec_called);
    EXPECT_EQ(raw->last_func_id, "1");
}

TEST_F(MgrTest, DispatchToUnknownModuleFails)
{
    ParmarPack pack;
    pack.mod_id = "ghost";

    bool ok = mgr->Dispatch(&pack);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(pack.success);
    EXPECT_EQ(pack.error.code, 404);
}

TEST_F(MgrTest, DispatchWithNullPackPrintsModules)
{
    // Should not crash
    EXPECT_NO_THROW(mgr->Dispatch(nullptr));
}

TEST_F(MgrTest, HelpShowsModules)
{
    ParmarPack pack;
    pack.mod_id = "help";
    EXPECT_NO_THROW(mgr->Dispatch(&pack));
}

// ---- 查询 ----

TEST_F(MgrTest, GetModuleCount)
{
    size_t before = mgr->GetModuleCount();
    mgr->AddModule(std::make_unique<MockModule>("count_a"));
    EXPECT_EQ(mgr->GetModuleCount(), before + 1);
    mgr->AddModule(std::make_unique<MockModule>("count_b"));
    EXPECT_EQ(mgr->GetModuleCount(), before + 2);
}

TEST_F(MgrTest, GetModuleByName)
{
    mgr->AddModule(std::make_unique<MockModule>("findme"));
    EXPECT_NE(mgr->GetModule("findme"), nullptr);
    EXPECT_EQ(mgr->GetModule("nope"), nullptr);
}

// ---- 单例 ----

TEST_F(MgrTest, IsSingleton)
{
    auto& a = ModuleLifeManager::GetInstance();
    auto& b = ModuleLifeManager::GetInstance();
    EXPECT_EQ(&a, &b);
}
