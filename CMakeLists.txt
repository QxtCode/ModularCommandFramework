# =================================================================
#  test_shell v2.7 — 跨平台 C++20 命令框架
# =================================================================
#  构建: cmake --preset debug
#        cmake --build --preset debug
#
#  平台: Windows (MSVC) / Linux (GCC/Clang) / macOS (Apple Clang)
#  平台差异集中在 core/platform/，按 _win.cpp / _posix.cpp 编译
# =================================================================

cmake_minimum_required(VERSION 3.16)
project(test_shell
    VERSION 2.7.0
    DESCRIPTION "C++20 跨平台命令框架 — Signal/Slot + Task Shard + 平台抽象层"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 所有二进制输出到同一目录（运行时库路径解析）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# MSVC: UTF-8 源码（中文注释）
if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(Threads REQUIRED)

# ============================================
#  EventBus 共享库（单例在此）
#  跨平台: .dll (Windows) / .so (Linux) / .dylib (macOS)
# ============================================
add_subdirectory(eventbus)
add_subdirectory(monitor)

# ============================================
#  示例插件（演示动态加载）
# ============================================
add_subdirectory(modules/samples)

# ============================================
#  platform_utils — 平台抽象层静态库
#  公共源 + 按平台选择 _win.cpp / _posix.cpp
# ============================================
set(PLATFORM_COMMON
    core/platform/console.cpp
)
if(WIN32)
    set(PLATFORM_IMPL
        core/platform/file_system_win.cpp
        core/platform/shared_library_win.cpp
        core/platform/shared_memory_win.cpp
        core/platform/process_win.cpp
    )
else()
    set(PLATFORM_IMPL
        core/platform/file_system_posix.cpp
        core/platform/shared_library_posix.cpp
        core/platform/shared_memory_posix.cpp
        core/platform/process_posix.cpp
    )
endif()

add_library(platform_utils STATIC
    ${PLATFORM_COMMON}
    ${PLATFORM_IMPL}
 )
target_include_directories(platform_utils PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(platform_utils PUBLIC cxx_std_20)

# ============================================
#  主程序
# ============================================
add_executable(test_shell
    main.cpp
    core/Task.cpp
    parser/CommandParser.cpp
 )

target_include_directories(test_shell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/sdk
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/parser
    ${CMAKE_CURRENT_SOURCE_DIR}/modules
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/logging
    ${CMAKE_CURRENT_SOURCE_DIR}/monitor
)

# POSIX 平台依赖: dl (Linux) / librt (旧 Linux 的 shm_open)
if(NOT WIN32)
    set(PLATFORM_LIBS ${CMAKE_DL_LIBS})
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_library(RT_LIBRARY rt)
        if(RT_LIBRARY)
            list(APPEND PLATFORM_LIBS ${RT_LIBRARY})
        endif()
    endif()
else()
    set(PLATFORM_LIBS "")
endif()

target_link_libraries(test_shell PRIVATE platform_utils eventbus Threads::Threads ${PLATFORM_LIBS})

# 复制配置文件到 exe 同目录（Process::ExeDir() 定位）
add_custom_command(TARGET test_shell POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/test_shell.cfg"
        "$<TARGET_FILE_DIR:test_shell>/test_shell.cfg"
    COMMENT "Copying test_shell.cfg to output directory"
)

# ============================================
#  测试
# ============================================
option(BUILD_TESTING "Build unit tests" ON)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()
