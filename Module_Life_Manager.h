#pragma once
#include "Base_Command.h"
#include "Moduel_Base_Object.h"
#include "ModuleDLL.h"
#include <memory>
#include <iostream>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/*
==========模块生命周期管理器==========

  职责：
    1. 管理所有已注册模块（内置模块 + DLL 动态加载的模块）
    2. 接收参数包，根据 mod_id 找到对应模块/命令并执行
    3. 支持运行时加载 / 卸载 / 热重载 DLL 模块

  用法：
    auto& mgr = ModuleLifeManager::GetInstance();

    // 方式1：注册内置模块（链接进 exe 的）
    mgr.AddModule(std::make_unique<PrintModule>());

    // 方式2：从 DLL 加载模块（运行时动态加载）
    mgr.LoadModuleDLL("modules/PrintModule.dll");

    // 热重载（开发时改了 DLL，不用重启主程序）
    mgr.ReloadModule("modules/PrintModule.dll");
*/

class ModuleLifeManager
{
public:
    ModuleLifeManager() = default;

    // ===== 单例 =====
    static ModuleLifeManager& GetInstance()
    {
        static ModuleLifeManager instance;
        return instance;
    }

    // ========== 模块注册 ==========

    /// 注册一个内置模块（静态链接进主程序的）
    /// @return 成功返回 true，同名/初始化失败返回 false
    bool AddModule(std::unique_ptr<ModuleBaseObject> module)
    {
        if (!module) return false;

        const char* name = module->GetName();
        if (module_map_.find(name) != module_map_.end()) {
            std::cerr << "[ModuleMgr] Module '" << name << "' already registered!" << std::endl;
            return false;
        }

        // 走生命周期：先初始化
        if (!module->OnInit()) {
            std::cerr << "[ModuleMgr] " << name << " OnInit() failed!" << std::endl;
            return false;
        }

        std::cout << "[ModuleMgr] Module loaded: " << name
                  << " v" << module->GetVersion() << std::endl;
        module_map_[name] = std::move(module);
        return true;
    }

    /// 注册一个命令模块（CommandModule 把命令 → 实际模块桥接起来）
    bool AddCommand(const std::string& modulename, std::unique_ptr<BaseCommand> cmd)
    {
        if (modulename.empty() || !cmd) return false;
        if (command_map_.find(modulename) != command_map_.end()) return false;
        command_map_[modulename] = std::move(cmd);
        return true;
    }

    // ========== DLL 动态加载（热加载核心）==========

    /// 从 DLL 文件加载模块
    /// @param dllPath  DLL 文件路径，如 "modules/EmailModule.dll"
    /// @return 成功返回 true
    bool LoadModuleDLL(const std::string& dllPath)
    {
#ifdef _WIN32
        HMODULE handle = LoadLibraryA(dllPath.c_str());
        if (!handle) {
            std::cerr << "[ModuleMgr] LoadLibrary failed: " << dllPath << std::endl;
            return false;
        }

        // 找 DLL 导出的工厂函数
        auto createFunc  = (CreateModuleFunc)GetProcAddress(handle, "CreateModule");
        auto destroyFunc = (DestroyModuleFunc)GetProcAddress(handle, "DestroyModule");

        if (!createFunc || !destroyFunc) {
            std::cerr << "[ModuleMgr] DLL 缺少 CreateModule/DestroyModule 导出!" << std::endl;
            FreeLibrary(handle);
            return false;
        }

        // 用 DLL 内部的 new 创建模块（跨模块堆安全）
        ModuleBaseObject* module = createFunc();
        if (!module) {
            std::cerr << "[ModuleMgr] CreateModule() 返回 null: " << dllPath << std::endl;
            FreeLibrary(handle);
            return false;
        }

        const char* name = module->GetName();

        // 同名模块 → 先卸载旧的
        if (module_map_.find(name) != module_map_.end()) {
            std::cout << "[ModuleMgr] Replacing existing module: " << name << std::endl;
            RemoveModuleInternal(name);
        }

        // 存入模块表
        DLLEntry entry;
        entry.handle      = handle;
        entry.destroyFunc = destroyFunc;
        entry.dllPath     = dllPath;

        module_map_[name].reset(module);   // 接管裸指针
        dll_entries_[name] = entry;

        std::cout << "[ModuleMgr] DLL loaded: " << name
                  << " v" << module->GetVersion()
                  << " (" << dllPath << ")" << std::endl;
        return true;
#else
        // Linux / macOS：dlopen
        void* handle = dlopen(dllPath.c_str(), RTLD_LAZY);
        if (!handle) {
            std::cerr << "[ModuleMgr] dlopen failed: " << dllPath << " - " << dlerror() << std::endl;
            return false;
        }
        auto createFunc  = (CreateModuleFunc)dlsym(handle, "CreateModule");
        auto destroyFunc = (DestroyModuleFunc)dlsym(handle, "DestroyModule");
        if (!createFunc || !destroyFunc) { dlclose(handle); return false; }

        ModelBaseObject* module = createFunc();
        if (!module) { dlclose(handle); return false; }

        const char* name = module->GetName();
        if (module_map_.find(name) != module_map_.end()) RemoveModuleInternal(name);

        DLLEntry entry;
        entry.linuxHandle = handle;
        entry.destroyFunc = destroyFunc;
        entry.dllPath     = dllPath;
        module_map_[name].reset(module);
        dll_entries_[name] = entry;
        return true;
#endif
    }

