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

// v2.7: TokenizeCommand 已集成到 HandleTXT，支持引号保护空格。
// 带空格的参数值用双引号包裹即可，例如 -v:msg|"hello world"。

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

// ================================================================
//  TokenizeCommand — 分词器隔离测试（v2.7 新增）
// ================================================================
//  TokenizeCommand 是纯函数：string → vector<string>。
//  不依赖 CommandParser 单例、队列、ParmarPack，完全隔离测试。
//  测试通过后才会集成到 HandleTXT 中替换 iss >> token。

class TokenizerTest : public ::testing::Test {};

// ---- 基本：纯空格切分（行为不变） ----

TEST_F(TokenizerTest, Basic_WhitespaceSplit)
{
    auto tokens = TokenizeCommand("-m:Calc -f:add -v:a|1,b|2");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "-m:Calc");
    EXPECT_EQ(tokens[1], "-f:add");
    EXPECT_EQ(tokens[2], "-v:a|1,b|2");
}

TEST_F(TokenizerTest, Basic_OnlyModule)
{
    auto tokens = TokenizeCommand("-m:MyModule");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "-m:MyModule");
}

TEST_F(TokenizerTest, Basic_ChineseNoQuotes)
{
    auto tokens = TokenizeCommand("-m:测试 -f:你好 -v:msg|世界");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "-m:测试");
    EXPECT_EQ(tokens[1], "-f:你好");
    EXPECT_EQ(tokens[2], "-v:msg|世界");
}

TEST_F(TokenizerTest, Basic_EmptyInput)
{
    auto tokens = TokenizeCommand("");
    EXPECT_TRUE(tokens.empty());
}

TEST_F(TokenizerTest, Basic_OnlyWhitespace)
{
    auto tokens = TokenizeCommand("   \t  ");
    EXPECT_TRUE(tokens.empty());
}

TEST_F(TokenizerTest, Basic_ConsecutiveSpaces)
{
    auto tokens = TokenizeCommand("-m:A    -f:B");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "-m:A");
    EXPECT_EQ(tokens[1], "-f:B");
}

// ---- 双引号：值内含空格 ----

TEST_F(TokenizerTest, DQuote_SpaceInValue)
{
    auto tokens = TokenizeCommand("-m:Print -f:echo -v:msg|\"hello world\"");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "-m:Print");
    EXPECT_EQ(tokens[1], "-f:echo");
    EXPECT_EQ(tokens[2], "-v:msg|hello world");
}

TEST_F(TokenizerTest, DQuote_MultipleWordsInValue)
{
    auto tokens = TokenizeCommand("-v:desc|\"C++ command framework\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "-v:desc|C++ command framework");
}

TEST_F(TokenizerTest, DQuote_TwoQuotedValues)
{
    auto tokens = TokenizeCommand(
        "-v:src|\"C:/my files/in.txt\",dst|\"/home/user/out data\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0],
        "-v:src|C:/my files/in.txt,dst|/home/user/out data");
}

// ---- 单引号 ----

TEST_F(TokenizerTest, SQuote_SpaceInValue)
{
    auto tokens = TokenizeCommand("-v:msg|'hello world'");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "-v:msg|hello world");
}

TEST_F(TokenizerTest, SQuote_PreservesDQuoteInside)
{
    // 单引号内的双引号是普通字符
    auto tokens = TokenizeCommand("-v:msg|'he said \"hello\"'");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "-v:msg|he said \"hello\"");
}

// ---- 反斜杠转义 ----

TEST_F(TokenizerTest, Escape_Space)
{
    auto tokens = TokenizeCommand("-v:msg|hello\\ world");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "-v:msg|hello world");
}

TEST_F(TokenizerTest, Escape_Backslash)
{
    auto tokens = TokenizeCommand("-v:path|C:\\\\Users\\\\test");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "-v:path|C:\\Users\\test");
}

TEST_F(TokenizerTest, Escape_DQuote)
{
    auto tokens = TokenizeCommand("-v:msg|\\\"hello\\\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "-v:msg|\"hello\"");
}

// 注意：TokenizeCommand 暂不支持双引号内部的 \" 转义。
// 即 `"hello \"world\""` 中的 \" 不会产出一个字面双引号。
// 因为双引号内遇到 \ 时，尝试读下一个字符如果也是 " 则双双跳过，
// 但 C++ 字符串字面量与分词器转义规则叠在一起难以在测试中精确构造输入。
// 此功能不影响核心价值（引号保护空格），如有需求再补。

// ---- 混合场景 ----

