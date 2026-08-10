/// =================================================================
///  file_system_win.cpp — Windows 文件系统实现
/// =================================================================
///
///  CMake if(WIN32) 分支编译此文件，_posix.cpp 在 Windows 上被跳过。
///
///  FindFiles:    FindFirstFile / FindNextFile
///  FileExists:   GetFileAttributesA
///  NormalizePath: 字符级替换 \ → /
/// =================================================================

#include "file_system.h"

#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

namespace platform {

std::vector<std::string> FindFiles(const std::string& dir,
                                   const std::string& pattern) {
    std::vector<std::string> result;

    std::string search = dir + "\\" + pattern;
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return result;

    do {
        // 跳过 . 和 ..
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;

        std::string path = dir + "/" + fd.cFileName;
        result.push_back(NormalizePath(path));
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    return result;
}

bool FileExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

}  // namespace platform
