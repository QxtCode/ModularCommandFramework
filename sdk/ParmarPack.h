/// =================================================================
///  ParmarPack — 模块之间传递数据的信封
/// =================================================================
///
///  速查:
///    pack->Set("key", "val")          设值
///    pack->Get("key")                 取值 (可能为空)
///    pack->GetOr("key", "def")        取值，空的给默认
///    pack->GetAsOr<int>("a", 0)       取数字
///    pack->GetAsOr<bool>("f")         取布尔
///    pack->Has("key")                 有这键没
///    pack->GetAll("key")              取全部值
///
///  v2.3 新 API —— 以前要写四行 find→end→empty→[0]，现在一行。

#pragma once
#include <cctype>    // isalnum
#include <cstdio>    // snprintf
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

// ============================================================
//  Standard error codes
// ============================================================
namespace ErrorCode
{
    constexpr int OK                = 0;
    constexpr int MODULE_NOT_FOUND  = 404;
    constexpr int FUNC_NOT_FOUND    = 405;
    constexpr int SIGNAL_NOT_FOUND  = 406;
    constexpr int INVALID_PARAMS    = 400;
    constexpr int DLL_LOAD_FAILED   = 501;
    constexpr int INTERNAL_ERROR    = 500;
}

class Task;  // forward

class ParmarPack
{
public:
    // ============================================================
    //  Data fields (public — backward compatible)
    // ============================================================
    std::string mod_id;
    std::string func_id;
    std::unordered_map<std::string, std::vector<std::string>> params;

    bool        success = false;
    std::string return_value;

    struct Error
    {
        int         code = 0;
        std::string message;
    } error;

    Task* owner_task       = nullptr;
    bool  show_explanation = true;

    // ============================================================
    //  Convenience accessors (v2.3)
    // ============================================================

    /// Set a single value. Equivalent to params[key] = {val}.
    void Set(const std::string& key, const std::string& val)
    {
        params[key] = {val};
    }

    /// Check if a key exists and has at least one value.
    bool Has(const std::string& key) const
    {
        auto it = params.find(key);
        return it != params.end() && !it->second.empty();
    }

    /// Get the first value as optional<string>.
    /// Returns nullopt if key not found or empty.
    std::optional<std::string> Get(const std::string& key) const
    {
        auto it = params.find(key);
        if (it != params.end() && !it->second.empty())
            return it->second[0];
        return std::nullopt;
    }

    /// Get the first value, or return a default if missing.
    std::string GetOr(const std::string& key,
                      const std::string& def = "") const
    {
        auto v = Get(key);
        return v ? *v : def;
    }

    /// Get all values for a key (empty vector if key not found).
    const std::vector<std::string>& GetAll(const std::string& key) const
    {
        static const std::vector<std::string> empty;
        auto it = params.find(key);
        return (it != params.end()) ? it->second : empty;
    }

    // ============================================================
    //  Typed access: GetAs<T>(key) → optional<T>
    // ============================================================
    /// Get value as type T. Returns nullopt if missing or unparseable.
    /// Supported types: int, long, long long, unsigned,
    ///                   float, double, bool, std::string.
    template<typename T>
    std::optional<T> GetAs(const std::string& key) const
    {
        auto raw = Get(key);
        if (!raw) return std::nullopt;
        return Parse<T>(*raw);
    }

    /// Get value as type T, or return a default if missing/unparseable.
    template<typename T>
    T GetAsOr(const std::string& key, const T& def = T{}) const
    {
        auto v = GetAs<T>(key);
        return v ? *v : def;
    }

    // ============================================================
    //  序列化 (v2.7) — 快照存储用
    // ============================================================
    /// 序列化为 JSON 字符串（mod_id / func_id / params / success / return_value）。
    std::string ToJson() const;

