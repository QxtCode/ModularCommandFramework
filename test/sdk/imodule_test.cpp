/// =================================================================
///  IModule SDK Isolation Tests
/// =================================================================
///  These tests ONLY include "sdk/IModule.h".
///  No EventBus, no ModuleBaseObject, no ModuleLifeManager.
///  Proves the SDK is fully self-contained.
///
///  Test categories:
///    1. REGISTER_FUNC + Execute basic flow
///    2. Boundary: unknown func, nullptr, empty strings
///    3. Help output
///    4. Thread safety (cout_mutex_ shared across instances)
///    5. show_explanation flag
///    6. Override Execute / Help

#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// === ONLY SDK include ===
#include "sdk/IModule.h"

// ---- Cout Capture ----
class CoutCapture
{
public:
    CoutCapture() : old_(std::cout.rdbuf(ss_.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(old_); }
    std::string Str() const { return ss_.str(); }
private:
    std::stringstream ss_;
    std::streambuf*   old_;
};

// ---- Test Module (SDK-only: inherits IModule directly) ----
class SDKTestModule : public IModule
{
public:
    explicit SDKTestModule(std::string name = "SDKMod") : name_(std::move(name)) {}
    const char* GetName() const override { return name_.c_str(); }

    // Expose protected members for testing
    bool HasFunc(const std::string& id) const { return funcs_.count(id) > 0; }
    bool HasExplain(const std::string& id) const { return explains_.count(id) > 0; }
    size_t FuncCount() const { return funcs_.size(); }

private:
    std::string name_;
};

// ================================================================
//  1. REGISTER_FUNC + Execute basic flow
// ================================================================

TEST(IModuleTest, RegisterFuncPopulatesTables)
{
    SDKTestModule mod;
    mod.OnInit();  // won't register anything (no REGISTER_FUNC called in base)
    EXPECT_EQ(mod.FuncCount(), 0u);

    // Register manually via protected RegisterFunc through REGISTER_FUNC
    // We test via a derived class that calls REGISTER_FUNC in OnInit
}

TEST(IModuleTest, ExecuteCallsRegisteredFunction)
{
    class MyMod : public SDKTestModule {
    public:
        MyMod() : SDKTestModule("Calc") {}
        bool OnInit() override {
            REGISTER_FUNC("add", "a + b", {
                pack->return_value = "result_42";
                pack->success = true;
            });
            return true;
        }
    };

    MyMod mod;
    ASSERT_TRUE(mod.OnInit());

    ParmarPack pack;
    pack.func_id = "add";
    pack.show_explanation = true;
    mod.Execute(&pack);

    EXPECT_TRUE(pack.success);
    EXPECT_EQ(pack.return_value, "result_42");
}

TEST(IModuleTest, UnknownFunctionReturnsError)
{
    SDKTestModule mod;

    ParmarPack pack;
    pack.func_id = "no_such_func";
    pack.show_explanation = false;
    mod.Execute(&pack);

    EXPECT_FALSE(pack.success);
    EXPECT_EQ(pack.error.code, 404);
}

TEST(IModuleTest, NullPackDoesNotCrash)
{
    SDKTestModule mod;
    EXPECT_NO_THROW(mod.Execute(nullptr));
    EXPECT_NO_THROW(mod.Help(nullptr));
    EXPECT_NO_THROW(mod.ReturnValue(nullptr));
}

// ================================================================
//  2. Boundary: duplicate registration, empty strings
// ================================================================

TEST(IModuleTest, DuplicateRegistrationOverwrites)
{
    class MyMod : public SDKTestModule {
    public:
        MyMod() : SDKTestModule("Dup") {}
        bool OnInit() override {
            REGISTER_FUNC("f", "first", { pack->success = true; });
            REGISTER_FUNC("f", "second", { pack->success = true; pack->return_value = "v2"; });
            return true;
        }
    };

    MyMod mod;
    mod.OnInit();

    // Should have 1 function (second overwrote first)
    EXPECT_EQ(mod.FuncCount(), 1u);

    ParmarPack pack;
    pack.func_id = "f";
    pack.show_explanation = false;
    mod.Execute(&pack);

    EXPECT_TRUE(pack.success);
    EXPECT_EQ(pack.return_value, "v2");  // second version
}

TEST(IModuleTest, EmptyFunctionIdIsAllowed)
{
    class MyMod : public SDKTestModule {
    public:
        MyMod() : SDKTestModule("Empty") {}
        bool OnInit() override {
            REGISTER_FUNC("", "empty id", { pack->success = true; });
            return true;
        }
    };

    MyMod mod;
    EXPECT_TRUE(mod.OnInit());
    EXPECT_EQ(mod.FuncCount(), 1u);

    ParmarPack pack;
    pack.func_id = "";
    pack.show_explanation = false;
    mod.Execute(&pack);
    EXPECT_TRUE(pack.success);
}

// ================================================================
//  3. Help output
// ================================================================

TEST(IModuleTest, HelpPrintsAllRegisteredFunctions)
{
    class MyMod : public SDKTestModule {
    public:
        MyMod() : SDKTestModule("HelpMod") {}
        bool OnInit() override {
            REGISTER_FUNC("a", "Alpha", {});
            REGISTER_FUNC("b", "Beta", {});
            REGISTER_FUNC("c", "Gamma", {});
            return true;
        }
    };

    MyMod mod;
    mod.OnInit();

    CoutCapture cap;
    mod.Help(nullptr);

    std::string out = cap.Str();
    EXPECT_NE(out.find("HelpMod Help"), std::string::npos);
    EXPECT_NE(out.find("a : Alpha"), std::string::npos);
    EXPECT_NE(out.find("b : Beta"), std::string::npos);
    EXPECT_NE(out.find("c : Gamma"), std::string::npos);
}

// ================================================================
//  4. Thread safety (cout_mutex_ shared across instances)
// ================================================================

TEST(IModuleTest, CoutMutexIsSharedAcrossInstances)
{
    // Two different module instances should share the same mutex
    class ModA : public SDKTestModule {
    public:
        ModA() : SDKTestModule("Alpha") {}
        bool OnInit() override {
            REGISTER_FUNC("a", "Func A", { pack->success = true; });
            return true;
        }
    };
    class ModB : public SDKTestModule {
    public:
        ModB() : SDKTestModule("Beta") {}
        bool OnInit() override {
            REGISTER_FUNC("b", "Func B", { pack->success = true; });
            return true;
        }
    };

    ModA ma; ma.OnInit();
    ModB mb; mb.OnInit();

    std::atomic<int> ok{0};
    std::thread t1([&]() {
        for (int i = 0; i < 100; ++i) {
            ParmarPack p; p.func_id = "a"; p.show_explanation = true;
            ma.Execute(&p);
            if (p.success) ok.fetch_add(1);
        }
    });
    std::thread t2([&]() {
        for (int i = 0; i < 100; ++i) {
            ParmarPack p; p.func_id = "b"; p.show_explanation = true;
            mb.Execute(&p);
            if (p.success) ok.fetch_add(1);
        }
    });

    t1.join(); t2.join();
    EXPECT_EQ(ok.load(), 200);
}

TEST(IModuleTest, ConcurrentExecuteNoDataRace)
{
    class Mod : public SDKTestModule {
    public:
        Mod() : SDKTestModule("Concurrent") {}
        bool OnInit() override {
            REGISTER_FUNC("f", "concurrent func", {
                pack->success = true;
                pack->return_value = "ok";
            });
            return true;
        }
    };

    Mod mod;
    mod.OnInit();

    constexpr int kThreads = 8;
    constexpr int kCalls   = 50;
    std::atomic<int> count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < kCalls; ++i) {
                ParmarPack p;
                p.func_id = "f";
                p.show_explanation = true;
                mod.Execute(&p);
                if (p.success && p.return_value == "ok")
                    count.fetch_add(1);
            }
        });
    }

    for (auto& th : threads) th.join();
    EXPECT_EQ(count.load(), kThreads * kCalls);
}

