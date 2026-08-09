/// =================================================================
///  CommandParser 单元测试
/// =================================================================
///  覆盖：
///    - TXT 格式：文本命令解析
///    - RAW 格式：UI 直传 ParmarPack
///    - 自定义格式注册
///    - 错误处理（未知格式、空数据、类型错误）
///    - 自定义命令头
///    - PopPack / TryPopPack

#include <gtest/gtest.h>
#include <any>
#include <memory>
#include <string>
#include "parser/CommandParser.h"

class CmdParserTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        parser = &CommandParser::Get();
        // 清空队列（上一个测试可能残留数据）
        std::unique_ptr<ParmarPack> discarded;
        while (parser->TryPopPack(discarded)) {}
    }

    CommandParser* parser = nullptr;
};

// ================================================================
//  TXT 格式 — 文本解析
// ================================================================

TEST_F(CmdParserTest, TXT_BasicCommand)
{
    bool ok = parser->SendCommand("TXT", std::any(std::string("-m:MyMod -f:run")));
    EXPECT_TRUE(ok);

    auto pack = parser->PopPack();
    ASSERT_NE(pack, nullptr);
    EXPECT_EQ(pack->mod_id, "MyMod");
    EXPECT_EQ(pack->func_id, "run");
}

TEST_F(CmdParserTest, TXT_WithParams)
{
    bool ok = parser->SendCommand("TXT",
        std::any(std::string("-m:Calc -f:add -v:a|10,b|20")));
    EXPECT_TRUE(ok);

    auto pack = parser->PopPack();
    ASSERT_NE(pack, nullptr);
    EXPECT_EQ(pack->mod_id, "Calc");
    EXPECT_EQ(pack->func_id, "add");

    ASSERT_TRUE(pack->params.count("a"));
    ASSERT_FALSE(pack->params["a"].empty());
    EXPECT_EQ(pack->params["a"][0], "10");

    ASSERT_TRUE(pack->params.count("b"));
    ASSERT_FALSE(pack->params["b"].empty());
    EXPECT_EQ(pack->params["b"][0], "20");
}

TEST_F(CmdParserTest, TXT_MultipleValuesSameKey)
{
    parser->SendCommand("TXT",
        std::any(std::string("-m:Files -f:process -v:path|a.txt,path|b.txt,path|c.txt")));

    auto pack = parser->PopPack();
    ASSERT_NE(pack, nullptr);
    ASSERT_TRUE(pack->params.count("path"));
    EXPECT_EQ(pack->params["path"].size(), 3u);
    EXPECT_EQ(pack->params["path"][0], "a.txt");
    EXPECT_EQ(pack->params["path"][1], "b.txt");
    EXPECT_EQ(pack->params["path"][2], "c.txt");
}

TEST_F(CmdParserTest, TXT_MissingModuleId)
{
    bool ok = parser->SendCommand("TXT",
        std::any(std::string("-f:run -v:a|1")));
    EXPECT_FALSE(ok);

    // 队列应为空（解析失败不入队）
    std::unique_ptr<ParmarPack> pack;
    EXPECT_FALSE(parser->TryPopPack(pack));
}

TEST_F(CmdParserTest, TXT_EmptyText)
{
    EXPECT_FALSE(parser->SendCommand("TXT", std::any(std::string(""))));
}

TEST_F(CmdParserTest, TXT_HelpCommand)
{
    bool ok = parser->SendCommand("TXT",
        std::any(std::string("-m:TestMod -f:help")));
    EXPECT_TRUE(ok);

    auto pack = parser->PopPack();
    ASSERT_NE(pack, nullptr);
    EXPECT_EQ(pack->mod_id, "TestMod");
    EXPECT_EQ(pack->func_id, "help");
}

TEST_F(CmdParserTest, TXT_NoParamsSection)
{
    // 无 -v: 参数时也能正常解析（params 为空）
    parser->SendCommand("TXT",
        std::any(std::string("-m:SimpleMod -f:status")));

    auto pack = parser->PopPack();
    ASSERT_NE(pack, nullptr);
    EXPECT_EQ(pack->mod_id, "SimpleMod");
    EXPECT_EQ(pack->func_id, "status");
    EXPECT_TRUE(pack->params.empty());
}