TEST_F(TokenizerTest, Mixed_QuotedAndUnquoted)
{
    auto tokens = TokenizeCommand(
        "-m:Search -f:query -v:term|\"hello world\",limit|10");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0], "-m:Search");
    EXPECT_EQ(tokens[1], "-f:query");
    EXPECT_EQ(tokens[2], "-v:term|hello world,limit|10");
}

TEST_F(TokenizerTest, Mixed_QuoteOnlyValuePart)
{
    // 只有 | 后面的部分加引号
    auto tokens = TokenizeCommand("-m:Log -f:write -v:msg|\"error: disk full\"");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[2], "-v:msg|error: disk full");
}

// ---- 边界条件 ----

TEST_F(TokenizerTest, Edge_UnclosedDQuote)
{
    // 未闭合引号 → 到字符串末尾自动结束（宽松处理）
    auto tokens = TokenizeCommand("-v:msg|\"hello world");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "-v:msg|hello world");
}

TEST_F(TokenizerTest, Edge_UnclosedSQuote)
{
    auto tokens = TokenizeCommand("-v:msg|'hello world");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "-v:msg|hello world");
}

TEST_F(TokenizerTest, Edge_EmptyQuotedString)
{
    // "" 不产生任何字符，token 只含引号前的内容
    auto tokens = TokenizeCommand("-v:key|\"\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "-v:key|");  // 值为空
}

TEST_F(TokenizerTest, Edge_EscapeAtEnd)
{
    // 行尾 \ → 保留字面反斜杠
    auto tokens = TokenizeCommand("-v:path|C:\\\\");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "-v:path|C:\\");
}

TEST_F(TokenizerTest, Edge_TabAsSeparator)
{
    auto tokens = TokenizeCommand("-m:A\t-f:B");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "-m:A");
    EXPECT_EQ(tokens[1], "-f:B");
}

TEST_F(TokenizerTest, Edge_LeadingAndTrailingSpaces)
{
    auto tokens = TokenizeCommand("  -m:A -f:B  ");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "-m:A");
    EXPECT_EQ(tokens[1], "-f:B");
}

TEST_F(TokenizerTest, Edge_SingleToken)
{
    auto tokens = TokenizeCommand("hello");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "hello");
}

// ---- 扩展现有测试：原来不能过的现在能过了 ----

TEST_F(TokenizerTest, Regression_SpaceInParamValue)
{
    // 这是之前会截断的场景：-v:key|hello world
    // 现在用引号保护 → 完整保留
    auto tokens = TokenizeCommand("-m:Print -f:2 -v:Param|\"hello world\"");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[2], "-v:Param|hello world");
}

TEST_F(TokenizerTest, Regression_PathWithSpaces)
{
    auto tokens = TokenizeCommand(
        "-m:Files -f:open -v:src|\"C:/Program Files/app/data.txt\"");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[2], "-v:src|C:/Program Files/app/data.txt");
}

// ================================================================
//  边界条件 — 第二轮：相邻引号、引号位置、嵌套、连续转义
// ================================================================

TEST_F(TokenizerTest, Edge_AdjacentQuotes)
{
    // "a""b" → 同 token 内两对引号拼接
    auto tokens = TokenizeCommand("-v:key|\"a\"\"b\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "-v:key|ab");
}

TEST_F(TokenizerTest, Edge_QuotedTokensSeparatedBySpace)
{
    // "a" "b" → 空格分隔 = 两个 token
    auto tokens = TokenizeCommand("\"a\" \"b\"");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0], "a");
    EXPECT_EQ(tokens[1], "b");
}

TEST_F(TokenizerTest, Edge_SpacesAtQuoteEdges)
{
    // 引号边界空格不能丢
    auto tokens = TokenizeCommand("-v:msg|\" hello world \"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "-v:msg| hello world ");
}

TEST_F(TokenizerTest, Edge_QuoteNotAtTokenStart)
{
    // 引号可以在 token 中间出现
    auto tokens = TokenizeCommand("-v:cfg|key=\"my value\",opt|1");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "-v:cfg|key=my value,opt|1");
}

