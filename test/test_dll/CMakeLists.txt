# 测试用 DLL 模块 — 编译为共享库供 test_runner 加载
# 需要 link eventbus（DLL singleton 在此）

add_library(TestDLLModule SHARED
    TestDLLModule.cpp
)

target_include_directories(TestDLLModule PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../core
)

target_link_libraries(TestDLLModule PRIVATE eventbus)

# 输出到测试运行目录（和 test_runner.exe 一起），不带 lib 前缀
set_target_properties(TestDLLModule PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.."
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.."
)
