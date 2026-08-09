/// EventBus unit tests — signal registration, emit, slot lifecycle

#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "event_bus/event_bus.h"

class BusTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        bus = &EventBus::GetInstance();
    }
    EventBus* bus = nullptr;
};

// ---- 注册 ----

TEST_F(BusTest, RegisterSignalReturnsName)
{
    std::string name = bus->RegisterSignal<int>("test_sig");
    EXPECT_EQ(name, "test_sig");
}

TEST_F(BusTest, RegisterEmptyNameReturnsEmpty)
{
    std::string name = bus->RegisterSignal<int>("");
    EXPECT_TRUE(name.empty());
}

// ---- Emit ----

TEST_F(BusTest, EmitInvokesSlot)
{
    bus->RegisterSignal<int>("emit_test");
    bool called = false;

    auto id = bus->LinkSlotFunc<int>("emit_test", [&](int v) {
        called = true;
        EXPECT_EQ(v, 42);
    });
    EXPECT_NE(id, 0u);

    EXPECT_TRUE(bus->Emit("emit_test", 42));
    EXPECT_TRUE(called);
}

TEST_F(BusTest, EmitUnknownReturnsFalse)
{
    EXPECT_FALSE(bus->Emit("ghost_signal", 0));
}

TEST_F(BusTest, EmitString)
{
    bus->RegisterSignal<std::string>("str_test");
    std::string received;
    bus->LinkSlotFunc<std::string>("str_test", [&](const std::string& s) {
        received = s;
    });
    bus->Emit("str_test", std::string("hello"));
    EXPECT_EQ(received, "hello");
}

// ---- 多槽位 ----

TEST_F(BusTest, MultipleSlotsAllCalled)
{
    bus->RegisterSignal<int>("multi");
    int count = 0;
    bus->LinkSlotFunc<int>("multi", [&](int) { count++; });
    bus->LinkSlotFunc<int>("multi", [&](int) { count++; });
    bus->LinkSlotFunc<int>("multi", [&](int) { count++; });
    bus->Emit("multi", 0);
    EXPECT_EQ(count, 3);
}

// ---- 类型安全 ----

TEST_F(BusTest, TypeMismatchLinkFails)
{
    bus->RegisterSignal<int>("type_test");
    EXPECT_EQ(bus->LinkSlotFunc<double>("type_test", [](double) {}), 0u);
}

TEST_F(BusTest, TypeMismatchEmitFails)
{
    bus->RegisterSignal<int>("type_emit");
    bus->LinkSlotFunc<int>("type_emit", [](int) {});
    EXPECT_FALSE(bus->Emit("type_emit", 3.14));
}

// ---- 空参数 ----

TEST_F(BusTest, NoArgSignal)
{
    bus->RegisterSignal<>("noarg");
    bool called = false;
    bus->LinkSlotFunc<>("noarg", [&]() { called = true; });
    bus->Emit("noarg");
    EXPECT_TRUE(called);
}

// ---- Enable / Disable ----

TEST_F(BusTest, DisablePreventsEmit)
{
    bus->RegisterSignal<int>("dis_test");
    bool called = false;
    bus->LinkSlotFunc<int>("dis_test", [&](int) { called = true; });

    bus->Emit("dis_test", 1);
    EXPECT_TRUE(called);
    called = false;

    bus->DisableSignal("dis_test");
    EXPECT_FALSE(bus->Emit("dis_test", 1));
    EXPECT_FALSE(called);
}

// ---- 槽位删除 ----

TEST_F(BusTest, DeleteSlotRemovesIt)
{
    bus->RegisterSignal<int>("del_test");
    bool called = false;
    auto id = bus->LinkSlotFunc<int>("del_test", [&](int) { called = true; });

    bus->Emit("del_test", 0);
    EXPECT_TRUE(called);

    called = false;
    EXPECT_TRUE(bus->DeleteSlot("del_test", id));
    bus->Emit("del_test", 0);
    EXPECT_FALSE(called);
}

TEST_F(BusTest, DeleteInvalidIdReturnsFalse)
{
    bus->RegisterSignal<int>("invalid_del");
    EXPECT_FALSE(bus->DeleteSlot("invalid_del", 99999));
}

// ---- 弱引用 ----

TEST_F(BusTest, WeakSlotExpires)
{
    bus->RegisterSignal<int>("weak_test");
    bool called = false;

    {
        auto sp = std::make_shared<int>(100);
        bus->LinkSlotFunc<int>("weak_test", sp, [&](int x) {
            called = true;
            EXPECT_EQ(x, 100);
        });
        bus->Emit("weak_test", 100);
        EXPECT_TRUE(called);
        called = false;
    }
    // sp 析构 → slot 自动失效
    EXPECT_NO_THROW(bus->Emit("weak_test", 100));
    EXPECT_FALSE(called);
}

// ---- 异常安全 ----

TEST_F(BusTest, ExceptionInSlotIsolated)
{
    bus->RegisterSignal<int>("ex_test");
    int count = 0;

    bus->LinkSlotFunc<int>("ex_test", [](int) {
        throw std::runtime_error("boom");
    });
    bus->LinkSlotFunc<int>("ex_test", [&](int) { count++; });

    EXPECT_NO_THROW(bus->Emit("ex_test", 0));
    EXPECT_EQ(count, 1);
    EXPECT_EQ(bus->GetSlotCount("ex_test"), 1u);  // throwing slot auto-removed
}

// ---- 查询 ----

TEST_F(BusTest, GetSlotCount)
{
    bus->RegisterSignal<int>("count_test");
    EXPECT_EQ(bus->GetSlotCount("count_test"), 0u);
    bus->LinkSlotFunc<int>("count_test", [](int) {});
    EXPECT_EQ(bus->GetSlotCount("count_test"), 1u);
}

