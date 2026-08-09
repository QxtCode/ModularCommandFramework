/// =================================================================
///  LogModule — 内置日志模块（基于用户原版日志库）
/// =================================================================
///
///  【架构】
///  LogFormat (接口) → XLogFormat / MLogFormat     — 格式化（纯文本/XML）
///  LogOutput (接口) → ConsoleOutput / FileOutput   — 输出目标
///  Logger          → 组合 format + output + level   — 日志引擎
///  LogFac          → 单例工厂，从配置文件初始化      — 全局入口
///  LogModule       → ModuleBaseObject 包装           — 命令接口
///
///  【两种使用方式】
///  1. 命令行：-m:Logger -f:info -v:msg|服务器启动
///            -m:Logger -f:setlevel -v:level|error
///  2. 代码宏：LOG_INFO("服务器启动");
///            LOG_ERROR("连接失败");
///            LOG_FATAL("致命错误");
///
///  【配置 (log.conf)】
///  log_type=console          # console / file / thread_file
///  log_file=log.txt          # 文件模式下有效
///  log_level=debug           # debug / info / error / fatal
///  log_format=xml             # (空=纯文本) / xml
/// =================================================================

#pragma once
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "core/ModuleBaseObject.h"
#include "event_bus/event_bus.h"

// =================================================================
//  日志等级
// =================================================================
// 注意：Windows.h 定义了 ERROR、DEBUG 宏，必须先 undef 再定义枚举
#ifdef ERROR
#undef ERROR
#endif
#ifdef DEBUG
#undef DEBUG
#endif

enum class LogLevel { DEBUG, INFO, ERROR, FATAL };

inline const char* LogLevelToString(LogLevel level)
{
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
    }
    return "UNKNOWN";
}

inline LogLevel StringToLogLevel(const std::string& s)
{
    if (s == "info")  return LogLevel::INFO;
    if (s == "error") return LogLevel::ERROR;
    if (s == "fatal") return LogLevel::FATAL;
    return LogLevel::DEBUG;
}

// =================================================================
//  LogFormat — 日志格式化接口
// =================================================================
class LogFormat
{
public:
    virtual ~LogFormat() = default;
    virtual std::string Format(LogLevel level,
                               const std::string& msg,
                               const std::string& file,
                               int line) = 0;
};

// 纯文本格式: [2026-08-07 10:30:00] [INFO] main.cpp:42 服务器启动
class PlainFormat : public LogFormat
{
public:
    std::string Format(LogLevel level, const std::string& msg,
                       const std::string& file, int line) override
    {
        auto now = std::time(nullptr);
        char time_buf[32];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S",
                      std::localtime(&now));

        // 取文件名（去掉路径）
        auto pos = file.find_last_of("\\/");
        std::string filename = (pos != std::string::npos) ? file.substr(pos + 1) : file;

        return std::string("[") + time_buf + "] [" +
               LogLevelToString(level) + "] " +
               filename + ":" + std::to_string(line) + "  " + msg + "\n";
    }
};

// XML 格式
class XmlFormat : public LogFormat
{
public:
    std::string Format(LogLevel level, const std::string& msg,
                       const std::string& file, int line) override
    {
        auto now = std::time(nullptr);
        char time_buf[32];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S",
                      std::localtime(&now));

        auto pos = file.find_last_of("\\/");
        std::string filename = (pos != std::string::npos) ? file.substr(pos + 1) : file;

        return std::string("<log>\n") +
               "  <time>" + time_buf + "</time>\n" +
               "  <level>" + LogLevelToString(level) + "</level>\n" +
               "  <file>" + filename + ":" + std::to_string(line) + "</file>\n" +
               "  <msg>" + msg + "</msg>\n" +
               "</log>\n";
    }
};

// =================================================================
//  LogOutput — 日志输出接口
// =================================================================
class LogOutput
{
public:
    virtual ~LogOutput() = default;
    virtual void Write(const std::string& formatted) = 0;
};

// 控制台输出
class ConsoleOutput : public LogOutput
{
public:
    void Write(const std::string& formatted) override
    {
        std::lock_guard<std::mutex> lock(IModule::OutputMutex());
        std::cout << formatted;
    }
};

