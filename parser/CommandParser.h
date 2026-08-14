#pragma once
#include <any>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/LockQueue.h"
#include "core/ParmarPack.h"

// =================================================================
//  CommandParser — 多源命令解析器（单例）
// =================================================================
//
//  在数据流中的位置：输入源 → CommandParser → PopPack → ShellEngine::SubmitTask
//  ShellEngine::ProcessInput 是消费者，通过 TryPopPack 非阻塞取解析结果。
//
//  【设计思想：为什么有两个入口？】
//
//  命令可以来自任何地方：
//    - 控制台输入   → 文本字符串
//    - Qt 按钮点击   → 预构造的 ParmarPack
//    - 网络请求     → JSON / XML
//
//  SendCommand("TXT", std::any)   — 万能入口，适合文本/JSON/XML 等可拷贝数据
//  SendPack(unique_ptr<ParmarPack>) — UI 直通入口，绕过 std::any（因为
//                                    unique_ptr 不可拷贝，塞不进 std::any）
//
//  【为什么 unique_ptr 不能放 std::any？】
//  C++ 标准要求 std::any 存储的类型必须是可拷贝构造的（is_copy_constructible）。
//  unique_ptr 是只移动类型，不符合这个要求。
//  所以 UI 直传 Pack 用专用的 SendPack()，不走 std::any。
//
//  【UI 零入侵】
//  Qt 代码只需要：
//    #include "parser/CommandParser.h"
//    auto pack = std::make_unique<ParmarPack>();
//    pack->mod_id = "Calculator";  pack->func_id = "add";
//    CommandParser::Get().SendPack(std::move(pack));
//  不需要知道 ModuleLifeManager、EventBus、Task 的存在。
// =================================================================

class CommandParser
{
public:
    // ============================================================
    //  格式处理器：接收 std::any 数据 → 解析 → 入队 → 返回是否成功
    // ============================================================
    //  参数 value 是 std::any&（非 const），允许移动其中的 unique_ptr 出去。
    //  返回 true 表示成功解析并入队，false 表示解析失败。
    using FormatHandler = std::function<bool(std::any& value, LockQueue<ParmarPack>& queue)>;

    // ============================================================
    //  命令头写入器（-m:, -f:, -v: 的处理函数）
    // ============================================================
    using CmdWriter = std::function<void(ParmarPack*, const std::string&)>;

    // ---- 单例 ----
    static CommandParser& Get()
    {
        static CommandParser parser;
        return parser;
    }

    // ============================================================
    //  ★ 入口 1：SendCommand — 万能格式入口（用于可拷贝数据）
    // ============================================================
    /// @param format  格式标识，如 "TXT"、"JSON"
    /// @param value   命令数据，必须是可拷贝类型（string、json 对象等）
    /// @return true: 成功解析并入队 / false: 格式未注册或解析失败
    ///
    /// 示例：
    ///   parser.SendCommand("TXT", std::string("-m:Calc -f:add -v:a|1,b|2"));
    ///   parser.RegisterFormat("JSON", myJsonHandler);
    ///   parser.SendCommand("JSON", json_object);
    bool SendCommand(const std::string& format, std::any value);

    // ============================================================
    //  ★ 入口 2：SendPack — UI/代码直通入口（unique_ptr 不能放 any）
    // ============================================================
    /// 直接提交一个 ParmarPack。UI 代码的最佳入口。
    ///
    /// 示例（Qt 按钮点击）：
    ///   auto pack = std::make_unique<ParmarPack>();
    ///   pack->mod_id  = "Calculator";
    ///   pack->func_id = "add";
    ///   pack->params["a"].push_back("10");
    ///   parser.SendPack(std::move(pack));
    bool SendPack(std::unique_ptr<ParmarPack> pack);

    // ============================================================
    //  注册格式处理器（扩展新输入源）
    // ============================================================
    /// 注册一个自定义格式处理器。
    /// 例如注册 JSON 处理器后，就可以 SendCommand("JSON", json_data)。
    void RegisterFormat(const std::string& format, FormatHandler handler)
    {
        if (!format.empty() && handler)
            format_handlers_[format] = std::move(handler);
    }

    // ============================================================
    //  队列操作（和以前 ConsoleParser 一样）
    // ============================================================
    std::unique_ptr<ParmarPack> PopPack() { return queue_.Pop(); }
    bool TryPopPack(std::unique_ptr<ParmarPack>& out) { return queue_.TryPop(out); }

    // ============================================================
    //  注册自定义命令头（扩展解析语法）
    // ============================================================
    /// 注册自定义命令头，如 "-d:" → 解析为自定义参数
    void RegisterHead(const std::string& head, CmdWriter writer)
    {
        if (!head.empty() && writer)
            head_writers_[head] = std::move(writer);
    }

    // ---- 单例保护 ----
    CommandParser(const CommandParser&) = delete;
    CommandParser& operator=(const CommandParser&) = delete;
    CommandParser(CommandParser&&) = delete;
    CommandParser& operator=(CommandParser&&) = delete;

private:
    CommandParser();

    // ---- 内置格式处理器 ----

    /// "TXT" 格式：解析控制台文本 "-m:X -f:Y -v:key|val,..."
    static bool HandleTXT(std::any& value, LockQueue<ParmarPack>& queue,
                          const std::unordered_map<std::string, CmdWriter>& heads);

    // ---- 成员 ----
    LockQueue<ParmarPack> queue_;
    std::unordered_map<std::string, FormatHandler> format_handlers_;
    std::unordered_map<std::string, CmdWriter> head_writers_;
};

// =================================================================
//  TokenizeCommand — 命令字符串分词器（v2.7）
// =================================================================
//  把一行命令字符串切成 token 数组，支持引号保护和反斜杠转义。
//
//  规则：
//    - 空白字符（空格/Tab）作为 token 分隔符
//    - "..." 双引号内空格不切分，引号本身不进入 token
//    - '...' 单引号内空格不切分，引号本身不进入 token
//    - \ 转义下一个字符（\ 变成空格，\" 变成 "，\\ 变成 \）
//    - 未闭合的引号到字符串末尾自动结束
//
//  示例：
//    "-m:Print -f:2"               → ["-m:Print", "-f:2"]
//    "-v:msg|\"hello world\""       → ["-v:msg|hello world"]
//    "-v:msg|'hello world'"         → ["-v:msg|hello world"]
//
//  与 HandleTXT 的关系：
//    这是纯函数，输入字符串，输出字符串数组。
//    不涉及 ParmarPack、队列、格式处理器 —— 完全隔离，可独立测试。
std::vector<std::string> TokenizeCommand(const std::string& input);
