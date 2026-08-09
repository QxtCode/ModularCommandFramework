#pragma once
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include "core/LockQueue.h"
#include "core/ParmarPack.h"

// ============================================================
//  ConsoleParser — 文本命令解析器（单例）【已废弃】
// ============================================================
//
//  新版请用 CommandParser（支持多源输入：控制台 / Qt / JSON 等）。
//  这个保留是为了兼容旧测试，新代码不要再依赖它。
//
//  输入格式: "-m:模块名 -f:函数ID -v:key|val,key|val..."
//  输出:     ParmarPack 入队，外部通过 PopPack() 取出

class ConsoleParser
{
public:
    using CmdWriter = std::function<void(ParmarPack*, const std::string&)>;

    static ConsoleParser& Get()
    {
        static ConsoleParser parser;
        return parser;
    }

    /// 从队列取出一个解析好的 ParmarPack（阻塞）
    std::unique_ptr<ParmarPack> PopPack() { return queue_.Pop(); }

    /// 非阻塞取出
    bool TryPopPack(std::unique_ptr<ParmarPack>& out) { return queue_.TryPop(out); }

    /// 解析文本并入库
    /// @param text  命令文本，格式 "-m:X -f:Y -v:k|v,k|v"
    /// @return 成功返回 true
    bool Send(const std::string& text);

    /// 注册自定义命令头处理函数（如 "-x:"）
    void RegisterHead(const std::string& head, CmdWriter writer);

    ConsoleParser(const ConsoleParser&) = delete;
    ConsoleParser& operator=(const ConsoleParser&) = delete;

private:
    ConsoleParser();
    LockQueue<ParmarPack> queue_;
    std::unordered_map<std::string, CmdWriter> head_writers_;
};
