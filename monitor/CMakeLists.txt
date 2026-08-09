# Monitor process — external dashboard for test_shell
# Uses FTXUI + shared memory (MetricsProtocol.h)

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
    ftxui::dom
    ftxui::screen
    ftxui::component
)

set_target_properties(shell_monitor PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Load simulator for dashboard demo
add_executable(simulate_load simulate.cpp)
target_include_directories(simulate_load PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
set_target_properties(simulate_load PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