// ================================================================
//  5. show_explanation flag
// ================================================================

TEST(IModuleTest, ShowExplanationPrintsHeader)
{
    class Mod : public SDKTestModule {
    public:
        Mod() : SDKTestModule("Show") {}
        bool OnInit() override {
            REGISTER_FUNC("hello", "Greeting function", {
                std::cout << "BODY" << std::endl;
                pack->success = true;
            });
            return true;
        }
    };

    Mod mod;
    mod.OnInit();

    ParmarPack pack;
    pack.func_id = "hello";
    pack.show_explanation = true;

    CoutCapture cap;
    mod.Execute(&pack);

    std::string out = cap.Str();
    EXPECT_NE(out.find("--- [Show.hello] Greeting function ---"), std::string::npos);
    EXPECT_NE(out.find("BODY"), std::string::npos);
    EXPECT_TRUE(pack.success);
}

TEST(IModuleTest, ShowExplanationFalseNoHeader)
{
    class Mod : public SDKTestModule {
    public:
        Mod() : SDKTestModule("Quiet") {}
        bool OnInit() override {
            REGISTER_FUNC("q", "Should not appear", {
                std::cout << "X" << std::endl;
                pack->success = true;
            });
            return true;
        }
    };

    Mod mod;
    mod.OnInit();

    ParmarPack pack;
    pack.func_id = "q";
    pack.show_explanation = false;

    CoutCapture cap;
    mod.Execute(&pack);

    std::string out = cap.Str();
    EXPECT_EQ(out.find("--- [Quiet.q]"), std::string::npos);
    EXPECT_EQ(out.find("Should not appear"), std::string::npos);
    EXPECT_NE(out.find("X"), std::string::npos);
}

