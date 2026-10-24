# =================================================================
#  test_shell v2.8 — 跨平台 C++20 命令框架
# =================================================================
#  构建: cmake --preset debug
#        cmake --build --preset debug
#
#  平台: Windows (MSVC) / Linux (GCC/Clang) / macOS (Apple Clang)
#  平台差异集中在 core/platform/，按 _win.cpp / _posix.cpp 编译
#
#  作为子项目引入（下游产品仓库）：
#    FetchContent / add_subdirectory 引入本仓库时，只构建库目标
#    (eventbus / test_shell_sdk / platform_utils / test_shell_core)，
#    不构建 CLI exe / 示例插件 / FTXUI 监控 / GoogleTest 测试，
#    也不会下载它们的依赖或覆盖宿主的输出目录设置。
# =================================================================

cmake_minimum_required(VERSION 3.16)
project(test_shell
    VERSION 2.8.0
    DESCRIPTION "C++20 跨平台命令框架 — Signal/Slot + Task Shard + 平台抽象层"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC: UTF-8 源码（中文注释）
if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(Threads REQUIRED)

# ---- 子项目守卫 ----
# 顶层构建（本仓库自己就是主工程）→ 全量目标；
# 被 FetchContent/add_subdirectory 引入 → 只建库，宿主不受影响。
if(PROJECT_NAME STREQUAL CMAKE_PROJECT_NAME)
    set(TEST_SHELL_TOPLEVEL ON)
else()
    set(TEST_SHELL_TOPLEVEL OFF)
endif()

# 输出目录只在顶层设置（子项目模式不覆盖宿主的输出目录约定）
if(TEST_SHELL_TOPLEVEL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

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

# ============================================
#  EventBus 共享库（单例在此）
#  跨平台: .dll (Windows) / .so (Linux) / .dylib (macOS)
#  核心依赖 —— CLI / UI / 测试都要，任何模式都构建。
# ============================================
add_subdirectory(eventbus)

# ============================================
#  test_shell_sdk — 纯头文件 SDK（INTERFACE 库）
#  模块作者只需链接它，无需 eventbus / core。
#  EXPORT_MODULE 已在 sdk/Export.h 中，自包含零 core 依赖。
# ============================================
add_library(test_shell_sdk INTERFACE)
target_include_directories(test_shell_sdk INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}          # 使 "sdk/..." 可被 include
    ${CMAKE_CURRENT_SOURCE_DIR}/sdk      # 使 "sdk.h" 等可直接 include
)
target_compile_features(test_shell_sdk INTERFACE cxx_std_20)

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
  "core/TaskFilePersistence.h")
target_include_directories(platform_utils PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(platform_utils PUBLIC cxx_std_20)

# ============================================
#  test_shell_core — 引擎核心静态库
#  ShellEngine / ShellApp / ModuleLifeManager / TaskFilePersistence
#  等全是头文件实现，真正要编译的 .cpp 只有 Task + CommandParser。
#  CLI(test_shell)、UI(test_shell_ui，在下游产品仓库)、测试共享它，
#  不再各自把核心源码编一遍（ui/CMakeLists 旧注释里规划的
#  「正式阶段抽 core 库共用」就是这一步）。
# ============================================
add_library(test_shell_core STATIC
    core/Task.cpp
    parser/CommandParser.cpp
)
target_include_directories(test_shell_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/sdk
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/parser
    ${CMAKE_CURRENT_SOURCE_DIR}/modules
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/logging
    ${CMAKE_CURRENT_SOURCE_DIR}/monitor
)
target_link_libraries(test_shell_core PUBLIC
    platform_utils
    eventbus
    Threads::Threads
    ${PLATFORM_LIBS}
)
target_compile_features(test_shell_core PUBLIC cxx_std_20)

# ============================================
#  以下目标只在顶层构建（子项目模式跳过：
#  下游不需要 CLI exe / 示例插件 / 监控面板 / 测试）
# ============================================
if(TEST_SHELL_TOPLEVEL)

    # 示例插件（演示动态加载）
    add_subdirectory(modules/samples)

    # ============================================
    #  主程序
    # ============================================
    add_executable(test_shell main.cpp)

    target_link_libraries(test_shell PRIVATE test_shell_core)

    # 复制配置文件到 exe 同目录（Process::ExeDir() 定位）
    add_custom_command(TARGET test_shell POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/test_shell.cfg"
            "$<TARGET_FILE_DIR:test_shell>/test_shell.cfg"
        COMMENT "Copying test_shell.cfg to output directory"
    )

    # FTXUI 监控面板 + 负载模拟器（FetchContent 拉依赖，仅顶层）
    add_subdirectory(monitor)

    # ============================================
    #  测试
    # ============================================
    option(BUILD_TESTING "Build unit tests" ON)

    if(BUILD_TESTING)
        enable_testing()
        add_subdirectory(test)
    endif()

endif()
