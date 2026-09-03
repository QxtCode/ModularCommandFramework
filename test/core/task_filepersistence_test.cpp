#include <gtest/gtest.h>
#include <filesystem>
#include <cstdlib>
#include "core/TaskFilePersistence.h"  // 替换为你的头文件路径

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() / ("task_test_" + std::to_string(rand()));
        fs::create_directories(path_);
    }
    ~TempDir() { fs::remove_all(path_); }
    std::string Path() const { return path_.string(); }
private:
    fs::path path_;
};

TEST(TaskFilePersistenceTest, SaveAndLoad) {
    TempDir tmp;
    TaskFilePersistence store(tmp.Path());

    TaskRecord r;
    r.task_id = 42;
    r.state = "PAUSED";
    r.current_shard = 3;
    r.shards_json = "[{\"mod\":\"Calc\",\"func\":\"add\"}]";

    store.Save(r);

    auto loaded = store.Load(42);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->task_id, 42u);
    EXPECT_EQ(loaded->state, "PAUSED");
    EXPECT_EQ(loaded->current_shard, 3u);
    EXPECT_EQ(loaded->shards_json, r.shards_json);
}

TEST(TaskFilePersistenceTest, DeleteAndLoad) {
    TempDir tmp;
    std::cout << "目录: " << tmp.Path() << std::endl;
    TaskFilePersistence store(tmp.Path());

    TaskRecord r;
    r.task_id = 99;
    r.state = "RUNNING";
    store.Save(r);

    // 检查文件是否存在
    std::string filePath = tmp.Path() + "/task_99.dat";
    std::cout << "保存后文件是否存在？ " << std::filesystem::exists(filePath) << std::endl;

    store.Delete(99);
    std::cout << "删除后文件是否存在？ " << std::filesystem::exists(filePath) << std::endl;
    EXPECT_FALSE(std::filesystem::exists(filePath));   // 直接检查文件
}

TEST(TaskFilePersistenceTest, RecordGC)
{
    TempDir tmp;
    std::cout << "目录: " << tmp.Path() << std::endl;
    TaskFilePersistence store(tmp.Path());

    TaskRecord r,re,rl;
    r.task_id = 99;
    r.state = "COMPLETED";
    store.Save(r);

    re.task_id = 93;
    re.state = "FAILED";
    store.Save(re);

    rl.task_id = 92;
    rl.state = "RUNNING";
    store.Save(rl);

    std::string filePath99 = tmp.Path() + "/task_99.dat";
    std::cout << "Did the file save successfully? " << std::filesystem::exists(filePath99) << std::endl;

    std::string filePath93 = tmp.Path() + "/task_93.dat";
    std::cout << "Did the file save successfully? " << std::filesystem::exists(filePath93) << std::endl;

    std::string filePath92 = tmp.Path() + "/task_92.dat";
    std::cout << "Did the file save successfully? " << std::filesystem::exists(filePath92) << std::endl;
    store.GC();
    std::cout << "Did the file GC successfully?" << std::filesystem::exists(filePath99) << std::endl;
    std::cout << "Did the file GC successfully?" << std::filesystem::exists(filePath93) << std::endl;
    std::cout << "Did the file GC successfully?" << std::filesystem::exists(filePath92) << std::endl;
    EXPECT_FALSE(std::filesystem::exists(filePath99));
    EXPECT_FALSE(std::filesystem::exists(filePath93));
    EXPECT_TRUE(std::filesystem::exists(filePath92));
}


