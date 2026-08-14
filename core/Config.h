/// =================================================================
///  ShellConfig — 运行时配置
/// =================================================================
///
///  优先级：命令行参数 > 配置文件 > 默认值
///
///  INI 配置文件格式（test_shell.cfg）：
///    pool_size  = 16
///    workers    = 8
///    plugin_dir = ./my_plugins
///    log_level  = DEBUG
///
///  路径解析由调用方负责。main.cpp 使用 platform::Process::ExeDir()
///  定位 exe 同目录的配置文件，CMake POST_BUILD 自动将其拷贝到输出目录。
///  这样无论从哪个工作目录启动，配置文件都能被正确找到。
///
///  # 开头是注释，空行忽略。
///  命令行覆盖用 --key=value 语法（优先级高于配置文件）。
/// =================================================================

#pragma once
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

struct ShellConfig {
    // ---- 引擎 ----
    int  pool_size = 8;       // Task 池槽位数
    int  workers   = 4;       // 工作线程数

    // ---- 路径 ----
    std::string plugin_dir = "plugins";

    // ---- 日志 ----
    std::string log_level = "info";   // debug | info | error | fatal

    // ---- 告警 ----
    int  queue_warn_threshold = 0;    // 队列积压告警阈值，0 = 不告警

    // ================================================================
    //  从 INI 文件加载
    // ================================================================
    bool LoadFromFile(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "[Config] 无法打开: " << path
                      << "（使用默认值）" << std::endl;
            return false;
        }

        std::string line;
        int lineno = 0;
        while (std::getline(file, line)) {
            lineno++;

            // 去首尾空白
            size_t start = line.find_first_not_of(" \t\r");
            if (start == std::string::npos) continue;
            size_t end = line.find_last_not_of(" \t\r");
            line = line.substr(start, end - start + 1);

            // 注释或空行
            if (line.empty() || line[0] == '#') continue;

            // key = value
            size_t eq = line.find('=');
            if (eq == std::string::npos) {
                std::cerr << "[Config] 第 " << lineno
                          << " 行缺少 '='，已忽略: " << line << std::endl;
                continue;
            }

            std::string key = Trim(line.substr(0, eq));
            std::string val = Trim(line.substr(eq + 1));
            if (key.empty() || val.empty()) continue;

            Apply(key, val);
        }
        return true;
    }

    // ================================================================
    //  命令行覆盖（--key=value）
    // ================================================================
    void ApplyArgs(int argc, char* argv[]) {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg.compare(0, 2, "--") != 0) continue;
            arg = arg.substr(2);

            size_t eq = arg.find('=');
            if (eq == std::string::npos) continue;

            std::string key = arg.substr(0, eq);
            std::string val = arg.substr(eq + 1);
            Apply(key, val);
        }
    }

    // ================================================================
    //  打印当前配置
    // ================================================================
    void Print() const {
        std::cout << "[Config] pool_size=" << pool_size
                  << " workers=" << workers
                  << " plugin_dir=" << plugin_dir
                  << " log_level=" << log_level
                  << " queue_warn=" << queue_warn_threshold
                  << std::endl;
    }

private:
    void Apply(const std::string& key, const std::string& val) {
        if (key == "pool_size")           pool_size = std::atoi(val.c_str());
        else if (key == "workers")        workers   = std::atoi(val.c_str());
        else if (key == "plugin_dir")     plugin_dir = val;
        else if (key == "log_level")      log_level  = val;
        else if (key == "queue_warn")     queue_warn_threshold = std::atoi(val.c_str());
        else
            std::cerr << "[Config] 未知配置项: " << key << std::endl;
    }

    
    static std::string Trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r");
        return s.substr(a, b - a + 1);
    }
};
