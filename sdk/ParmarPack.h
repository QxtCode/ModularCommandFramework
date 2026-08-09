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