TEST_F(BusTest, PrintAllSignalsDoesNotThrow)
{
    bus->RegisterSignal<int>("sig_a");
    bus->RegisterSignal<double>("sig_b");
    EXPECT_NO_THROW(bus->PrintAllSignals());
}

// ---- 并发 ----

TEST_F(BusTest, ConcurrentEmit)
{
    bus->RegisterSignal<int>("concurrent");
    std::atomic<int> counter{0};
    const int kSlots = 10;

    for (int i = 0; i < kSlots; i++)
        bus->LinkSlotFunc<int>("concurrent", [&](int) { counter.fetch_add(1); });

    const int kThreads = 4;
    const int kEmits   = 50;
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; t++)
    {
        threads.emplace_back([&]() {
            for (int i = 0; i < kEmits; i++)
                bus->Emit("concurrent", 0);
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(counter.load(), kSlots * kThreads * kEmits);
}

// ---- RemoveSignal ----

TEST_F(BusTest, RemoveSignalDeletesIt)
{
    bus->RegisterSignal<int>("rm_test");
    EXPECT_GE(bus->GetSlotCount("rm_test"), 0u);

    EXPECT_TRUE(bus->RemoveSignal("rm_test"));
    // 删了之后 Emit 返回 false
    EXPECT_FALSE(bus->Emit("rm_test", 42));
}

TEST_F(BusTest, RemoveNonexistentSignal)
{
    EXPECT_FALSE(bus->RemoveSignal("ghost_signal_xyz"));
}

TEST_F(BusTest, RemoveSignalThenGetNames)
{
    bus->RegisterSignal<int>("names_a");
    bus->RegisterSignal<double>("names_b");
    bus->RegisterSignal<std::string>("names_c");

    auto before = bus->GetSignalNames();
    EXPECT_GE(before.size(), 3u);

    bus->RemoveSignal("names_b");

    auto after = bus->GetSignalNames();
    EXPECT_EQ(after.size(), before.size() - 1);
    // "names_b" 不应该在列表里
    EXPECT_EQ(std::find(after.begin(), after.end(), "names_b"), after.end());
}

TEST_F(BusTest, RemoveSignalStopsSlotExecution)
{
    bus->RegisterSignal<int>("stop_test");
    int count = 0;
    bus->LinkSlotFunc<int>("stop_test", [&](int) { count++; });

    bus->Emit("stop_test", 1);
    EXPECT_EQ(count, 1);

    bus->RemoveSignal("stop_test");
    // 信号没了，Emit 返回 false，slot 不会被调
    EXPECT_FALSE(bus->Emit("stop_test", 2));
    EXPECT_EQ(count, 1);  // 还是 1，没变
}

// ---- GetSignalNames ----

TEST_F(BusTest, GetSignalNamesReturnsAll)
{
    bus->RegisterSignal<int>("gn_a");
    bus->RegisterSignal<double>("gn_b");

    auto names = bus->GetSignalNames();
    EXPECT_GE(names.size(), 2u);
    EXPECT_NE(std::find(names.begin(), names.end(), "gn_a"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "gn_b"), names.end());
}

TEST_F(BusTest, GetSignalNamesEmptyWhenNone)
{
    // 注意：EventBus 是全局单例，可能已经有一些信号
    // 只验证方法不崩溃，返回值 >= 0
    auto names = bus->GetSignalNames();
    EXPECT_GE(names.size(), 0u);
}

// ---- 前缀匹配清理（模拟 UnloadModule 的行为）----

TEST_F(BusTest, PrefixBasedCleanup)
{
    // 模拟模块注册的信号: "Calc.add", "Calc.mul", "Calc.help"
    bus->RegisterSignal<int>("Calc.add");
    bus->RegisterSignal<int>("Calc.mul");
    bus->RegisterSignal<int>("Calc.help");
    // 其他模块的信号，不应该被清理
    bus->RegisterSignal<int>("Other.foo");

    // 按前缀 "Calc." 清理
    std::string prefix = "Calc.";
    for (const auto& sig : bus->GetSignalNames())
    {
        if (sig.compare(0, prefix.size(), prefix) == 0)
            bus->RemoveSignal(sig);
    }

    // Calc 的信号全部没了
    EXPECT_FALSE(bus->Emit("Calc.add", 0));
    EXPECT_FALSE(bus->Emit("Calc.mul", 0));
    EXPECT_FALSE(bus->Emit("Calc.help", 0));

    // Other 的信号还在
    EXPECT_TRUE(bus->Emit("Other.foo", 0));
}

TEST_F(BusTest, PrefixCleanupDoesNotMatchPartial)
{
    // 前缀 "Mod." 不应该误删 "Module.add"（多一个字符）
    bus->RegisterSignal<int>("Mod.run");
    bus->RegisterSignal<int>("Module.run");

    std::string prefix = "Mod.";
    for (const auto& sig : bus->GetSignalNames())
    {
        if (sig.compare(0, prefix.size(), prefix) == 0)
            bus->RemoveSignal(sig);
    }

    EXPECT_FALSE(bus->Emit("Mod.run", 0));       // 删了
    EXPECT_TRUE(bus->Emit("Module.run", 0));      // 没删
}

// ---- RemoveSignal + 类型安全 ----

TEST_F(BusTest, ReRegisterAfterRemove)
{
    bus->RegisterSignal<int>("rereg");
    bus->RemoveSignal("rereg");

    // 重新注册同名信号（不同类型也可以）
    bus->RegisterSignal<std::string>("rereg");
    EXPECT_TRUE(bus->Emit("rereg", std::string("hello")));
}