TEST_F(TokenizerTest, Edge_NestedDQuotes)
{
    // "he said "hello"" → he said hello（中间无空格，合并为一个 token，与 bash 一致）
    auto tokens = TokenizeCommand("\"he said \"hello\"\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "he said hello");
}

TEST_F(TokenizerTest, Edge_SingleQuoteInsideDQuote)
{
    // 双引号内的单引号就是普通字符
    auto tokens = TokenizeCommand("-v:msg|\"it's a test\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "-v:msg|it's a test");
}

TEST_F(TokenizerTest, Edge_EscapeNonSpecialInDQuote)
{
    // 双引号内 \ 只在 \" 和 \\ 时被吞，\空格 保留反斜杠
    auto tokens = TokenizeCommand("\"hello\\ world\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "hello\\ world");
}

TEST_F(TokenizerTest, Edge_EscapeNonSpecialInSQuote)
{
    // 单引号内同理
    auto tokens = TokenizeCommand("'hello\\ world'");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "hello\\ world");
}

TEST_F(TokenizerTest, Edge_QuadBackslash)
{
    // \\\\ → \\  （两对 \\ 各生产一个 \）
    auto tokens = TokenizeCommand("-v:path|\\\\\\\\");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "-v:path|\\\\");
}

TEST_F(TokenizerTest, Edge_NewlineInQuotes)
{
    // 引号内的 \n 保留，不切分
    auto tokens = TokenizeCommand("-v:msg|\"line1\nline2\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "-v:msg|line1\nline2");
}

TEST_F(TokenizerTest, Edge_EscapeAtEndOfDQuote)
{
    // 未闭合引号 + 行尾反斜杠 → 反斜杠保留
    auto tokens = TokenizeCommand("\"hello\\");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], "hello\\");
}

// ================================================================
//  联动测试：TokenizeCommand 已集成到 HandleTXT → 走完整链路
// ================================================================
//  这些测试通过 CommandParser::SendCommand("TXT", ...) 验证：
//  字符串 → TokenizeCommand → HandleTXT → head_writers → ParmarPack

TEST_F(CmdParserTest, Integration_QuotedValueWithSpace)
{
    // 核心回归：带空格的参数值走完整链路
    bool ok = parser->SendCommand("TXT",
        std::any(std::string("-m:Search -f:query -v:term|\"hello world\",limit|10")));
    EXPECT_TRUE(ok);

    auto pack = parser->PopPack();
    ASSERT_NE(pack, nullptr);
    EXPECT_EQ(pack->mod_id, "Search");
    EXPECT_EQ(pack->func_id, "query");
    ASSERT_TRUE(pack->params.count("term"));
    EXPECT_EQ(pack->params["term"][0], "hello world");  // 空格完整保留！
    ASSERT_TRUE(pack->params.count("limit"));
    EXPECT_EQ(pack->params["limit"][0], "10");
}

TEST_F(CmdParserTest, Integration_PathWithSpaces)
{
    bool ok = parser->SendCommand("TXT",
        std::any(std::string("-m:Files -f:open -v:src|\"C:/Program Files/app/data.txt\"")));
    EXPECT_TRUE(ok);

    auto pack = parser->PopPack();
    ASSERT_NE(pack, nullptr);
    EXPECT_EQ(pack->mod_id, "Files");
    EXPECT_EQ(pack->func_id, "open");
    ASSERT_TRUE(pack->params.count("src"));
    EXPECT_EQ(pack->params["src"][0], "C:/Program Files/app/data.txt");
}

TEST_F(CmdParserTest, Integration_ChineseWithSpaceInValue)
{
    bool ok = parser->SendCommand("TXT",
        std::any(std::string("-m:测试模块 -f:你好 -v:msg|\"世界 你好\",lang|中文")));
    EXPECT_TRUE(ok);

    auto pack = parser->PopPack();
    ASSERT_NE(pack, nullptr);
    EXPECT_EQ(pack->mod_id, "测试模块");
    EXPECT_EQ(pack->func_id, "你好");
    EXPECT_EQ(pack->params["msg"][0], "世界 你好");
    EXPECT_EQ(pack->params["lang"][0], "中文");
}

TEST_F(CmdParserTest, Integration_MultipleSpacedValues)
{
    bool ok = parser->SendCommand("TXT",
        std::any(std::string(
            "-m:Log -f:write "
            "-v:msg|\"disk full error\",level|\"critical alert\","
            "path|\"/var/log/my app.log\"")));
    EXPECT_TRUE(ok);

    auto pack = parser->PopPack();
    ASSERT_NE(pack, nullptr);
    EXPECT_EQ(pack->mod_id, "Log");
    EXPECT_EQ(pack->func_id, "write");
    EXPECT_EQ(pack->params["msg"][0],   "disk full error");
    EXPECT_EQ(pack->params["level"][0], "critical alert");
    EXPECT_EQ(pack->params["path"][0],  "/var/log/my app.log");
}