// 文件输出
class FileOutput : public LogOutput
{
public:
    bool Open(const std::string& path)
    {
        ofs_.open(path, std::ios::app);
        return ofs_.is_open();
    }

    void Write(const std::string& formatted) override
    {
        if (ofs_.is_open())
            ofs_ << formatted;
    }

private:
    std::ofstream ofs_;
};

// 线程安全文件输出（后台线程写）
class ThreadFileOutput : public LogOutput
{
public:
    ThreadFileOutput() : running_(true), worker_(&ThreadFileOutput::WorkerLoop, this) {}

    ~ThreadFileOutput()
    {
        running_ = false;
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    bool Open(const std::string& path)
    {
        ofs_.open(path, std::ios::app);
        return ofs_.is_open();
    }

    void Write(const std::string& formatted) override
    {
        std::lock_guard lock(mutex_);
        queue_.push(formatted);
        cv_.notify_one();
    }

private:
    void WorkerLoop()
    {
        while (running_)
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
            while (!queue_.empty())
            {
                if (ofs_.is_open())
                    ofs_ << queue_.front();
                queue_.pop();
            }
        }
    }

    std::ofstream ofs_;
    std::queue<std::string> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

// =================================================================
//  Logger — 日志引擎
// =================================================================
class Logger
{
public:
    void SetFormat(std::unique_ptr<LogFormat> fmt)  { format_ = std::move(fmt); }
    void SetOutput(std::unique_ptr<LogOutput> out)  { output_ = std::move(out); }
    void SetLevel(LogLevel level)                   { level_ = level; }
    LogLevel GetLevel() const                       { return level_; }

    void Write(LogLevel level, const std::string& msg,
               const std::string& file, int line)
    {
        if (level < level_) return;       // 级别过滤
        if (!format_ || !output_) return;

        auto formatted = format_->Format(level, msg, file, line);
        output_->Write(formatted);

        // ★ 同时发射 EventBus 信号，UI 可监听 "log.written" 实时显示日志
        EventBus::GetInstance().Emit("log.written",
            LogLevelToString(level), msg.c_str(), file.c_str(), line);
    }

private:
    std::unique_ptr<LogFormat> format_;
    std::unique_ptr<LogOutput> output_;
    LogLevel level_{LogLevel::DEBUG};
};

// =================================================================
//  LogFac — 单例工厂
// =================================================================
class LogFac
{
public:
    static LogFac& Instance()
    {
        static LogFac fac;
        return fac;
    }

    Logger& GetLogger() { return logger_; }

    /// 从配置文件初始化
    /// 配置格式：key=value
    ///   log_type=console|file|thread_file
    ///   log_file=路径
    ///   log_level=debug|info|error|fatal
    ///   log_format=(空=纯文本)|xml
    void InitFromConfig(const std::string& config_path = "log.conf")
    {
        std::string log_type = "console";
        std::string log_file = "log.txt";
        std::string log_level = "debug";
        std::string log_format = "";

        // 读配置文件
        std::ifstream in(config_path);
        if (in.is_open())
        {
            std::string line;
            while (std::getline(in, line))
            {
                auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);
                // trim
                while (!key.empty() && key.back() == ' ') key.pop_back();
                while (!val.empty() && val.front() == ' ') val.erase(0, 1);

                if (key == "log_type")   log_type = val;
                if (key == "log_file")   log_file = val;
                if (key == "log_level")  log_level = val;
                if (key == "log_format") log_format = val;
            }
        }

        // 格式化器
        if (log_format == "xml")
            logger_.SetFormat(std::make_unique<XmlFormat>());
        else
            logger_.SetFormat(std::make_unique<PlainFormat>());

        // 级别
        logger_.SetLevel(StringToLogLevel(log_level));

        // 输出目标
        if (log_type == "file")
        {
            auto fout = std::make_unique<FileOutput>();
            fout->Open(log_file);
            logger_.SetOutput(std::move(fout));
        }
        else if (log_type == "thread_file")
        {
            auto fout = std::make_unique<ThreadFileOutput>();
            fout->Open(log_file);
            logger_.SetOutput(std::move(fout));
        }
        else
        {
            logger_.SetOutput(std::make_unique<ConsoleOutput>());
        }
    }

private:
    LogFac() { InitFromConfig(); }
    Logger logger_;
};

