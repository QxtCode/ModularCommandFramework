# Sample DLL modules — built as shared libraries for plugin loading
# Included from the root CMakeLists.txt, so CMAKE variables are already set up

add_library(CalculatorModule SHARED
    CalculatorModule.cpp
)

# 纯 SDK 模块：只链 test_shell_sdk（纯头文件，不链 eventbus）
target_link_libraries(CalculatorModule PRIVATE test_shell_sdk)

target_compile_features(CalculatorModule PUBLIC cxx_std_20)

# Output to same directory as test_shell.exe for easy loading
set_target_properties(CalculatorModule PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)
