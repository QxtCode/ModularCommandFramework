/// =================================================================
///  file_system_posix.cpp — POSIX 文件系统实现 (Linux / macOS)
/// =================================================================
///
///  CMake else() 分支编译此文件，_win.cpp 在 POSIX 上被跳过。
///
///  FindFiles:    opendir / readdir / fnmatch
///  FileExists:   access(F_OK)
///  NormalizePath: 字符级替换 \ → /
/// =================================================================

#include "file_system.h"

#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>

namespace platform {

std::vector<std::string> FindFiles(const std::string& dir,
                                   const std::string& pattern) {
    std::vector<std::string> result;

    DIR* dp = opendir(dir.c_str());
    if (!dp) return result;

    struct dirent* entry;
    while ((entry = readdir(dp)) != nullptr) {
        // 跳过 . 和 ..
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;

        // fnmatch: 0 = 匹配成功
        if (fnmatch(pattern.c_str(), entry->d_name, 0) == 0) {
            result.push_back(dir + "/" + entry->d_name);
        }
    }

    closedir(dp);
    return result;
}

bool FileExists(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

}  // namespace platform
