# ============================================================
#  shell_monitor — 外部监控面板（独立进程）
# ============================================================
#  通过共享内存（MetricsProtocol.h）读取 MetricsCollector 指标。
#  由 dashboard 命令通过 Process::Launch 启动，不占用 Task 槽位。
#  依赖 FTXUI (v5.0.0, FetchContent, gitee 镜像)。

include(FetchContent)

FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://gitee.com/mirrors/FTXUI.git
    GIT_TAG        v5.0.0
    GIT_SHALLOW    TRUE
)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
FetchContent_MakeAvailable(ftxui)

add_executable(shell_monitor
    main.cpp
)

target_include_directories(shell_monitor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(shell_monitor PRIVATE
    platform_utils
    ftxui::dom
    ftxui::screen
    ftxui::component
    ${PLATFORM_LIBS}
)

set_target_properties(shell_monitor PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Load simulator for dashboard demo
add_executable(simulate_load simulate.cpp)
target_include_directories(simulate_load PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(simulate_load PRIVATE
    platform_utils
    ${PLATFORM_LIBS}
)
set_target_properties(simulate_load PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

