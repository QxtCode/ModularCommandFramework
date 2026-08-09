cmake_minimum_required(VERSION 3.16)
project(test_shell
    VERSION 2.0.0
    DESCRIPTION "C++ 命令解析执行框架 — Signal/Slot + Task Shard + DLL Injection"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Place DLLs and EXEs in the same output directory so the linker can find them at runtime
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# MSVC: treat all source files as UTF-8 (Chinese comments in the code)
if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(Threads REQUIRED)

# ============================================
#  EventBus DLL (singleton lives here)
# ============================================
add_subdirectory(eventbus)
add_subdirectory(monitor)

# ============================================
#  Sample DLL modules (for plugin loading demonstration)
# ============================================
add_subdirectory(modules/samples)

# ============================================
#  Main executable
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
target_link_libraries(test_shell PRIVATE eventbus Threads::Threads psapi)

# ============================================
#  Tests
# ============================================
option(BUILD_TESTING "Build unit tests" ON)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()
