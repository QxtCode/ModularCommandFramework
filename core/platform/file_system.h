/// =================================================================
///  file_system.h — 跨平台文件系统工具
/// =================================================================
///
///  提供:
///    - FindFiles()      — 枚举目录中匹配模式的文件
///    - FileExists()     — 检查文件是否存在
///    - NormalizePath()  — 统一路径分隔符为 '/'
///
///  用法:
///    auto dlls = platform::FindFiles("plugins", "*.dll");
///    if (platform::FileExists("test_shell.cfg")) { ... }
///    std::string path = platform::NormalizePath("a\\b\\c");
/// =================================================================

#pragma once
#include <string>
#include <vector>

namespace platform {

/// 枚举目录中匹配模式的文件，返回完整路径。
/// @param dir     目录路径
/// @param pattern 文件名模式，如 "*.dll"、"*.so"
/// @return        匹配的完整路径列表（dir/文件名）
std::vector<std::string> FindFiles(const std::string& dir,
                                   const std::string& pattern);

/// 检查文件（或目录）是否存在。
bool FileExists(const std::string& path);

/// 将路径中的反斜杠替换为正斜杠。
/// Windows 内核也接受 '/'，所以统一后跨平台无感。
inline std::string NormalizePath(const std::string& path) {
    std::string result = path;
    for (auto& ch : result)
        if (ch == '\\') ch = '/';
    return result;
}

}  // namespace platform
