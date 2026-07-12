cmake_minimum_required(VERSION 3.16)
project(test_shell
    VERSION 1.0
    DESCRIPTION "C++ 命令解析执行框架"
    LANGUAGES CXX
)

# 使用 C++17（你用了 std::any）
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 开启多线程支持（你用了 std::mutex / condition_variable）
find_package(Threads REQUIRED)

# 源文件
add_executable(test_shell
    main.cpp
    console_parser.cpp
    friend_pm.cpp
    print_module.cpp
    task.cpp
    tasks_pool.cpp
)

# 头文件目录（让 #include 能找到）
target_include_directories(test_shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 链接线程库
target_link_libraries(test_shell PRIVATE Threads::Threads)
