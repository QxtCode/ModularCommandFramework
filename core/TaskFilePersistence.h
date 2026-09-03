#pragma once
#include"ITaskPersistence.h"
#include<fstream>
#include<mutex>
#include<filesystem>
class TaskFilePersistence :public ITaskPersistence
{

public:
    explicit TaskFilePersistence(const std::string & dir)
        :dir_(dir)
    {
        std::filesystem::create_directories(dir_);//创建目录确保目录存在
    }

    ///保存/更新Task快照幂等可重复调用覆盖同一task_id
    void Save(const TaskRecord& record) override;

    ///读取Task快照不存在返回nullopt
    std::optional<TaskRecord> Load(uint32_t task_id) const override;

    ///删除Task快照(COMPLETED 后调用)
    void Delete(uint32_t task_id) override;

    ///加载所有非终态记录(启动恢复用)
     std::vector<TaskRecord> LoadAll() const override;

    ///清理终态记(定期调用或Shutdown时)
     void GC() override;

    ///后端是否可用不可用时框架退化为纯内存模式不丢任务
     bool IsAvailable() const override { return true; }
private:
    mutable  std::mutex mutex_;
    std::string dir_;

    //不加锁读取(防死锁)
    std::optional<TaskRecord> LoadContent(uint32_t task_id)const;

    //检查文件是否是任务快照
    std::string CheckFileIsRecord(const std::filesystem::directory_entry& entry)const;
}; 

/*
    =================================================
                        接口实现
    =================================================
*/

inline void TaskFilePersistence::GC() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : std::filesystem::directory_iterator(dir_)) {
        auto filename = CheckFileIsRecord(entry);
        if (filename.empty()) continue;
        std::string numPart = filename.substr(5, filename.rfind('.') - 5);
        try {
            uint32_t id = static_cast<uint32_t>(std::stoul(numPart));
            auto rec = LoadContent(id);
            if (rec && rec->IsTerminal()) {
                std::error_code ec;
                std::filesystem::remove(entry.path(), ec);
            }
        }
        catch (...) { /* 忽略 */ }
    }
}

inline std::string TaskFilePersistence::CheckFileIsRecord(const std::filesystem::directory_entry& entry) const {
    if (!entry.is_regular_file()) return {};
    std::string filename = entry.path().filename().string();
    if (filename.rfind("task_", 0) != 0) return {};
    if (filename.rfind('.') == std::string::npos || filename.substr(filename.rfind('.')) != ".dat") return {};
    return filename;
}


inline std::optional<TaskRecord> TaskFilePersistence::LoadContent(uint32_t task_id)const
{
    std::string path = dir_ + "/task_" + std::to_string(task_id) + ".dat";
    std::ifstream rf(path);
    try
    {
        TaskRecord tr;
        tr.task_id = task_id;
        std::string line;
        while (std::getline(rf, line))
        {
            auto eq = line.find("=");
            if (eq == std::string::npos) continue;
            std::string k, v;
            k = line.substr(0, eq);
            v = line.substr(eq + 1);
            if (k == "state")
                tr.state = v;
            else if (k == "shard")
                tr.current_shard = std::stoull(v);
            else if (k == "total")
                tr.declared_total = std::stoull(v);
            else if (k == "created")
                tr.created_at_ms = std::stoll(v);
            else if (k == "scheduled")
                tr.scheduled_at_ms = std::stoll(v);
            else if (k == "retry")
                tr.retry_count = stoi(v);
            else if (k == "max_retries")
                tr.max_retries = stoi(v);
            else if (k == "shards_json")
                tr.shards_json = v;
        }
        return tr;
    }
    catch (...)
    {
        return std::nullopt;
    }
}


inline std::vector<TaskRecord> TaskFilePersistence::LoadAll() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TaskRecord> result;
    for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
        auto filename = CheckFileIsRecord(entry);
        if (filename.empty()) continue;
        std::string numPart = filename.substr(5, filename.rfind('.') - 5);
        try {
            uint32_t id = static_cast<uint32_t>(std::stoul(numPart));
            auto rec = LoadContent(id);
            if (rec && !rec->IsTerminal()) {
                result.push_back(std::move(*rec));
            }
        }
        catch (...) {
            // 忽略无效文件
        }
    }
    return result;
}
inline void TaskFilePersistence::Delete(uint32_t task_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string path = dir_ + "/task_" + std::to_string(task_id) + ".dat";
    std::error_code ec;
    std::filesystem::remove(path, ec);
}



inline void TaskFilePersistence::Save(const TaskRecord& record)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string path = dir_ + "/task_" + std::to_string(record.task_id) + ".dat";
    std::ofstream of(path, std::ios::trunc);
    if (!of) return;
    of << "state=" << record.state << "\n"
        << "shard=" << record.current_shard << "\n"
        << "total=" << record.declared_total << "\n"
        << "created=" << record.created_at_ms << "\n"
        << "scheduled=" << record.scheduled_at_ms << "\n"
        << "retry=" << record.retry_count << "\n"
        << "max_retries=" << record.max_retries << "\n"
        << "shards_json=" << record.shards_json << "\n";
}


inline std::optional<TaskRecord> TaskFilePersistence::Load(uint32_t task_id)const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return LoadContent(task_id);
}
