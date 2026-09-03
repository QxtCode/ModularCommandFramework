/// =================================================================
///  ModuleBase / sdk.h umbrella 隔离测试
/// =================================================================
///  只 include "sdk/sdk.h"（伞头）。不碰 EventBus / core / ModuleLifeManager。
///  证明：一个 include 到位，继承 ModuleBase + REGISTER_FUNC + 收尾助手
///  即可自洽运行 —— SDK 基类层可独立于宿主使用。
///
///  测试维度：假模块（FakeMod）+ 边界（SetError / 空值 / 枚举）。

#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "sdk/sdk.h"

// ---- 假模块：完全用 SDK 伞头写，不依赖 core / eventbus ----
class FakeMod : public ModuleBase
{
public:
    const char* GetName() const override { return "Fake"; }

    bool OnInit() override
    {
        REGISTER_FUNC("double", "x*2 (-v:x|21)", {
            int x = pack->GetAsOr<int>("x", 0);
            SetResult(pack, std::to_string(x * 2));
        });
        REGISTER_FUNC("boom", "always fail", {
            SetError(pack, ErrorCode::INTERNAL_ERROR, "boom");
        });
        return true;
    }
};

TEST(ModuleBaseTest, SetResultWritesSuccessAndReturnValue)
{
    FakeMod m;
    m.OnInit();

    ParmarPack p;
    p.func_id = "double";
    p.params["x"] = {"21"};
    m.Execute(&p);

    EXPECT_TRUE(p.success);
    EXPECT_EQ(p.return_value, "42");
}

TEST(ModuleBaseTest, SetErrorWritesError)
{
    FakeMod m;
    m.OnInit();

    ParmarPack p;
    p.func_id = "boom";
    m.Execute(&p);

    EXPECT_FALSE(p.success);
    EXPECT_EQ(p.error.code, ErrorCode::INTERNAL_ERROR);
    EXPECT_EQ(p.error.message, "boom");
}

TEST(ModuleBaseTest, SetResultWithNullPackDoesNotCrash)
{
    EXPECT_NO_THROW(ModuleBase::SetResult(nullptr, "x"));
    EXPECT_NO_THROW(ModuleBase::SetError(nullptr, 1, "y"));
}

TEST(ModuleBaseTest, UmbrellaHeaderProvidesForEachFunc)
{
    FakeMod m;
    m.OnInit();

    std::vector<std::string> ids;
    m.ForEachFunc([&](const std::string& id, const std::string&) { ids.push_back(id); });

    EXPECT_EQ(ids.size(), 2u);  // "double" + "boom"
}

TEST(ModuleBaseTest, IsIModuleDerived)
{
    FakeMod m;
    IModule* base = &m;  // 向上转型成立，证明继承链正确
    EXPECT_EQ(std::string(base->GetName()), "Fake");
}
