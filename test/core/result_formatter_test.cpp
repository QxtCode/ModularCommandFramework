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
