/// =================================================================
///  FuncExplainTest - unit tests for function explanation header
/// =================================================================
///  Tests that ModuleBaseObject::Execute() prints an explanation
///  header before invoking the registered function.
///
///  Isolation: no EventBus / ModuleLifeManager / Task dependency.
///  Uses direct funcs_/explains_ table manipulation and cout capture.
///  Includes multi-threading safety tests.

#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/ModuleBaseObject.h"
#include "core/ParmarPack.h"

// ================================================================
//  Test module (bypasses REGISTER_FUNC to avoid EventBus dependency)
// ================================================================
class ExplainTestModule : public ModuleBaseObject
{
public:
    const char* GetName() const override { return "TestMod"; }

    void AddFunc(const std::string& id, const std::string& desc,
                 std::function<void(ParmarPack*)> fn)
    {
        funcs_[id]    = std::move(fn);
        explains_[id] = desc;
    }

    void AddFuncNoDesc(const std::string& id, std::function<void(ParmarPack*)> fn)
    {
        funcs_[id] = std::move(fn);
        // deliberately skip explains_
    }
};

// ================================================================
//  cout capture helper
// ================================================================
class CoutCapture
{
public:
    CoutCapture() : old_buf_(std::cout.rdbuf(ss_.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(old_buf_); }
    std::string Get() const { return ss_.str(); }
private:
    std::stringstream ss_;
    std::streambuf*   old_buf_;
};

// ================================================================
//  Basic tests
// ================================================================

TEST(FuncExplainTest, PrintsExplanationHeader)
{
    ExplainTestModule mod;
    mod.AddFunc("greet", "Print greeting", [](ParmarPack* p) {
        std::cout << "hello world" << std::endl;
        p->success = true;
    });

    ParmarPack pack;
    pack.mod_id  = "TestMod";
    pack.func_id = "greet";
    pack.show_explanation = true;

    CoutCapture cap;
    mod.Execute(&pack);

    std::string output = cap.Get();
    EXPECT_NE(output.find("--- [TestMod.greet]"), std::string::npos);
    EXPECT_NE(output.find("Print greeting"), std::string::npos);
    EXPECT_NE(output.find("hello world"), std::string::npos);
    EXPECT_TRUE(pack.success);
}

TEST(FuncExplainTest, MissingDescriptionShowsFallback)
{
    ExplainTestModule mod;
    mod.AddFuncNoDesc("no_desc", [](ParmarPack* p) {
        std::cout << "no desc func" << std::endl;
        p->success = true;
    });

    ParmarPack pack;
    pack.mod_id  = "TestMod";
    pack.func_id = "no_desc";
    pack.show_explanation = true;

    CoutCapture cap;
    mod.Execute(&pack);

    EXPECT_NE(cap.Get().find("(no description)"), std::string::npos);
    EXPECT_TRUE(pack.success);
}

TEST(FuncExplainTest, ShowExplanationFalseSuppressesHeader)
{
    ExplainTestModule mod;
    mod.AddFunc("quiet", "Silent function", [](ParmarPack* p) {
        std::cout << "quiet output" << std::endl;
        p->success = true;
    });

    ParmarPack pack;
    pack.mod_id  = "TestMod";
    pack.func_id = "quiet";
    pack.show_explanation = false;

    CoutCapture cap;
    mod.Execute(&pack);

    std::string output = cap.Get();
    EXPECT_EQ(output.find("--- [TestMod.quiet]"), std::string::npos);
    EXPECT_EQ(output.find("Silent function"), std::string::npos);
    EXPECT_NE(output.find("quiet output"), std::string::npos);
    EXPECT_TRUE(pack.success);
}

TEST(FuncExplainTest, UnknownFunctionDoesNotPrintHeader)
{
    ExplainTestModule mod;
    mod.AddFunc("exists", "Existing function", [](ParmarPack* p) {
        p->success = true;
    });

    ParmarPack pack;
    pack.mod_id  = "TestMod";
    pack.func_id = "ghost_func";
    pack.show_explanation = true;

    CoutCapture cap;
    mod.Execute(&pack);

    EXPECT_EQ(cap.Get().find("--- [TestMod.ghost_func]"), std::string::npos);
    EXPECT_FALSE(pack.success);
    EXPECT_EQ(pack.error.code, 404);
}

TEST(FuncExplainTest, NullPackDoesNotCrash)
{
    ExplainTestModule mod;
    EXPECT_NO_THROW(mod.Execute(nullptr));
}

// ================================================================
//  Multi-threading safety tests
// ================================================================

TEST(FuncExplainTest, ConcurrentExecuteDoesNotCrash)
{
    ExplainTestModule mod;
    mod.AddFunc("concurrent", "Concurrent test", [](ParmarPack* p) {
        p->return_value = "done";
        p->success = true;
    });

    constexpr int kThreads = 8;
    constexpr int kCallsPerThread = 50;

    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&]() {
            for (int i = 0; i < kCallsPerThread; ++i)
            {
                ParmarPack pack;
                pack.mod_id  = "TestMod";
                pack.func_id = "concurrent";
                pack.show_explanation = true;
                mod.Execute(&pack);
                if (pack.success) success_count.fetch_add(1);
            }
        });
    }

    for (auto& th : threads) th.join();
    EXPECT_EQ(success_count.load(), kThreads * kCallsPerThread);
}

