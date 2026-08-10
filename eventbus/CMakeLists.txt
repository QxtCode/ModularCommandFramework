# ============================================================
#  EventBus 共享库 — 跨平台 (.dll / .so / .dylib)
#  单例在此，主程序和插件共享同一 EventBus 实例。
#  EVENTBUS_DLL_EXPORTS → PLATFORM_EXPORT 控制符号可见性。
# ============================================================

add_library(eventbus SHARED
    src/event_bus_dll.cpp
)

target_include_directories(eventbus
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/..   # 根目录 → core/platform/platform.h
)

target_compile_features(eventbus PUBLIC cxx_std_20)

target_compile_definitions(eventbus PRIVATE EVENTBUS_DLL_EXPORTS)