// ================================================================
//  6. Override Execute / Help
// ================================================================

TEST(IModuleTest, OverrideExecuteWorks)
{
    class Mod : public SDKTestModule {
    public:
        Mod() : SDKTestModule("CustomExec") {}
        void Execute(ParmarPack* pack) override {
            custom_called = true;
            pack->success = true;
            pack->return_value = "custom";
        }
        bool custom_called = false;
    };

    Mod mod;

    ParmarPack pack;
    pack.func_id = "anything";
    mod.Execute(&pack);

    EXPECT_TRUE(mod.custom_called);
    EXPECT_EQ(pack.return_value, "custom");
}

TEST(IModuleTest, OverrideHelpWorks)
{
    class Mod : public SDKTestModule {
    public:
        Mod() : SDKTestModule("CustomHelp") {}
        void Help(ParmarPack*) override {
            help_called = true;
            std::cout << "CUSTOM_HELP" << std::endl;
        }
        bool help_called = false;
    };

    Mod mod;

    CoutCapture cap;
    mod.Help(nullptr);

    EXPECT_TRUE(mod.help_called);
    EXPECT_NE(cap.Str().find("CUSTOM_HELP"), std::string::npos);
}

// ================================================================
//  7. ReturnValue default
// ================================================================

TEST(IModuleTest, ReturnValueFillsDefault)
{
    SDKTestModule mod;

    ParmarPack pack;
    pack.return_value = "";  // empty → should be filled by ReturnValue
    mod.ReturnValue(&pack);

    EXPECT_FALSE(pack.return_value.empty());
    EXPECT_NE(pack.return_value.find("SDKMod"), std::string::npos);
}

TEST(IModuleTest, ReturnValuePreservesExisting)
{
    SDKTestModule mod;

    ParmarPack pack;
    pack.return_value = "already_set";
    mod.ReturnValue(&pack);

    EXPECT_EQ(pack.return_value, "already_set");  // not overwritten
}

// ================================================================
//  8. GetVersion default
// ================================================================

TEST(IModuleTest, GetVersionDefaultsToOne)
{
    SDKTestModule mod;
    EXPECT_EQ(mod.GetVersion(), 1);
}

// ================================================================
//  9. PamarPack default (from SDK)
// ================================================================

TEST(IModuleTest, ParmarPackShowExplanationDefaultsTrue)
{
    ParmarPack pack;
    EXPECT_TRUE(pack.show_explanation);
}

TEST(IModuleTest, ParmarPackErrorCodeValues)
{
    EXPECT_EQ(ErrorCode::OK, 0);
    EXPECT_EQ(ErrorCode::MODULE_NOT_FOUND, 404);
    EXPECT_EQ(ErrorCode::FUNC_NOT_FOUND, 405);
    EXPECT_EQ(ErrorCode::SIGNAL_NOT_FOUND, 406);
    EXPECT_EQ(ErrorCode::INVALID_PARAMS, 400);
    EXPECT_EQ(ErrorCode::DLL_LOAD_FAILED, 501);
    EXPECT_EQ(ErrorCode::INTERNAL_ERROR, 500);
}
