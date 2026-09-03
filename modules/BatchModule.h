/// =================================================================
///  BatchModule — 多分片批量文件处理模块 (v2.8)
/// =================================================================
///
///  这是 test_shell 第一个真正用上框架「三件套」的真实模块：
///    1. 多分片工作流（Task::PushShard 动态追加分片）
///    2. 暂停 / 恢复（分片边界生效，Pause/Resume）
///    3. 持久化恢复（ExportRecord/Restore + FilePersistence，重启从断点继续）
///
///  之前所有内置/示例模块（Print/Log/Metrics/TaskManager/Calculator）都是
///  单分片、无状态、从不暂停——单分片命令用普通函数调用就够了，体现不出
///  框架为「长流程、可暂停、可恢复」而生的价值。本模块填补这个空缺。
///
///  【设计原则】分片自包含 + 一次性 push 全部分片 + 模块无状态
///    - 每个分片自带 {file, start, chunk}，存在 ParmarPack params 里
///      （不存模块成员变量）→ 跟着分片序列化 → 重启后自动恢复
///    - 入口分片执行时枚举目录 + 一次性 PushShard 所有 chunk 分片 +
///      SetTotalShards → 暂停时未处理分片已在 shards_ 里，ExportRecord
///      全序列化，Restore 后从 current_shard 继续
///    - 模块不持有任何跨分片状态 → 重启后模块是新对象也无所谓，
///      恢复纯靠框架机制，模块代码零特殊处理
///
///  【命令】
///    -m:Batch -f:count -v:dir|./logs,chunk|1000
///      dir   = 要统计的目录（默认当前目录）
///      chunk = 每分片处理的行数（默认 1000）
///
///  【分片结构】
///    shards_[0]   = {mod:"Batch", func:"count", params:{dir, chunk}}   入口分片
///    shards_[1..N]= {mod:"Batch", func:"chunk", params:{file, start, chunk}}  每块一个
///
///  【暂停 / 恢复 / 重启】
///    -m:TaskManager -f:pause -v:id|N    暂停在某个 chunk 边界
///    -m:TaskManager -f:list             看暂停在第几个 chunk
///    /exit                              退出，快照落盘（配 FilePersistence）
///    (重启进程)
///    -m:TaskManager -f:resume -v:id|N   从断点 chunk 继续，不重复已完成的块
/// =================================================================

#pragma once
#include <fstream>
#include <string>
#include "sdk/sdk.h"
#include "core/platform/file_system.h"  // platform::FindFiles

class BatchModule : public ModuleBase
{
public:
    const char* GetName() const override { return "Batch"; }

    bool OnInit() override
    {
        // ============================================================
        //  入口分片：枚举目录 + 一次性 push 所有 chunk 分片
        // ============================================================
        REGISTER_FUNC("count", "Count lines in a dir (-v:dir|D,chunk|N)", {
            std::string dir = pack->GetOr("dir", ".");
            int chunk = pack->GetAsOr<int>("chunk", 1000);
            if (chunk <= 0) chunk = 1000;

            // 枚举目录下所有文件（FindFiles 返回完整路径）
            auto files = platform::FindFiles(dir, "*");

            size_t total_chunks = 0;
            for (const auto& file : files) {
                // 数该文件总行数，算需要几块（向上取整）
                size_t lines = CountLines(file);
                size_t blocks = (lines + static_cast<size_t>(chunk) - 1)
                                / static_cast<size_t>(chunk);
                if (blocks == 0) blocks = 1;   // 空文件至少 1 块（读到 0 行）

                for (size_t b = 0; b < blocks; ++b) {
                    auto shard = std::make_unique<ParmarPack>();
                    shard->mod_id  = "Batch";
                    shard->func_id = "chunk";
                    shard->show_explanation = false;  // 避免每分片打标题噪声
                    shard->Set("file",  file);
                    shard->Set("start", std::to_string(b * static_cast<size_t>(chunk)));
                    shard->Set("chunk", std::to_string(chunk));
                    pack->owner_task->PushShard(std::move(shard));
                    ++total_chunks;
                }
            }

            // 预声明总分片数 = 1（入口）+ N（chunk），进度条从第一帧就准
            pack->owner_task->SetTotalShards(1 + total_chunks);

            SetResult(pack, "queued " + std::to_string(files.size())
                          + " files into " + std::to_string(total_chunks) + " chunks");
        });

        // ============================================================
        //  chunk 分片：读一块，数行数
        // ============================================================
        REGISTER_FUNC("chunk", "process one chunk of a file", {
            std::string file = pack->GetOr("file", "");
            size_t start = static_cast<size_t>(pack->GetAsOr<long long>("start", 0));
            int chunk = pack->GetAsOr<int>("chunk", 1000);
            if (chunk <= 0) chunk = 1000;

            if (file.empty()) {
                SetError(pack, ErrorCode::INVALID_PARAMS, "no file in shard");
                return;
            }

            std::ifstream f(file);
            if (!f) {
                SetError(pack, ErrorCode::INTERNAL_ERROR, "cannot open " + file);
                return;
            }

            // 跳过前 start 行，读接下来 chunk 行
            std::string line;
            size_t skipped = 0;
            while (skipped < start && std::getline(f, line)) ++skipped;

            int got = 0;
            while (got < chunk && std::getline(f, line)) ++got;

            SetResult(pack, file + ":" + std::to_string(start) + "-"
                          + std::to_string(start + static_cast<size_t>(got))
                          + " lines=" + std::to_string(got));
        });

        return true;
    }

private:
    /// 数一个文件的总行数（入口分片规划分块数用）
    static size_t CountLines(const std::string& path) {
        std::ifstream f(path);
        if (!f) return 0;
        size_t n = 0;
        std::string line;
        while (std::getline(f, line)) ++n;
        return n;
    }
};