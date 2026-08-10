/// =================================================================
///  shared_library.h — RAII 跨平台动态库加载
/// =================================================================
///
///  替代 LoadLibrary/dlopened 的手动资源管理。
///  用法:
///    auto lib = SharedLibrary::Load("plugins/MyModule.so");
///    if (!lib) { /* 加载失败 */ }
///    auto* create = lib->GetFunction<ModuleBaseObject*()>("CreateModule");
///    auto* module = create();
///
///  设计要点:
///    - RAII: 析构时自动 FreeLibrary/dlclose
///    - 移动语义: 不可拷贝，支持 unique_ptr 转移所有权
///    - 类型安全: GetFunction<T>() 用 reinterpret_cast，调用方保证签名匹配
///    - 导出 C 函数: 跨编译器 ABI 安全（只传裸指针和基础类型）
/// =================================================================

#pragma once
#include <memory>
#include <string>

namespace platform {

class SharedLibrary {
public:
    /// 加载动态库。失败返回 nullptr。
    static std::unique_ptr<SharedLibrary> Load(const std::string& path);

    /// 析构自动释放动态库。
    ~SharedLibrary();

    /// 获取导出符号的原始指针。
    /// 失败返回 nullptr（符号未找到）。
    void* GetRawSymbol(const std::string& name);

    /// 类型安全的符号查找。
    /// 调用方保证 T 与动态库中导出的函数签名一致。
    template<typename T>
    T GetFunction(const std::string& name) {
        return reinterpret_cast<T>(GetRawSymbol(name));
    }

    /// 检查是否加载成功。
    explicit operator bool() const { return handle_ != nullptr; }

    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

private:
    SharedLibrary() = default;

#ifdef _WIN32
    using NativeHandle = void*;  // HMODULE
#else
    using NativeHandle = void*;  // dlopen handle
#endif
    NativeHandle handle_ = nullptr;
};

}  // namespace platform