TEST(FuncExplainTest, ConcurrentExecuteDifferentModules)
{
    class NamedModule : public ExplainTestModule
    {
        std::string name_;
    public:
        explicit NamedModule(std::string n) : name_(std::move(n)) {}
        const char* GetName() const override { return name_.c_str(); }
    };

    auto mod1 = std::make_unique<NamedModule>("Alpha");
    auto mod2 = std::make_unique<NamedModule>("Beta");

    mod1->AddFunc("a", "Module A function", [](ParmarPack* p) { p->success = true; });
    mod2->AddFunc("b", "Module B function", [](ParmarPack* p) { p->success = true; });

    std::atomic<int> ok{0};
    std::thread t1([&]() {
        for (int i = 0; i < 100; ++i) {
            ParmarPack p;
            p.mod_id = "Alpha"; p.func_id = "a"; p.show_explanation = true;
            mod1->Execute(&p);
            if (p.success) ok.fetch_add(1);
        }
    });
    std::thread t2([&]() {
        for (int i = 0; i < 100; ++i) {
            ParmarPack p;
            p.mod_id = "Beta"; p.func_id = "b"; p.show_explanation = true;
            mod2->Execute(&p);
            if (p.success) ok.fetch_add(1);
        }
    });

    t1.join(); t2.join();
    EXPECT_EQ(ok.load(), 200);
}

// ================================================================
//  Default value & ordering tests
// ================================================================

TEST(FuncExplainTest, ShowExplanationDefaultsToTrue)
{
    ParmarPack pack;
    EXPECT_TRUE(pack.show_explanation);
}

TEST(FuncExplainTest, HeaderPrintedBeforeFunctionBody)
{
    ExplainTestModule mod;
    mod.AddFunc("order", "Order test", [](ParmarPack* p) {
        std::cout << "BODY_OUTPUT" << std::endl;
        p->success = true;
    });

    ParmarPack pack;
    pack.mod_id  = "TestMod";
    pack.func_id = "order";
    pack.show_explanation = true;

    CoutCapture cap;
    mod.Execute(&pack);

    std::string output = cap.Get();
    auto header_pos = output.find("--- [TestMod.order]");
    auto body_pos   = output.find("BODY_OUTPUT");
    ASSERT_NE(header_pos, std::string::npos);
    ASSERT_NE(body_pos, std::string::npos);
    EXPECT_LT(header_pos, body_pos);
}
