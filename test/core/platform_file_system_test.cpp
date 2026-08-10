/// platform::file_system 独立测试
/// 覆盖: FindFiles, FileExists, NormalizePath

#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <sys/stat.h>
    #include <unistd.h>
#endif

#include "core/platform/file_system.h"

using namespace platform;

// ================================================================
//  NormalizePath
// ================================================================
TEST(PlatformFileSystem, NormalizePath_NoChange) {
    EXPECT_EQ(NormalizePath("a/b/c"), "a/b/c");
}

TEST(PlatformFileSystem, NormalizePath_BackslashesToForward) {
    EXPECT_EQ(NormalizePath("a\\b\\c"), "a/b/c");
}

TEST(PlatformFileSystem, NormalizePath_MixedSeparators) {
    EXPECT_EQ(NormalizePath("a\\b/c"), "a/b/c");
}

TEST(PlatformFileSystem, NormalizePath_Empty) {
    EXPECT_EQ(NormalizePath(""), "");
}

TEST(PlatformFileSystem, NormalizePath_SingleBackslash) {
    EXPECT_EQ(NormalizePath("\\"), "/");
}

// ================================================================
//  FileExists
// ================================================================
TEST(PlatformFileSystem, FileExists_NonExistent) {
    EXPECT_FALSE(FileExists("__nonexistent_file_12345__.txt"));
}

TEST(PlatformFileSystem, FileExists_EmptyPath) {
    EXPECT_FALSE(FileExists(""));
}

// ================================================================
//  FindFiles
// ================================================================
class PlatformFileSystemDirTest : public ::testing::Test {
protected:
    std::string test_dir;

    void SetUp() override {
        test_dir = "test_platform_fs_tmp";

        // 清理可能残留的目录
        CleanupDir();

        // 创建测试目录
#ifdef _WIN32
        CreateDirectoryA(test_dir.c_str(), nullptr);
#else
        mkdir(test_dir.c_str(), 0755);
#endif

        // 创建测试文件
        CreateFile("a.dll");
        CreateFile("b.dll");
        CreateFile("c.so");
        CreateFile("readme.txt");
        CreateFile(".hidden");
    }

    void TearDown() override {
        CleanupDir();
    }

private:
    void CreateFile(const std::string& name) {
        std::string path = test_dir + "/" + name;
        std::ofstream f(path);
        f << "test";
        f.close();
    }

    void CleanupDir() {
        // 删除所有文件和目录
        for (const auto& f : FindFiles(test_dir, "*"))
            std::remove(f.c_str());
        for (const auto& f : FindFiles(test_dir, ".*"))
            std::remove(f.c_str());
#ifdef _WIN32
        RemoveDirectoryA(test_dir.c_str());
#else
        rmdir(test_dir.c_str());
#endif
    }
};

TEST_F(PlatformFileSystemDirTest, FindFiles_DllPattern) {
    auto files = FindFiles(test_dir, "*.dll");
    EXPECT_EQ(files.size(), 2u);

    // 路径格式统一
    for (const auto& f : files) {
        EXPECT_TRUE(f.find('\\') == std::string::npos);
        EXPECT_TRUE(f.find(test_dir + "/") == 0);
    }
}

TEST_F(PlatformFileSystemDirTest, FindFiles_SoPattern) {
    auto files = FindFiles(test_dir, "*.so");
    EXPECT_EQ(files.size(), 1u);
    EXPECT_TRUE(files[0].find("c.so") != std::string::npos);
}

TEST_F(PlatformFileSystemDirTest, FindFiles_AllPattern) {
    auto files = FindFiles(test_dir, "*");
    // 应该找到 5 个普通文件（不包含 . 和 ..）
    EXPECT_EQ(files.size(), 5u);
}

TEST_F(PlatformFileSystemDirTest, FindFiles_NoMatch) {
    auto files = FindFiles(test_dir, "*.exe");
    EXPECT_EQ(files.size(), 0u);
}

TEST_F(PlatformFileSystemDirTest, FindFiles_NonExistentDir) {
    auto files = FindFiles("__nonexistent_dir__", "*");
    EXPECT_EQ(files.size(), 0u);
}

TEST_F(PlatformFileSystemDirTest, FileExists_RealFile) {
    EXPECT_TRUE(FileExists(test_dir + "/a.dll"));
}

TEST_F(PlatformFileSystemDirTest, FileExists_DirectoryExists) {
    EXPECT_TRUE(FileExists(test_dir));
}
