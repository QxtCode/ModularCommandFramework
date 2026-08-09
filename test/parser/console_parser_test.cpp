/// ConsoleParser unit tests — text parsing into ParmarPack

#include <gtest/gtest.h>
#include <thread>
#include "parser/ConsoleParser.h"

class ParserTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        parser = &ConsoleParser::Get();
    }

    ConsoleParser* parser = nullptr;
};

// ---- 基本解析 ----

TEST_F(ParserTest, ParseBasicCommand)
{
    bool ok = parser->Send("-m:MyMod -f:42 -v:k|val");
    EXPECT_TRUE(ok);

    auto pack = parser->PopPack();
    ASSERT_NE(pack, nullptr);
    EXPECT_EQ(pack->mod_id, "MyMod");
    EXPECT_EQ(pack->func_id, "42");

    auto it = pack->params.find("k");
    ASSERT_NE(it, pack->params.end());
    ASSERT_FALSE(it->second.empty());
    EXPECT_EQ(it->second[0], "val");
}

TEST_F(ParserTest, ParseMultipleParams)
{
    parser->Send("-m:M -f:1 -v:a|1,b|2,c|3");

    auto pack = parser->PopPack();
    EXPECT_EQ(pack->params.size(), 3u);
    EXPECT_EQ(pack->params["a"][0], "1");
    EXPECT_EQ(pack->params["b"][0], "2");
    EXPECT_EQ(pack->params["c"][0], "3");
}

TEST_F(ParserTest, MissingModuleIdReturnsFalse)
{
    EXPECT_FALSE(parser->Send("-f:1 -v:k|v"));
}

TEST_F(ParserTest, EmptyTextReturnsFalse)
{
    EXPECT_FALSE(parser->Send(""));
}

// ---- 自定义命令头 ----

TEST_F(ParserTest, CustomHead)
{
    parser->RegisterHead("-x:",
        [](ParmarPack* p, const std::string& val) {
            p->params["custom"].push_back(val);
        });

    parser->Send("-m:M -f:1 -x:hello_world");

    auto pack = parser->PopPack();
    EXPECT_EQ(pack->params["custom"][0], "hello_world");
}

// ---- 空格处理（当前解析器按空格分词，值内不应有空格）----

TEST_F(ParserTest, TokensAreSplitBySpaces)
{
    // 解析器用 >> 分词，所以参数值内不要有空格
    parser->Send("-m:TestMod -f:func1 -v:key|val");

    auto pack = parser->PopPack();
    EXPECT_EQ(pack->mod_id, "TestMod");
    EXPECT_EQ(pack->func_id, "func1");
    EXPECT_EQ(pack->params["key"][0], "val");
}

// ---- 线程安全 ----

TEST_F(ParserTest, TryPopReturnsFalseWhenEmpty)
{
    std::unique_ptr<ParmarPack> out;
    EXPECT_FALSE(parser->TryPopPack(out));
}
