/// =================================================================
///  LogModule 单元测试
/// =================================================================
///  覆盖：
///    - 日志级别过滤（debug/info/error/fatal）
///    - 纯文本格式 / XML 格式
///    - 控制台 / 文件输出
///    - 线程安全文件输出
///    - LogModule 通过 EventBus 命令调用
///    - "log.written" 信号发射
///    - 错误码标准化

#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include "modules/logging/LogModule.h"
#include "event_bus/event_bus.h"

// ================================================================
//  Logger 引擎测试（不依赖 EventBus / ModuleBaseObject）
// ================================================================

TEST(LoggerTest, LevelFilter)
{
    Logger logger;
    logger.SetFormat(std::make_unique<PlainFormat>());

    // 用自定义输出捕获日志
    struct CaptureOutput : public LogOutput {
        std::string last;
        void Write(const std::string& s) override { last = s; }
    };
    auto cap = std::make_unique<CaptureOutput>();
    auto* raw_cap = cap.get();
    logger.SetOutput(std::move(cap));

    // INFO 级别 → DEBUG 被过滤
    logger.SetLevel(LogLevel::INFO);
    logger.Write(LogLevel::DEBUG, "should be filtered", __FILE__, __LINE__);
    EXPECT_TRUE(raw_cap->last.empty());

    logger.Write(LogLevel::INFO, "should appear", __FILE__, __LINE__);
    EXPECT_FALSE(raw_cap->last.empty());
    EXPECT_NE(raw_cap->last.find("should appear"), std::string::npos);
}

TEST(LoggerTest, FormatPlain)
{
    PlainFormat fmt;
    auto result = fmt.Format(LogLevel::ERROR, "test message", "main.cpp", 42);

    // 包含时间戳（至少 19 个字符）、级别、文件名、行号、消息
    EXPECT_NE(result.find("ERROR"), std::string::npos);
    EXPECT_NE(result.find("main.cpp"), std::string::npos);
    EXPECT_NE(result.find("42"), std::string::npos);
    EXPECT_NE(result.find("test message"), std::string::npos);
}

TEST(LoggerTest, FormatXml)
{
    XmlFormat fmt;
    auto result = fmt.Format(LogLevel::INFO, "hello xml", "test.cpp", 10);

    EXPECT_NE(result.find("<log>"), std::string::npos);
    EXPECT_NE(result.find("<level>INFO</level>"), std::string::npos);
    EXPECT_NE(result.find("<msg>hello xml</msg>"), std::string::npos);
    EXPECT_NE(result.find("test.cpp"), std::string::npos);
}

// ================================================================
//  文件输出测试
// ================================================================

TEST(LoggerTest, FileOutput)
{
    std::string test_path = "test_log_output.txt";
    std::remove(test_path.c_str());

    {
        FileOutput fout;
        ASSERT_TRUE(fout.Open(test_path));
        fout.Write("line1\n");
        fout.Write("line2\n");
    } // fout 析构 → ofs_ 关闭 → 文件落盘

    // 读回验证
    std::ifstream in(test_path);
    ASSERT_TRUE(in.is_open());
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("line1"), std::string::npos);
    EXPECT_NE(content.find("line2"), std::string::npos);

    std::remove(test_path.c_str());
}

// ================================================================
//  线程安全文件输出测试
// ================================================================

TEST(LoggerTest, ThreadFileOutput)
{
    std::string test_path = "test_thread_log.txt";
    std::remove(test_path.c_str());

    ThreadFileOutput tout;
    ASSERT_TRUE(tout.Open(test_path));

    // 写几行
    tout.Write("thread_line_1\n");
    tout.Write("thread_line_2\n");

    // 等后台线程处理
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // tout 析构 → 等后台线程结束 → 文件完整
    // 用作用域强制析构
    {
        ThreadFileOutput t2;
        t2.Open(test_path);
        t2.Write("scope_test\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    } // t2 析构

    std::ifstream in(test_path);
    ASSERT_TRUE(in.is_open());
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("scope_test"), std::string::npos);

    std::remove(test_path.c_str());
}

// ================================================================
//  LogModule 通过 EventBus 命令调用
// ================================================================

class LogModuleTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        bus = &EventBus::GetInstance();

        // 注册核心信号
        bus->RegisterSignal<uint32_t, bool, int, const char*, const char*>("task.result");

        // 注册 LogModule（OnInit 注册 info/error/debug/fatal/setlevel + log.written）
        log_mod = std::make_shared<LogModule>();
        log_mod->OnInit();
        log_mod->ConnectToEventBus(log_mod);  // v2.4: shared_ptr for weak-ref protection

        // 手动注册 "Logger.help"（正常流程由 AddModule 自动注册，这里模拟）
        bus->RegisterSignal<ParmarPack*>("Logger.help");
        bus->LinkSlotFunc<ParmarPack*>("Logger.help",
            [this](ParmarPack* p) { log_mod->Help(p); p->success = true; });
    }

    void TearDown() override
    {
        // 重置全局日志级别（避免测试间互相影响）
        LogFac::Instance().GetLogger().SetLevel(LogLevel::DEBUG);
        log_mod.reset();
    }

    EventBus* bus = nullptr;
    std::shared_ptr<LogModule> log_mod;
};