// 注意：TXT 格式的限制
// 按空格切 token，所以 -v: 的参数值内部不能有空格。
// 例如 "-v:msg|hello world" 会被切成两个 token："-v:msg|hello" 和 "world"，
// 后者因为没有 ":" 会被丢弃。
// 这是简化设计 —— 控制台命令参数本来就不该含空格。
// 如果未来需要支持含空格的参数，可以加引号解析或者换 JSON 格式。

// ================================================================
//  SendPack — UI 直传 ParmarPack（不走 std::any）
// ================================================================

TEST_F(CmdParserTest, SendPack_BasicPack)
{
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id  = "Button";
    pack->func_id = "click";
    pack->params["x"].push_back("100");
    pack->params["y"].push_back("200");

    bool ok = parser->SendPack(std::move(pack));
    EXPECT_TRUE(ok);

    auto result = parser->PopPack();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->mod_id, "Button");
    EXPECT_EQ(result->func_id, "click");
    EXPECT_EQ(result->params["x"][0], "100");
    EXPECT_EQ(result->params["y"][0], "200");
}

TEST_F(CmdParserTest, SendPack_NullPack)
{
    bool ok = parser->SendPack(nullptr);
    EXPECT_FALSE(ok);
}

TEST_F(CmdParserTest, SendPack_EmptyModId)
{
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id = "";  // 空模块名
    pack->func_id = "test";

    bool ok = parser->SendPack(std::move(pack));
    EXPECT_FALSE(ok);
}

TEST_F(CmdParserTest, SendPack_ComplexParams)
{
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id = "Config";
    pack->func_id = "set";
    pack->params["host"].push_back("localhost");
    pack->params["port"].push_back("8080");
    pack->params["debug"].push_back("true");

    parser->SendPack(std::move(pack));
    auto result = parser->PopPack();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->params.size(), 3u);
}

// ================================================================
//  自定义格式注册
// ================================================================

TEST_F(CmdParserTest, CustomFormat_RegisterAndUse)
{
    // 注册一个自定义格式 "DOUBLE"：把字符串参数转成两个 ParmarPack
    parser->RegisterFormat("DOUBLE",
        [](std::any& value, LockQueue<ParmarPack>& queue) -> bool
        {
            try {
                std::string text = std::any_cast<std::string>(value);
                // 创建两个 pack
                auto p1 = std::make_unique<ParmarPack>();
                p1->mod_id = "First";
                p1->func_id = text;
                queue.Push(std::move(p1));

                auto p2 = std::make_unique<ParmarPack>();
                p2->mod_id = "Second";
                p2->func_id = text;
                queue.Push(std::move(p2));
                return true;
            } catch (...) {
                return false;
            }
        });

    parser->SendCommand("DOUBLE", std::any(std::string("echo")));

    auto first = parser->PopPack();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->mod_id, "First");
    EXPECT_EQ(first->func_id, "echo");

    auto second = parser->PopPack();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->mod_id, "Second");
    EXPECT_EQ(second->func_id, "echo");
}

TEST_F(CmdParserTest, CustomFormat_CanAccessBusAndModules)
{
    // 模拟真实场景：自定义格式处理器可以访问外部资源
    // 这里只验证格式处理器能捕获外部变量
    int call_count = 0;

    parser->RegisterFormat("COUNT",
        [&call_count](std::any& value, LockQueue<ParmarPack>& queue) -> bool
        {
            call_count++;
            auto pack = std::make_unique<ParmarPack>();
            pack->mod_id = "Counter";
            pack->func_id = std::to_string(call_count);
            queue.Push(std::move(pack));
            return true;
        });

    parser->SendCommand("COUNT", std::any(42));   // value 不一定是 string
    parser->SendCommand("COUNT", std::any(42));
    parser->SendCommand("COUNT", std::any(42));

    EXPECT_EQ(call_count, 3);

    auto p1 = parser->PopPack();
    EXPECT_EQ(p1->func_id, "1");
    auto p2 = parser->PopPack();
    EXPECT_EQ(p2->func_id, "2");
    auto p3 = parser->PopPack();
    EXPECT_EQ(p3->func_id, "3");
}

