# ============================================================
#  test_runner — 单元测试可执行文件
# ============================================================
#  使用 FetchContent 自动下载 GoogleTest (v1.15.2, 从 gitee 镜像)。
#  FETCHCONTENT_UPDATES_DISCONNECTED=ON 避免重复下载。
#
#  测试 DLL 自动拷贝: POST_BUILD 将 eventbus.dll + TestDLLModule.dll
#  复制到 test_runner.exe 同目录，测试中通过 CWD 定位。
#  回退路径 out/build/debug（匹配 CMakePresets.json binaryDir）。

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://gitee.com/mirrors/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON)  # once downloaded, never re-fetch
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# ============================================
#  Test executable (all sources needed for linking)
# ============================================
add_subdirectory(test_dll)

add_executable(test_runner
    main.cpp
    ../core/Task.cpp
    ../parser/CommandParser.cpp
    ../parser/ConsoleParser.cpp
    core/task_test.cpp
    core/tasks_pool_test.cpp
    core/module_life_manager_test.cpp
    core/dll_module_test.cpp
    parser/command_parser_test.cpp
    parser/console_parser_test.cpp
    eventbus/event_bus_test.cpp
    modules/log_module_test.cpp
    core/func_explain_test.cpp
    core/thread_pool_test.cpp
    core/result_formatter_test.cpp
    core/integration_stress_test.cpp
    core/concurrency_stress_test.cpp
    core/dll_lifecycle_stress_test.cpp
    core/stress_60s_test.cpp
    core/predicate_wait_test.cpp
    core/stress_gradual_test.cpp
    core/idle_resource_test.cpp
    core/task_pause_test.cpp
    core/task_progress_test.cpp
    core/shm_protocol_test.cpp
    core/monitor_integration_test.cpp
    core/cpu_metrics_test.cpp
    core/monitor_logic_test.cpp
    core/input_thread_test.cpp
    core/result_store_test.cpp
    core/shell_engine_test.cpp
    core/peak_stress_test.cpp
    core/task_store_test.cpp
    sdk/imodule_test.cpp
    core/platform_file_system_test.cpp
    core/platform_shared_library_test.cpp
    core/platform_shared_memory_test.cpp
    core/platform_process_test.cpp
    core/platform_boundary_test.cpp
)

target_include_directories(test_runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk
    ${CMAKE_CURRENT_SOURCE_DIR}/../core
    ${CMAKE_CURRENT_SOURCE_DIR}/../parser
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/logging
    ${CMAKE_CURRENT_SOURCE_DIR}/../monitor
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(test_runner PRIVATE
    platform_utils
    eventbus
    GTest::gtest
    GTest::gtest_main
    Threads::Threads
    psapi
    ${PLATFORM_LIBS}
)

target_compile_features(test_runner PUBLIC cxx_std_20)

# 确保测试 DLL 在 test_runner 之前构建
add_dependencies(test_runner TestDLLModule)

# 自动拷贝 eventbus.dll 到测试运行目录
add_custom_command(TARGET test_runner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:eventbus>
        $<TARGET_FILE_DIR:test_runner>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:TestDLLModule>
        $<TARGET_FILE_DIR:test_runner>
    COMMENT "Copying eventbus.dll + TestDLLModule.dll → test dir"
)

include(GoogleTest)
gtest_discover_tests(test_runner
    PROPERTIES
        LABELS "test_shell"
        DISCOVERY_MODE PRE_TEST
    WORKING_DIRECTORY $<TARGET_FILE_DIR:test_runner>
)
