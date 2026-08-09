/// =================================================================
///  ConsoleParser 实现【已废弃，保留兼容旧测试】
/// =================================================================

#include "ConsoleParser.h"
#include <sstream>
#include <iostream>

// ---- 辅助：去空格 ----
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

ConsoleParser::ConsoleParser()
{
    // 默认注册三个命令头: -m:  -f:  -v:
    RegisterHead("-m:", [](ParmarPack* p, const std::string& val) {
        if (p) p->mod_id = Trim(val);
    });

    RegisterHead("-f:", [](ParmarPack* p, const std::string& val) {
        if (p) p->func_id = Trim(val);
    });

    RegisterHead("-v:", [](ParmarPack* p, const std::string& val) {
        if (!p) return;
        // 格式: key|val,key2|val2,...
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
}

void ConsoleParser::RegisterHead(const std::string& head, CmdWriter writer)
{
    if (!head.empty() && writer)
        head_writers_[head] = std::move(writer);
}

bool ConsoleParser::Send(const std::string& text)
{
    if (text.empty()) return false;

    auto pack = std::make_unique<ParmarPack>();
    std::istringstream iss(text);
    std::string token;

    while (iss >> token)
    {
        auto colon = token.find(':');
        if (colon == std::string::npos)
        {
            std::cerr << "[Parser] Token missing ':': " << token << "\n";
            continue;
        }

        std::string head = token.substr(0, colon + 1);  // e.g. "-m:"
        std::string value = token.substr(colon + 1);

        auto it = head_writers_.find(head);
        if (it == head_writers_.end())
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

    queue_.Push(std::move(pack));
    return true;
}