// =================================================================
//  便捷宏（代码中直接使用，不经过 EventBus）
// =================================================================
#define LOG_DEBUG(msg) LogFac::Instance().GetLogger().Write(LogLevel::DEBUG, msg, __FILE__, __LINE__)
#define LOG_INFO(msg)  LogFac::Instance().GetLogger().Write(LogLevel::INFO,  msg, __FILE__, __LINE__)
#define LOG_ERROR(msg) LogFac::Instance().GetLogger().Write(LogLevel::ERROR, msg, __FILE__, __LINE__)
#define LOG_FATAL(msg) LogFac::Instance().GetLogger().Write(LogLevel::FATAL, msg, __FILE__, __LINE__)

// =================================================================
//  LogModule — 命令接口（REGISTER_FUNC 注册到 EventBus）
// =================================================================
class LogModule : public ModuleBaseObject
{
public:
    const char* GetName() const override { return "Logger"; }

    bool OnInit() override
    {
        // 确保 LogFac 已初始化（构造函数里自动调了 InitFromConfig）
        LogFac::Instance();

        // 注册 "log.written" 信号（UI 可监听此信号实时显示日志）
        // 信号签名: (const char* level, const char* msg, const char* file, int line)
        // 用法: bus.LinkSlotFunc<const char*,const char*,const char*,int>(
        //           "log.written", [](auto l, auto m, auto f, int n) { ... });
        auto& bus = EventBus::GetInstance();
        bus.RegisterSignal<const char*, const char*, const char*, int>("log.written");

        REGISTER_FUNC("info", "记录 INFO 日志 (-v:msg|内容)", {
            auto msg = pack->Get("msg");
            if (msg)
            {
                LOG_INFO(*msg);
                pack->success = true;
                pack->return_value = "logged: " + *msg;
            }
            else
            {
                pack->success = false;
                pack->error.code = ErrorCode::INVALID_PARAMS;
                pack->error.message = "Missing -v:msg| parameter";
            }
        });

        REGISTER_FUNC("error", "记录 ERROR 日志 (-v:msg|内容)", {
            auto msg = pack->Get("msg");
            if (msg)
            {
                LOG_ERROR(*msg);
                pack->success = true;
                pack->return_value = "error logged: " + *msg;
            }
            else
            {
                pack->success = false;
                pack->error.code = ErrorCode::INVALID_PARAMS;
                pack->error.message = "Missing -v:msg| parameter";
            }
        });

        REGISTER_FUNC("debug", "记录 DEBUG 日志 (-v:msg|内容)", {
            auto msg = pack->Get("msg");
            if (msg)
            {
                LOG_DEBUG(*msg);
                pack->success = true;
                pack->return_value = "debug logged: " + *msg;
            }
            else
            {
                pack->success = false;
                pack->error.code = ErrorCode::INVALID_PARAMS;
                pack->error.message = "Missing -v:msg| parameter";
            }
        });

        REGISTER_FUNC("fatal", "记录 FATAL 日志 (-v:msg|内容)", {
            auto msg = pack->Get("msg");
            if (msg)
            {
                LOG_FATAL(*msg);
                pack->success = true;
                pack->return_value = "fatal logged: " + *msg;
            }
            else
            {
                pack->success = false;
                pack->error.code = ErrorCode::INVALID_PARAMS;
                pack->error.message = "Missing -v:msg| parameter";
            }
        });

        REGISTER_FUNC("setlevel", "设置日志级别 (-v:level|debug/info/error/fatal)", {
            auto level = pack->Get("level");
            if (level)
            {
                auto lv = StringToLogLevel(*level);
                LogFac::Instance().GetLogger().SetLevel(lv);
                pack->success = true;
                pack->return_value = std::string("Log level set to ") + LogLevelToString(lv);
            }
            else
            {
                pack->success = false;
                pack->error.code = ErrorCode::INVALID_PARAMS;
                pack->error.message = "Missing -v:level| parameter";
            }
        });

        return true;
    }
};
