/// =================================================================
///  CommandParser 实现
/// =================================================================

#include "CommandParser.h"
#include <iostream>

// ---- 辅助：去首尾空格 ----
static std::string Trim(const std::string& s)
{
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
        ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;
    return s.substr(start, end - start);
}

// =================================================================
//  TokenizeCommand — 命令字符串分词器（v2.7）
// =================================================================
//  逐字符状态机。三种状态回答一个问题：「当前空格是分隔符还是值？」
//
//  状态图：
//    NORMAL ──空格──→ 产出 token
//    NORMAL ── " ──→ IN_DQUOTE（不存引号）
//    NORMAL ── ' ──→ IN_SQUOTE（不存引号）
//    NORMAL ── \ ──→ 吞掉，下一个字符当普通字符
//    IN_DQUOTE ── " ──→ NORMAL（不存引号）
//    IN_DQUOTE ── \ ──→ 吞掉，下一个字符如果是 " 或 \ 则当普通字符
//    IN_SQUOTE ── ' ──→ NORMAL（不存引号）
//    IN_SQUOTE ── \ ──→ 吞掉，下一个字符如果是 ' 或 \ 则当普通字符
//
//  边界行为：
//    - 未闭合引号 → 到字符串末尾自动结束（不报错，宽松处理）
//    - 连续空格 → 不产生空 token（已检查 !current.empty()）
//    - 行尾 \ → 保留字面反斜杠（不做续行）
//    - 空输入 → 返回空 vector
std::vector<std::string> TokenizeCommand(const std::string& input)
{
    std::vector<std::string> tokens;
    std::string current;

    enum State { NORMAL, IN_DQUOTE, IN_SQUOTE };
    State state = NORMAL;

    for (size_t i = 0; i < input.size(); ++i)
    {
        char ch = input[i];

        switch (state)
        {
        case NORMAL:
            if (ch == '\\' && i + 1 < input.size())
            {
                char next = input[i + 1];
                // 只在转义这些特殊字符时吞掉反斜杠
                // \" → "   \' → '   \\ → \   \空格 → 空格
                // 其他（如 \d → \d）保留反斜杠，保证 Windows 路径不坏
                if (next == '"' || next == '\'' || next == '\\' ||
                    std::isspace(static_cast<unsigned char>(next)))
                {
                    current += next;
                    ++i;
                }
                else
                {
                    current += ch;  // 保留字面反斜杠
                }
            }
            else if (ch == '"')
            {
                state = IN_DQUOTE;  // 不存引号
            }
            else if (ch == '\'')
            {
                state = IN_SQUOTE;  // 不存引号
            }
            else if (std::isspace(static_cast<unsigned char>(ch)))
            {
                // 空白 → 切分
                if (!current.empty())
                {
                    tokens.push_back(current);
                    current.clear();
                }
            }
            else
            {
                current += ch;
            }
            break;

        case IN_DQUOTE:
            if (ch == '\\' && i + 1 < input.size())
            {
                char next = input[i + 1];
                if (next == '"' || next == '\\')
                {
                    // \" 变 " , \\ 变 \（反斜杠）
                    current += next;
                    ++i;
                }
                else
                {
                    // 其他字符前的 \ 保留字面反斜杠
                    current += ch;
                }
            }
            else if (ch == '"')
            {
                state = NORMAL;  // 关闭引号，不存引号本身
            }
            else
            {
                current += ch;  // 引号内所有字符（含空格）直接追加
            }
            break;

        case IN_SQUOTE:
            if (ch == '\\' && i + 1 < input.size())
            {
                char next = input[i + 1];
                if (next == '\'' || next == '\\')
                {
                    // \' 变 ' , \\ 变 \（反斜杠）
                    current += next;
                    ++i;
                }
                else
                {
                    current += ch;
                }
            }
            else if (ch == '\'')
            {
                state = NORMAL;  // 关闭引号，不存引号本身
            }
            else
            {
                current += ch;  // 引号内所有字符直接追加
            }
            break;
        }
    }

    // 最后一个 token（字符串结束 = 隐式切分）
    if (!current.empty())
    {
        tokens.push_back(current);
    }

    return tokens;
}

