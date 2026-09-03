/// =================================================================
///  SDK 模块 + 宿主接线 集成测试（v2.8 接线反转验收）
/// =================================================================
///  证明：纯 SDK 基类 ModuleBase（不含 ConnectToEventBus）注册进框架后，
///  宿主通过 IModule::ForEachFunc 把命令自动接成 EventBus 信号，命令可被
///  Emit（worker 路径）/ Dispatch（直连路径）触发。
///  这是「接线从模块侧搬到宿主侧」的关键验收点。
///
///  测试维度：假模块（PureSdkModule）+ 边界（卸载后信号清零）。

#include <gtest/gtest.h>
#include <atomic>
#include <memory>

#include "core/ModuleLifeManager.h"
#include "event_bus/event_bus.h"

// 纯 SDK 模块：只继承 ModuleBase，不碰 ModuleBaseObject / ConnectToEventBus
class PureSdkModule : public ModuleBase
{
public:
    const char* GetName() const override { return "PureSdk"; }

    bool OnInit() override
    {
        REGISTER_FUNC("double", "x*2 (-v:x|21)", {
            int x = pack->GetAsOr<int>("x", 0);
            exec_count.fetch_add(1);
            SetResult(pack, std::to_string(x * 2));
        });
        return true;
    }

    std::atomic<int> exec_count{0};
};

class SdkWiringTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ModuleLifeManager::GetInstance().UnloadModule("PureSdk");  // 清残留（幂等）
    }
    void TearDown() override
    {
        ModuleLifeManager::GetInstance().UnloadModule("PureSdk");
    }
};

TEST_F(SdkWiringTest, AddModuleWiresCommandsViaHostForEachFunc)
{
    auto mod = std::make_unique<PureSdkModule>();
    PureSdkModule* raw = mod.get();
    ASSERT_TRUE(ModuleLifeManager::GetInstance().AddModule(std::move(mod)));

    // 命令已被宿主接线成 "PureSdk.double" 信号（宿主 ForEachFunc 完成，非 ConnectToEventBus）
    auto& bus = EventBus::GetInstance();
    EXPECT_NE(bus.GetSlotCount("PureSdk.double"), 0u);

    // 通过 EventBus 触发 —— 等价 worker 的 bus.Emit 路径
    ParmarPack pack;
    pack.func_id = "double";
    pack.params["x"] = {"21"};
    ASSERT_TRUE(bus.Emit<ParmarPack*>("PureSdk.double", &pack));

    EXPECT_EQ(raw->exec_count.load(), 1);
    EXPECT_TRUE(pack.success);
    EXPECT_EQ(pack.return_value, "42");
}

TEST_F(SdkWiringTest, DispatchReachesPureSdkModule)
{
    auto mod = std::make_unique<PureSdkModule>();
    PureSdkModule* raw = mod.get();
    ASSERT_TRUE(ModuleLifeManager::GetInstance().AddModule(std::move(mod)));

    ParmarPack pack;
    pack.mod_id   = "PureSdk";
    pack.func_id  = "double";
    pack.params["x"] = {"5"};

    ASSERT_TRUE(ModuleLifeManager::GetInstance().Dispatch(&pack));
    EXPECT_EQ(raw->exec_count.load(), 1);
    EXPECT_EQ(pack.return_value, "10");
}

TEST_F(SdkWiringTest, UnloadRemovesWiredSignals)
{
    ASSERT_TRUE(ModuleLifeManager::GetInstance().AddModule(std::make_unique<PureSdkModule>()));
    auto& bus = EventBus::GetInstance();
    EXPECT_NE(bus.GetSlotCount("PureSdk.double"), 0u);

    ASSERT_TRUE(ModuleLifeManager::GetInstance().UnloadModule("PureSdk"));
    EXPECT_EQ(bus.GetSlotCount("PureSdk.double"), 0u);  // 信号已随卸载移除
}