    /// 卸载指定模块
    bool UnloadModule(const std::string& moduleName)
    {
        return RemoveModuleInternal(moduleName);
    }

    /// 热重载 DLL 模块（先卸载旧 DLL，再加载新的）
    bool ReloadModule(const std::string& dllPath)
    {
        std::cout << "[ModuleMgr] Hot-reloading: " << dllPath << std::endl;
        return LoadModuleDLL(dllPath);   // LoadModuleDLL 内部处理同名替换
    }

    // ========== 命令分派 ==========

    /// 打印所有已注册模块
    void PrintAllCommands()
    {
        std::cout << "\n===== Registered Commands =====" << std::endl;
        for (const auto& [name, cmd] : command_map_) {
            std::cout << "  [CMD] " << name << std::endl;
        }
        std::cout << "===== Loaded Modules ==========" << std::endl;
        for (const auto& [name, mod] : module_map_) {
            std::cout << "  [MOD] " << name << " v" << mod->GetVersion() << std::endl;
        }
        std::cout << "===============================" << std::endl;
    }

    /// 接收参数包，分派到对应模块执行
    bool Dispatch(ParmarPack* pack)
    {
        if (!pack) return false;

        // help / 空命令 → 打印所有模块
        if (pack->mod_id.empty() || pack->mod_id == "help") {
            PrintAllCommands();
            return true;
        }

        // 优先走命令模块（c_printmodule 这类）
        auto cmdIt = command_map_.find(pack->mod_id);
        if (cmdIt != command_map_.end()) {
            cmdIt->second->Execute(pack);
            return true;
        }

        // 直接找模块（DLL 模块直接用模块名调用）
        auto modIt = module_map_.find(pack->mod_id);
        if (modIt != module_map_.end()) {
            if (pack->func_id == "help" || pack->func_id.empty()) {
                modIt->second->Help(pack);
            } else {
                modIt->second->Execute(pack);
            }
            return true;
        }

        pack->success = false;
        pack->error.message = "Module not found: " + pack->mod_id;
        return false;
    }

    // ---- 禁止拷贝 / 移动 ----
    ModuleLifeManager(const ModuleLifeManager&) = delete;
    ModuleLifeManager& operator=(const ModuleLifeManager&) = delete;
    ModuleLifeManager(ModuleLifeManager&&) = delete;
    ModuleLifeManager& operator=(ModuleLifeManager&&) = delete;

private:
    // DLL 加载信息
    struct DLLEntry {
#ifdef _WIN32
        HMODULE handle = nullptr;
#else
        void* linuxHandle = nullptr;
#endif
        DestroyModuleFunc destroyFunc = nullptr;
        std::string dllPath;
    };

    /// 内部：卸载并清理模块
    bool RemoveModuleInternal(const std::string& name)
    {
        auto modIt = module_map_.find(name);
        if (modIt == module_map_.end()) return false;

        auto dllIt = dll_entries_.find(name);

        std::cout << "[ModuleMgr] Unloading: " << name << std::endl;

        if (dllIt != dll_entries_.end()) {
            // DLL 模块：必须用 DLL 内部的 DestroyModule 释放
            //（因为对象是 DLL 的 new 创建的，跨模块堆不能混用 delete）
            dllIt->second.destroyFunc(modIt->second.release());
#ifdef _WIN32
            FreeLibrary(dllIt->second.handle);
#else
            dlclose(dllIt->second.linuxHandle);
#endif
            dll_entries_.erase(dllIt);
        }
        // 内置模块：unique_ptr 自动析构，走基类虚析构

        module_map_.erase(modIt);
        std::cout << "[ModuleMgr] Unloaded: " << name << std::endl;
        return true;
    }

    std::unordered_map<std::string, std::unique_ptr<BaseCommand>>    command_map_;
    std::unordered_map<std::string, std::unique_ptr<ModuleBaseObject>> module_map_;
    std::unordered_map<std::string, DLLEntry>                         dll_entries_;
};