// =================================================================
//  构造函数：注册内置格式 + 默认命令头
// =================================================================
CommandParser::CommandParser()
{
    // ---- 注册默认命令头 ----
    RegisterHead("-m:", [](ParmarPack* p, const std::string& val) {
        if (p) p->mod_id = Trim(val);
    });

    RegisterHead("-f:", [](ParmarPack* p, const std::string& val) {
        if (p) p->func_id = Trim(val);
    });

    RegisterHead("-v:", [](ParmarPack* p, const std::string& val) {
        if (!p) return;
        // 解析: key1|val1,key2|val2,...
        size_t pos = 0;
        while (pos < val.size())
        {
            auto comma = val.find(',', pos);
            if (comma == std::string::npos) comma = val.size();

            std::string pair = val.substr(pos, comma - pos);
            auto pipe = pair.find('|');
            if (pipe != std::string::npos)
            {
                std::string k = Trim(pair.substr(0, pipe));
                std::string v = Trim(pair.substr(pipe + 1));
                p->params[k].push_back(v);
            }
            pos = comma + 1;
        }
    });

    // ---- 注册内置格式处理器 ----
    // TXT: 控制台文本解析（需要访问 head_writers_，通过 lambda 捕获 this）
    auto& heads = head_writers_;  // 引用，给 lambda 捕获用
    RegisterFormat("TXT", [&heads](std::any& value, LockQueue<ParmarPack>& queue) -> bool {
        return HandleTXT(value, queue, heads);
    });
    // 注意：没有 "RAW" 格式。UI 直传用 SendPack() 方法，不走 std::any。
    // 因为 unique_ptr 不可拷贝，塞不进 std::any（C++ 标准要求 is_copy_constructible）。
}

// =================================================================
//  SendCommand — 统一入口
// =================================================================
bool CommandParser::SendCommand(const std::string& format, std::any value)
{
    auto it = format_handlers_.find(format);
    if (it == format_handlers_.end())
    {
        std::cerr << "[Parser] Unknown format: " << format << "\n";
        return false;
    }

    if (!it->second)
    {
        std::cerr << "[Parser] Format handler is null: " << format << "\n";
        return false;
    }

    return it->second(value, queue_);
}

// =================================================================
//  HandleTXT — 文本格式解析
// =================================================================
//  TokenizeCommand 切 token → 找 ":" 切出头和值 → 查 head_writers_ → 填充 ParmarPack
//
//  v2.7: 用 TokenizeCommand 替换 iss >> token，支持引号保护空格和反斜杠转义。
bool CommandParser::HandleTXT(std::any& value, LockQueue<ParmarPack>& queue,
                               const std::unordered_map<std::string, CmdWriter>& heads)
{
    // 从 std::any 取出字符串
    std::string text;
    try
    {
        text = std::any_cast<std::string>(value);
    }
    catch (const std::bad_any_cast&)
    {
        std::cerr << "[Parser] TXT format requires std::string\n";
        return false;
    }

    if (text.empty()) return false;

    auto pack = std::make_unique<ParmarPack>();

    for (const auto& token : TokenizeCommand(text))
    {
        auto colon = token.find(':');
        if (colon == std::string::npos)
        {
            std::cerr << "[Parser] Token missing ':': " << token << "\n";
            continue;
        }

        std::string head  = token.substr(0, colon + 1);  // e.g. "-m:"
        std::string value = token.substr(colon + 1);

        auto it = heads.find(head);
        if (it == heads.end())
        {
            std::cerr << "[Parser] Unknown flag: " << head << "\n";
            continue;
        }
        it->second(pack.get(), value);
    }

    if (pack->mod_id.empty())
    {
        std::cerr << "[Parser] Missing module id (-m:).\n";
        return false;
    }

    queue.Push(std::move(pack));
    return true;
}

// =================================================================
//  SendPack — UI/代码直通入口（不经过 std::any）
// =================================================================
//  为什么不用 std::any？因为 unique_ptr 不可拷贝，塞不进 std::any。
//  C++ 标准要求 std::any 存储的类型满足 is_copy_constructible。
bool CommandParser::SendPack(std::unique_ptr<ParmarPack> pack)
{
    if (!pack)
    {
        std::cerr << "[Parser] SendPack: pack is null.\n";
        return false;
    }

    if (pack->mod_id.empty())
    {
        std::cerr << "[Parser] SendPack: pack->mod_id is empty.\n";
        return false;
    }

    queue_.Push(std::move(pack));
    return true;
}
