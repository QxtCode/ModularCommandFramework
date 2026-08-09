# Sample DLL modules — built as shared libraries for plugin loading
# Included from the root CMakeLists.txt, so CMAKE variables are already set up

add_library(CalculatorModule SHARED
    CalculatorModule.cpp
)

target_include_directories(CalculatorModule PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/core
)

target_link_libraries(CalculatorModule PRIVATE eventbus)

target_compile_features(CalculatorModule PUBLIC cxx_std_20)

# Output to same directory as test_shell.exe for easy loading
set_target_properties(CalculatorModule PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)