    /// 从 JSON 反序列化。失败返回 false，out 内容未定义。
    static bool FromJson(const std::string& json, ParmarPack& out);

private:
    // ---- Type conversion helpers ----
    template<typename T>
    static std::optional<T> Parse(const std::string& s)
    {
        if constexpr (std::is_same_v<T, std::string>)
        {
            return s;
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            return (s == "true" || s == "1" || s == "yes");
        }
        else if constexpr (std::is_integral_v<T>)
        {
            try {
                if constexpr (std::is_same_v<T, int>)
                    return std::stoi(s);
                else if constexpr (std::is_same_v<T, long>)
                    return std::stol(s);
                else if constexpr (std::is_same_v<T, long long>)
                    return std::stoll(s);
                else if constexpr (std::is_same_v<T, unsigned long>)
                    return std::stoul(s);
                else if constexpr (std::is_same_v<T, unsigned long long>)
                    return std::stoull(s);
                else
                    return static_cast<T>(std::stoll(s));
            } catch (...) {
                return std::nullopt;
            }
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
            try {
                if constexpr (std::is_same_v<T, float>)
                    return std::stof(s);
                else
                    return std::stod(s);
            } catch (...) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }
};

// =================================================================
//  序列化实现 (v2.7) — 手写 JSON 子集，供快照存储使用
// =================================================================
//
//  为什么要手写而不引入 JSON 库：项目只依赖 FTXUI + GoogleTest，
//  快照只需覆盖 ParmarPack 的固定 schema（string / bool / 对象 / 字符串数组）。
//  这里实现一个"够用且正确"的递归下降解析器，支持字符串转义。
//
//  schema：
//    {"mod":"...","func":"...","params":{"k":["v1","v2"],...},
//     "success":true,"ret":"..."}
// =================================================================

namespace parmar_json {

/// 把字符串转义成带引号的 JSON 字符串字面量。
inline std::string Escape(const std::string& s)
{
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

/// 跳过空白字符。
inline void SkipWs(const std::string& s, size_t& i)
{
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' ||
                            s[i] == '\n' || s[i] == '\r'))
        ++i;
}

/// 解析一个 JSON 字符串字面量（含反转义）。i 必须指向开引号 '"'。
/// 成功返回 true 并把内容写入 out，i 停在结束引号之后。
inline bool ParseString(const std::string& s, size_t& i, std::string& out)
{
    if (i >= s.size() || s[i] != '"') return false;
    ++i;  // 跳过开引号
    out.clear();
    while (i < s.size()) {
        char c = s[i];
        if (c == '"') { ++i; return true; }        // 结束引号
        if (c == '\\') {
            ++i;
            if (i >= s.size()) return false;
            char e = s[i];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    // \uXXXX（按字节写入，覆盖 ASCII + UTF-8 片段）
                    if (i + 4 >= s.size()) return false;
                    unsigned code = 0;
                    for (int k = 1; k <= 4; ++k) {
                        char h = s[i + k];
                        code <<= 4;
                        if      (h >= '0' && h <= '9') code |= (h - '0');
                        else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
                        else return false;
                    }
                    out += static_cast<char>(code);
                    i += 4;
                    break;
                }
                default: return false;
            }
            ++i;
        } else {
            out += c;
            ++i;
        }
    }
    return false;  // 没遇到结束引号
}

/// 解析字符串数组 [ "a", "b" ]，结果写入 out。
inline bool ParseStringArray(const std::string& s, size_t& i,
                             std::vector<std::string>& out)
{
    SkipWs(s, i);
    if (i >= s.size() || s[i] != '[') return false;
    ++i;
    SkipWs(s, i);
    if (i < s.size() && s[i] == ']') { ++i; return true; }  // 空数组
    while (true) {
        std::string v;
        if (!ParseString(s, i, v)) return false;
        out.push_back(std::move(v));
        SkipWs(s, i);
        if (i >= s.size()) return false;
        if (s[i] == ',') { ++i; SkipWs(s, i); continue; }
        if (s[i] == ']') { ++i; return true; }
        return false;
    }
}

