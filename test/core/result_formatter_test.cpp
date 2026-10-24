/// =================================================================
///  IResultFormatter isolation tests
/// =================================================================
///  Tests formatters in isolation — just pack → string translation.

#include <gtest/gtest.h>
#include <string>
#include "core/IResultFormatter.h"

// ================================================================
//  ConsoleFormatter
// ================================================================

TEST(ConsoleFormatterTest, SuccessOutput)
{
    ConsoleFormatter fmt;
    ParmarPack pack;
    pack.success = true;
    pack.return_value = "300";

    EXPECT_EQ(fmt.Format(pack), "[OK] 300");
}

TEST(ConsoleFormatterTest, ErrorOutput)
{
    ConsoleFormatter fmt;
    ParmarPack pack;
    pack.success = false;
    pack.error.code = 404;
    pack.error.message = "Module not found: GhostMod";

    std::string out = fmt.Format(pack);
    EXPECT_NE(out.find("[ERR 404]"), std::string::npos);
    EXPECT_NE(out.find("Module not found"), std::string::npos);
    EXPECT_NE(out.find("GhostMod"), std::string::npos);
}

TEST(ConsoleFormatterTest, EmptyReturnValue)
{
    ConsoleFormatter fmt;
    ParmarPack pack;
    pack.success = true;
    pack.return_value = "";

    EXPECT_EQ(fmt.Format(pack), "[OK] ");
}

TEST(ConsoleFormatterTest, AllErrorCodes)
{
    ConsoleFormatter fmt;

    struct { int code; const char* expected; } cases[] = {
        {ErrorCode::MODULE_NOT_FOUND, "Module not found"},
        {ErrorCode::FUNC_NOT_FOUND,   "Function not found"},
        {ErrorCode::SIGNAL_NOT_FOUND, "Signal not registered"},
        {ErrorCode::INVALID_PARAMS,   "Invalid parameters"},
        {ErrorCode::DLL_LOAD_FAILED,  "DLL load failed"},
        {ErrorCode::INTERNAL_ERROR,   "Internal error"},
        {999, "Unknown error"},
    };

    for (auto& c : cases) {
        ParmarPack pack;
        pack.success = false;
        pack.error.code = c.code;
        pack.error.message = "test";

        std::string out = fmt.Format(pack);
        EXPECT_NE(out.find(c.expected), std::string::npos)
            << "Error code " << c.code << " should translate";
    }
}

// ================================================================
//  JsonFormatter
// ================================================================

TEST(JsonFormatterTest, SuccessOutput)
{
    JsonFormatter fmt;
    ParmarPack pack;
    pack.success = true;
    pack.return_value = "hello";

    std::string out = fmt.Format(pack);
    EXPECT_NE(out.find("\"ok\":true"), std::string::npos);
    EXPECT_NE(out.find("\"data\":\"hello\""), std::string::npos);
}

TEST(JsonFormatterTest, ErrorOutput)
{
    JsonFormatter fmt;
    ParmarPack pack;
    pack.success = false;
    pack.error.code = 500;
    pack.error.message = "boom";

    std::string out = fmt.Format(pack);
    EXPECT_NE(out.find("\"ok\":false"), std::string::npos);
    EXPECT_NE(out.find("\"code\":500"), std::string::npos);
    EXPECT_NE(out.find("\"message\":\"boom\""), std::string::npos);
}

TEST(JsonFormatterTest, EscapeQuotes)
{
    JsonFormatter fmt;
    ParmarPack pack;
    pack.success = true;
    pack.return_value = "he said \"hello\"";

    std::string out = fmt.Format(pack);
    EXPECT_NE(out.find("\\\""), std::string::npos)
        << "Quotes should be escaped";
}

TEST(JsonFormatterTest, EscapeBackslash)
{
    JsonFormatter fmt;
    ParmarPack pack;
    pack.success = false;
    pack.error.code = 1;
    pack.error.message = "C:\\path\\file";

    std::string out = fmt.Format(pack);
    EXPECT_NE(out.find("C:\\\\path\\\\file"), std::string::npos)
        << "Backslashes should be escaped";
}

TEST(JsonFormatterTest, EscapesControlCharactersInData)
{
    // 弱转义 bug：return_value 含 \t \r 控制字符时旧实现原样输出，
    // 产出非法 JSON（JSON 规范禁止字符串内裸控制字符，QML JSON.parse 拒收）。
    // \x01 用相邻字面量隔离拼接，防 hex 转义贪婪吞噬后续字符。
    JsonFormatter fmt;
    ParmarPack pack;
    pack.success = true;
    pack.return_value = "tab\there\rcr" "\x01" "ctrl";

    std::string out = fmt.Format(pack);

    // 紧凑单行 JSON 全文不允许任何裸控制字节（< 0x20）
    for (unsigned char c : out) {
        EXPECT_GE(c, 0x20u)
            << "raw control byte 0x" << std::hex << static_cast<int>(c)
            << " in output — invalid JSON";
    }
    EXPECT_NE(out.find("\\t"), std::string::npos);
    EXPECT_NE(out.find("\\r"), std::string::npos);
    EXPECT_NE(out.find("\\u0001"), std::string::npos);
}

TEST(JsonFormatterTest, EscapesControlCharactersInErrorMessage)
{
    JsonFormatter fmt;
    ParmarPack pack;
    pack.success = false;
    pack.error.code = 1;
    pack.error.message = "line1\tline2" "\x02";

    std::string out = fmt.Format(pack);

    for (unsigned char c : out) {
        EXPECT_GE(c, 0x20u)
            << "raw control byte 0x" << std::hex << static_cast<int>(c)
            << " in output — invalid JSON";
    }
    EXPECT_NE(out.find("\\t"), std::string::npos);
    EXPECT_NE(out.find("\\u0002"), std::string::npos);
}

// ================================================================
//  QuietFormatter
// ================================================================

TEST(QuietFormatterTest, AlwaysEmpty)
{
    QuietFormatter fmt;
    ParmarPack pack;
    pack.success = true;
    pack.return_value = "should not appear";
    EXPECT_EQ(fmt.Format(pack), "");

    pack.success = false;
    pack.error.code = 500;
    EXPECT_EQ(fmt.Format(pack), "");
}

// ================================================================
//  Polymorphic use (strategy pattern proof)
// ================================================================

TEST(FormatterTest, PolymorphicDispatch)
{
    ConsoleFormatter console;
    JsonFormatter json;
    QuietFormatter quiet;

    ParmarPack pack;
    pack.success = true;
    pack.return_value = "done";

    // All three formatters produce different output from same pack
    EXPECT_NE(console.Format(pack), json.Format(pack));
    EXPECT_NE(json.Format(pack), quiet.Format(pack));
    EXPECT_EQ(quiet.Format(pack), "");
}

TEST(FormatterTest, FormatterAsInterface)
{
    // Prove strategy pattern works: assign different formatters
    ConsoleFormatter c;
    JsonFormatter j;
    QuietFormatter q;

    IResultFormatter* fmt = &c;
    EXPECT_NE(fmt->Format({}), "");  // not empty

    fmt = &q;
    EXPECT_EQ(fmt->Format({}), "");  // empty
}