// ================================================================
//  错误处理
// ================================================================

TEST_F(CmdParserTest, UnknownFormat)
{
    bool ok = parser->SendCommand("GHOST_FORMAT", std::any(0));
    EXPECT_FALSE(ok);
}

TEST_F(CmdParserTest, TXT_WrongType)
{
    // TXT 格式需要 std::string，传 int 应该失败
    bool ok = parser->SendCommand("TXT", std::any(42));
    EXPECT_FALSE(ok);
}

TEST_F(CmdParserTest, TXT_WrongIntType)
{
    // TXT 格式需要 std::string，传 int 应该失败（已在上面测试过）
    // SendPack 不走 std::any，不存在类型错误的情况
    // 这里验证 SendPack(nullptr) 返回 false
    EXPECT_FALSE(parser->SendPack(nullptr));
}

TEST_F(CmdParserTest, TryPopReturnsFalseWhenEmpty)
{
    std::unique_ptr<ParmarPack> pack;
    EXPECT_FALSE(parser->TryPopPack(pack));
    EXPECT_EQ(pack, nullptr);
}

// ================================================================
//  自定义命令头
// ================================================================

TEST_F(CmdParserTest, CustomHead)
{
    // 注册自定义头 "-d:" → 写 params["data"]
    parser->RegisterHead("-d:",
        [](ParmarPack* p, const std::string& val) {
            if (p) p->params["data"].push_back(val);
        });

    parser->SendCommand("TXT",
        std::any(std::string("-m:Test -f:1 -d:my_custom_data")));

    auto pack = parser->PopPack();
    ASSERT_NE(pack, nullptr);
    ASSERT_TRUE(pack->params.count("data"));
    EXPECT_EQ(pack->params["data"][0], "my_custom_data");
}

TEST_F(CmdParserTest, MultipleCustomHeads)
{
    parser->RegisterHead("-d:",
        [](ParmarPack* p, const std::string& val) {
            if (p) p->params["data"].push_back(val);
        });
    parser->RegisterHead("-x:",
        [](ParmarPack* p, const std::string& val) {
            if (p) p->params["extra"].push_back(val);
        });

    parser->SendCommand("TXT",
        std::any(std::string("-m:Test -f:1 -d:hello -x:world")));

    auto pack = parser->PopPack();
    EXPECT_EQ(pack->params["data"][0], "hello");
    EXPECT_EQ(pack->params["extra"][0], "world");
}

// ================================================================
//  TXT + SendPack 混合使用（模拟控制台和 UI 同时运行）
// ================================================================

TEST_F(CmdParserTest, Mixed_TXT_and_SendPack)
{
    // 控制台发一条
    parser->SendCommand("TXT",
        std::any(std::string("-m:ConsoleMod -f:cmd -v:msg|from_console")));

    // UI 发一条
    auto uiPack = std::make_unique<ParmarPack>();
    uiPack->mod_id  = "UIMod";
    uiPack->func_id = "button_click";
    uiPack->params["btn"].push_back("submit");
    parser->SendPack(std::move(uiPack));

    // 再发一条控制台
    parser->SendCommand("TXT",
        std::any(std::string("-m:ConsoleMod -f:cmd -v:msg|second")));

    // 按顺序取出
    auto p1 = parser->PopPack();
    EXPECT_EQ(p1->mod_id, "ConsoleMod");
    EXPECT_EQ(p1->params["msg"][0], "from_console");

    auto p2 = parser->PopPack();
    EXPECT_EQ(p2->mod_id, "UIMod");
    EXPECT_EQ(p2->func_id, "button_click");

    auto p3 = parser->PopPack();
    EXPECT_EQ(p3->mod_id, "ConsoleMod");
    EXPECT_EQ(p3->params["msg"][0], "second");
}