/// 跳过任意一个 JSON 值（用于忽略未知字段）。
inline bool SkipValue(const std::string& s, size_t& i)
{
    SkipWs(s, i);
    if (i >= s.size()) return false;
    char c = s[i];
    if (c == '"') { std::string dummy; return ParseString(s, i, dummy); }
    if (c == '{') {
        ++i; SkipWs(s, i);
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            std::string k; if (!ParseString(s, i, k)) return false;
            SkipWs(s, i); if (i >= s.size() || s[i] != ':') return false; ++i;
            if (!SkipValue(s, i)) return false;
            SkipWs(s, i); if (i >= s.size()) return false;
            if (s[i] == ',') { ++i; SkipWs(s, i); continue; }
            if (s[i] == '}') { ++i; return true; }
            return false;
        }
    }
    if (c == '[') {
        ++i; SkipWs(s, i);
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        while (true) {
            if (!SkipValue(s, i)) return false;
            SkipWs(s, i); if (i >= s.size()) return false;
            if (s[i] == ',') { ++i; SkipWs(s, i); continue; }
            if (s[i] == ']') { ++i; return true; }
            return false;
        }
    }
    // 标量：number / true / false / null
    while (i < s.size() &&
           (std::isalnum(static_cast<unsigned char>(s[i])) ||
            s[i] == '-' || s[i] == '+' || s[i] == '.' ||
            s[i] == 'e' || s[i] == 'E'))
        ++i;
    return true;
}

}  // namespace parmar_json

inline std::string ParmarPack::ToJson() const
{
    std::string out = "{\"mod\":";
    out += parmar_json::Escape(mod_id);
    out += ",\"func\":";
    out += parmar_json::Escape(func_id);
    out += ",\"params\":{";
    bool first_key = true;
    for (const auto& [k, v] : params) {
        if (!first_key) out += ',';
        first_key = false;
        out += parmar_json::Escape(k);
        out += ':';
        out += '[';
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) out += ',';
            out += parmar_json::Escape(v[i]);
        }
        out += ']';
    }
    out += "},\"success\":";
    out += success ? "true" : "false";
    out += ",\"ret\":";
    out += parmar_json::Escape(return_value);
    out += '}';
    return out;
}

inline bool ParmarPack::FromJson(const std::string& json, ParmarPack& out)
{
    ParmarPack tmp;
    size_t i = 0;
    parmar_json::SkipWs(json, i);
    if (i >= json.size() || json[i] != '{') return false;
    ++i;
    parmar_json::SkipWs(json, i);
    if (i < json.size() && json[i] == '}') { ++i; goto done; }  // 空对象

    while (true) {
        std::string key;
        if (!parmar_json::ParseString(json, i, key)) return false;
        parmar_json::SkipWs(json, i);
        if (i >= json.size() || json[i] != ':') return false;
        ++i;
        parmar_json::SkipWs(json, i);

        if (key == "mod" || key == "func" || key == "ret") {
            std::string v;
            if (!parmar_json::ParseString(json, i, v)) return false;
            if      (key == "mod") tmp.mod_id = std::move(v);
            else if (key == "func") tmp.func_id = std::move(v);
            else                    tmp.return_value = std::move(v);
        } else if (key == "success") {
            if (i + 4 <= json.size() && json.compare(i, 4, "true") == 0) {
                tmp.success = true; i += 4;
            } else if (i + 5 <= json.size() && json.compare(i, 5, "false") == 0) {
                tmp.success = false; i += 5;
            } else {
                return false;
            }
        } else if (key == "params") {
            parmar_json::SkipWs(json, i);
            if (i >= json.size() || json[i] != '{') return false;
            ++i;
            parmar_json::SkipWs(json, i);
            if (i < json.size() && json[i] == '}') { ++i; }  // 空 params
            else {
                while (true) {
                    std::string pk;
                    if (!parmar_json::ParseString(json, i, pk)) return false;
                    parmar_json::SkipWs(json, i);
                    if (i >= json.size() || json[i] != ':') return false;
                    ++i;
                    std::vector<std::string> vals;
                    if (!parmar_json::ParseStringArray(json, i, vals)) return false;
                    tmp.params[std::move(pk)] = std::move(vals);
                    parmar_json::SkipWs(json, i);
                    if (i >= json.size()) return false;
                    if (json[i] == ',') { ++i; parmar_json::SkipWs(json, i); continue; }
                    if (json[i] == '}') { ++i; break; }
                    return false;
                }
            }
        } else {
            // 未知字段：跳过整个值（向前兼容）
            if (!parmar_json::SkipValue(json, i)) return false;
        }

        parmar_json::SkipWs(json, i);
        if (i >= json.size()) return false;
        if (json[i] == ',') { ++i; parmar_json::SkipWs(json, i); continue; }
        if (json[i] == '}') { ++i; break; }
        return false;
    }

done:
    parmar_json::SkipWs(json, i);
    if (i != json.size()) return false;  // 后面还有多余内容

    out = std::move(tmp);
    return true;
}
