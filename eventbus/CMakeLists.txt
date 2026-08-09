# EventBus shared library (DLL)
# The singleton lives here so main.exe and plugin DLLs share one instance.

add_library(eventbus SHARED
    src/event_bus_dll.cpp
)

target_include_directories(eventbus
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(eventbus PUBLIC cxx_std_20)

# Export macro
target_compile_definitions(eventbus PRIVATE EVENTBUS_DLL_EXPORTS)