// ================================================================
//  并发安全（多线程同时 SendCommand）
// ================================================================

TEST_F(CmdParserTest, ConcurrentSendCommand)
{
    // LockQueue 已经有 mutex，多线程 Push 应该是安全的
    // 这里做基本的并发测试
    const int kThreads = 4;
    const int kPerThread = 25;
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; t++)
    {
        threads.emplace_back([this, t]() {
            for (int i = 0; i < kPerThread; i++)
            {
                auto pack = std::make_unique<ParmarPack>();
                pack->mod_id  = "Thread" + std::to_string(t);
                pack->func_id = std::to_string(i);
                parser->SendPack(std::move(pack));
            }
        });
    }
    for (auto& th : threads) th.join();

    // 取出所有，验证数量
    int count = 0;
    std::unique_ptr<ParmarPack> p;
    while (parser->TryPopPack(p))
        count++;

    EXPECT_EQ(count, kThreads * kPerThread);
}

// ================================================================
//  边界条件 — RegisterFormat
// ================================================================

TEST_F(CmdParserTest, RegisterFormat_EmptyNameIgnored)
{
    int called = 0;
    parser->RegisterFormat("", [&called](std::any&, LockQueue<ParmarPack>&) -> bool {
        called++; return true;
    });

    // 空名字的格式不会被注册，SendCommand 应该返回 false
    bool ok = parser->SendCommand("", std::any(42));
    EXPECT_FALSE(ok);
    EXPECT_EQ(called, 0);
}

TEST_F(CmdParserTest, RegisterFormat_NullHandlerIgnored)
{
    // null handler 不应该注册
    parser->RegisterFormat("NULL_FORMAT", nullptr);

    bool ok = parser->SendCommand("NULL_FORMAT", std::any(0));
    EXPECT_FALSE(ok);  // 格式不存在（null handler 被拒绝）
}

TEST_F(CmdParserTest, RegisterFormat_OverrideExisting)
{
    int old_called = 0, new_called = 0;

    // 第一次注册
    parser->RegisterFormat("OVERRIDE",
        [&old_called](std::any&, LockQueue<ParmarPack>&) -> bool {
            old_called++; return true;
        });

    // 第二次注册（覆盖）
    parser->RegisterFormat("OVERRIDE",
        [&new_called](std::any&, LockQueue<ParmarPack>&) -> bool {
            new_called++; return true;
        });

    parser->SendCommand("OVERRIDE", std::any(0));

    EXPECT_EQ(old_called, 0);  // 旧的没被调用
    EXPECT_EQ(new_called, 1);  // 新 handler 被调用
}

// ================================================================
//  边界条件 — TXT 解析
// ================================================================

TEST_F(CmdParserTest, TXT_OnlyModuleId)
{
    // 只有 -m:，没有 -f: 和 -v: —— 也是合法的（比如 help 命令）
    parser->SendCommand("TXT", std::any(std::string("-m:SomeModule")));

    auto pack = parser->PopPack();
    ASSERT_NE(pack, nullptr);
    EXPECT_EQ(pack->mod_id, "SomeModule");
    EXPECT_TRUE(pack->func_id.empty());
    EXPECT_TRUE(pack->params.empty());
}

TEST_F(CmdParserTest, TXT_UnknownFlagMixedWithKnown)
{
    // 未知 flag 被跳过，已知的照常解析
    parser->SendCommand("TXT",
        std::any(std::string("-m:TestMod -f:run -unknown:garbage -v:a|1")));

    auto pack = parser->PopPack();
    ASSERT_NE(pack, nullptr);
    EXPECT_EQ(pack->mod_id, "TestMod");
    EXPECT_EQ(pack->func_id, "run");
    EXPECT_EQ(pack->params["a"][0], "1");
    // -unknown: 被忽略，不影响其他解析
}