TEST_F(LogModuleTest, InfoCommand)
{
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id  = "Logger";
    pack->func_id = "info";
    pack->params["msg"].push_back("integration_test_message");

    bool emitted = bus->Emit("Logger.info", pack.get());
    EXPECT_TRUE(emitted);
    EXPECT_TRUE(pack->success);
}

TEST_F(LogModuleTest, ErrorCommand)
{
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id  = "Logger";
    pack->func_id = "error";
    pack->params["msg"].push_back("something went wrong");

    bool emitted = bus->Emit("Logger.error", pack.get());
    EXPECT_TRUE(emitted);
    EXPECT_TRUE(pack->success);
}

TEST_F(LogModuleTest, MissingParamReturnsError)
{
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id  = "Logger";
    pack->func_id = "info";
    // 故意不填 msg

    bus->Emit("Logger.info", pack.get());
    EXPECT_FALSE(pack->success);
    EXPECT_EQ(pack->error.code, 400);  // ErrorCode::INVALID_PARAMS
}

TEST_F(LogModuleTest, SetLevelCommand)
{
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id  = "Logger";
    pack->func_id = "setlevel";
    pack->params["level"].push_back("error");

    bus->Emit("Logger.setlevel", pack.get());
    EXPECT_TRUE(pack->success);

    // 验证级别确实变了：现在 DEBUG 级别的日志应该被过滤
    auto& logger = LogFac::Instance().GetLogger();
    EXPECT_EQ(logger.GetLevel(), LogLevel::ERROR);
}

TEST_F(LogModuleTest, LogWrittenSignal)
{
    // 监听 "log.written" 信号
    std::string last_level, last_msg, last_file;
    int last_line = 0;
    bool received = false;

    bus->LinkSlotFunc<const char*, const char*, const char*, int>(
        "log.written",
        [&](const char* level, const char* msg, const char* file, int line) {
            last_level = level;
            last_msg   = msg;
            last_file  = file;
            last_line  = line;
            received   = true;
        });

    // 触发一次日志
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id  = "Logger";
    pack->func_id = "info";
    pack->params["msg"].push_back("signal_test");
    bus->Emit("Logger.info", pack.get());

    EXPECT_TRUE(received);
    EXPECT_EQ(last_level, "INFO");
    EXPECT_EQ(last_msg, "signal_test");

    // 恢复级别（后续测试不受影响）
    LogFac::Instance().GetLogger().SetLevel(LogLevel::DEBUG);
}

// ================================================================
//  LogFac 单例测试
// ================================================================

TEST(LogFacTest, Singleton)
{
    auto& a = LogFac::Instance();
    auto& b = LogFac::Instance();
    EXPECT_EQ(&a, &b);
}

TEST(LogFacTest, MacrosDontCrash)
{
    // 确保宏调用不会崩溃（Logger 已在 LogFac 构造时初始化）
    EXPECT_NO_THROW(LOG_DEBUG("test debug"));
    EXPECT_NO_THROW(LOG_INFO("test info"));
    EXPECT_NO_THROW(LOG_ERROR("test error"));
    EXPECT_NO_THROW(LOG_FATAL("test fatal"));
}

// ================================================================
//  错误码标准测试
// ================================================================

TEST(ErrorCodeTest, StandardCodes)
{
    EXPECT_EQ(ErrorCode::OK, 0);
    EXPECT_EQ(ErrorCode::MODULE_NOT_FOUND, 404);
    EXPECT_EQ(ErrorCode::FUNC_NOT_FOUND, 405);
    EXPECT_EQ(ErrorCode::SIGNAL_NOT_FOUND, 406);
    EXPECT_EQ(ErrorCode::INVALID_PARAMS, 400);
    EXPECT_EQ(ErrorCode::DLL_LOAD_FAILED, 501);
    EXPECT_EQ(ErrorCode::INTERNAL_ERROR, 500);
}

// ================================================================
//  LogModule help 命令
// ================================================================

TEST_F(LogModuleTest, HelpSignal)
{
    // "Logger.help" 应该能正常执行
    auto pack = std::make_unique<ParmarPack>();
    pack->mod_id  = "Logger";
    pack->func_id = "help";

    bool emitted = bus->Emit("Logger.help", pack.get());
    EXPECT_TRUE(emitted);
    EXPECT_TRUE(pack->success);
}

// ================================================================
//  日志级别枚举转换
// ================================================================

TEST(LogLevelTest, StringConversion)
{
    EXPECT_STREQ(LogLevelToString(LogLevel::DEBUG), "DEBUG");
    EXPECT_STREQ(LogLevelToString(LogLevel::INFO),  "INFO");
    EXPECT_STREQ(LogLevelToString(LogLevel::ERROR), "ERROR");
    EXPECT_STREQ(LogLevelToString(LogLevel::FATAL), "FATAL");

    EXPECT_EQ(StringToLogLevel("debug"), LogLevel::DEBUG);
    EXPECT_EQ(StringToLogLevel("info"),  LogLevel::INFO);
    EXPECT_EQ(StringToLogLevel("error"), LogLevel::ERROR);
    EXPECT_EQ(StringToLogLevel("fatal"), LogLevel::FATAL);
    EXPECT_EQ(StringToLogLevel("unknown"), LogLevel::DEBUG);  // 默认
}
