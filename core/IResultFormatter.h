/// =================================================================
///  IResultFormatter — 结果翻译器（策略模式）
/// =================================================================
///
///  模块只管往 ParmarPack 里写结果（success / return_value / error），
///  翻译的事框架负责。换 UI 就换一个 Formatter，模块不用改。
///
///  已实现:
///    ConsoleFormatter  →  "[OK] 300"  /  "[ERR 404] Module not found"
///    JsonFormatter     →  {"ok":true,"data":"300"}
///    QuietFormatter    →  "" (静默模式)

#pragma once
#include <string>
#include "sdk/ParmarPack.h"

class IResultFormatter
{
public:
    virtual ~IResultFormatter() = default;
    virtual std::string Format(const ParmarPack& pack) const = 0;
};

// ================================================================
//  ConsoleFormatter — 给人看的控制台输出
// ================================================================
class ConsoleFormatter : public IResultFormatter
{
public:
    std::string Format(const ParmarPack& pack) const override
    {
        if (pack.success)
            return "[OK] " + pack.return_value;
        else
            return "[ERR " + std::to_string(pack.error.code) + "] "
                   + TranslateError(pack.error.code) + "\n  "
                   + pack.error.message;
    }

private:
    static const char* TranslateError(int code)
    {
        switch (code) {
            case ErrorCode::MODULE_NOT_FOUND: return "Module not found";
            case ErrorCode::FUNC_NOT_FOUND:   return "Function not found";
            case ErrorCode::SIGNAL_NOT_FOUND: return "Signal not registered";
            case ErrorCode::INVALID_PARAMS:   return "Invalid parameters";
            case ErrorCode::DLL_LOAD_FAILED:  return "DLL load failed";
            case ErrorCode::INTERNAL_ERROR:   return "Internal error";
            default:                          return "Unknown error";
        }
    }
};

// ================================================================
//  JsonFormatter — 给机器读的 JSON 输出（Web / API）
// ================================================================
class JsonFormatter : public IResultFormatter
{
public:
    std::string Format(const ParmarPack& pack) const override
    {
        std::string json = "{\"ok\":";
        json += pack.success ? "true" : "false";

        if (pack.success)
        {
            json += ",\"data\":\"" + Escape(pack.return_value) + "\"";
        }
        else
        {
            json += ",\"error\":{";
            json += "\"code\":" + std::to_string(pack.error.code) + ",";
            json += "\"message\":\"" + Escape(pack.error.message) + "\"";
            json += "}";
        }
        json += "}";
        return json;
    }

private:
    static std::string Escape(const std::string& s)
    {
        std::string out;
        for (char c : s)
        {
            if (c == '"')  out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else           out += c;
        }
        return out;
    }
};

// ================================================================
//  QuietFormatter — 不出声（后台/静默模式）
// ================================================================
class QuietFormatter : public IResultFormatter
{
public:
    std::string Format(const ParmarPack&) const override { return ""; }
};