TEST_F(CmdParserTest, TXT_ChineseCharacters)
{
    // 中文字符应该被正确保留
    parser->SendCommand("TXT",
        std::any(std::string("-m:测试模块 -f:你好 -v:msg|世界,lang|中文")));

    auto pack = parser->PopPack();
    ASSERT_NE(pack, nullptr);
    EXPECT_EQ(pack->mod_id, "测试模块");
    EXPECT_EQ(pack->func_id, "你好");
    EXPECT_EQ(pack->params["msg"][0], "世界");
    EXPECT_EQ(pack->params["lang"][0], "中文");
}

TEST_F(CmdParserTest, TXT_PathLikeValues)
{
    // 文件路径（不含空格才能正确解析）
    parser->SendCommand("TXT",
        std::any(std::string("-m:Files -f:open -v:src|C:/Users/test/data.txt,dst|/home/user/output")));

    auto pack = parser->PopPack();
    ASSERT_NE(pack, nullptr);
    EXPECT_EQ(pack->params["src"][0], "C:/Users/test/data.txt");
    EXPECT_EQ(pack->params["dst"][0], "/home/user/output");
}

TEST_F(CmdParserTest, TXT_BackslashPaths)
{
    parser->SendCommand("TXT",
        std::any(std::string("-m:Files -f:load -v:path|E:\\data\\config.json")));

    auto pack = parser->PopPack();
    ASSERT_NE(pack, nullptr);
    EXPECT_EQ(pack->params["path"][0], "E:\\data\\config.json");
}

// ================================================================
//  边界条件 — SendPack
// ================================================================

TEST_F(CmdParserTest, SendPack_EmptyFuncId)
{
    // func_id 为空是合法的（某些命令只需要 mod_id，比如查询模块状态）
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id  = "StatusModule";
    pack->func_id = "";

    EXPECT_TRUE(parser->SendPack(std::move(pack)));

    auto result = parser->PopPack();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->mod_id, "StatusModule");
    EXPECT_TRUE(result->func_id.empty());
}

TEST_F(CmdParserTest, SendPack_LargeParams)
{
    // 大量参数
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id  = "Bulk";
    pack->func_id = "process";

    for (int i = 0; i < 100; i++)
        pack->params["key" + std::to_string(i)].push_back("val" + std::to_string(i));

    EXPECT_TRUE(parser->SendPack(std::move(pack)));

    auto result = parser->PopPack();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->params.size(), 100u);
}

TEST_F(CmdParserTest, MultipleSendPack_Ordering)
{
    // 验证队列 FIFO 顺序
    for (int i = 0; i < 5; i++)
    {
        auto pack = std::make_unique<ParmarPack>();
        pack->mod_id = "Seq";
        pack->func_id = std::to_string(i);
        parser->SendPack(std::move(pack));
    }

    for (int i = 0; i < 5; i++)
    {
        auto result = parser->PopPack();
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result->func_id, std::to_string(i));
    }
}

// ================================================================
//  边界条件 — 队列
// ================================================================

TEST_F(CmdParserTest, Queue_DrainAfterMultipleSends)
{
    // 多发几条命令，Ensure 队列能正确排空
    parser->SendCommand("TXT", std::any(std::string("-m:A -f:1")));
    parser->SendCommand("TXT", std::any(std::string("-m:B -f:2")));

    auto p1 = std::make_unique<ParmarPack>();
    p1->mod_id = "C"; p1->func_id = "3";
    parser->SendPack(std::move(p1));

    EXPECT_EQ(parser->PopPack()->mod_id, "A");
    EXPECT_EQ(parser->PopPack()->mod_id, "B");
    EXPECT_EQ(parser->PopPack()->mod_id, "C");

    // 队列应该空了
    std::unique_ptr<ParmarPack> empty;
    EXPECT_FALSE(parser->TryPopPack(empty));
}

TEST_F(CmdParserTest, Queue_SetupClearsStaleData)
{
    // SetUp 中已经清空队列 — 验证确实清了
    std::unique_ptr<ParmarPack> pack;
    EXPECT_FALSE(parser->TryPopPack(pack));
}
